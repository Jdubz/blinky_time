"""BLE-DFU retry re-enters the bootloader when the entry command didn't take (#218).

Observed 2026-05-20: a flash failed with "Bootloader not found after 3 scan
attempts" while the device stayed in its app. Retries assumed the device was
already in the bootloader, so every retry rescanned for a bootloader that was
never there. Now a retry after a scan miss re-sends `bootloader ble` if the
device is still advertising as its app — and only then.
"""

from __future__ import annotations

import asyncio
from typing import Any

import pytest

from blinky_server.firmware import _guard
from blinky_server.firmware import ble_dfu as mod

APP = "AA:BB:CC:DD:EE:10"
NOT_FOUND = {"status": "error", "message": "not found", "bootloader_not_found": True}
OK = {"status": "ok", "message": "transfer ok", "erase_started": True}
# START_DFU written, then a later stage failed: the app may be partly erased.
ERASED_FAIL = {"status": "error", "message": "write failed", "erase_started": True}
# START_DFU explicitly rejected: bootloader reset, app intact (no erase).
INVALID_STATE = {
    "status": "error",
    "message": "INVALID_STATE",
    "needs_retry": True,
    "erase_started": False,
}


@pytest.fixture
def harness(monkeypatch: pytest.MonkeyPatch) -> dict[str, Any]:
    state: dict[str, Any] = {"transfers": [], "app_visible": [], "entries": [], "scans": []}

    real_sleep = asyncio.sleep
    monkeypatch.setattr(mod.asyncio, "sleep", lambda s: real_sleep(0))
    monkeypatch.setattr(mod, "parse_dfu_zip", lambda path: (b"init", b"firmware", 0x04))
    monkeypatch.setattr(mod, "clear_bluez_state", lambda *a, **k: None)
    monkeypatch.setattr(mod, "_reset_hci_adapter", lambda: None)

    async def preflight(addr: str, progress: Any) -> tuple[bool, str]:
        return True, "ok"

    monkeypatch.setattr(mod, "_preflight_ble_check", preflight)

    async def transfer(**kw: Any) -> dict[str, Any]:
        return dict(state["transfers"].pop(0))

    monkeypatch.setattr(mod, "_dfu_transfer", transfer)

    class Scanner:
        @staticmethod
        async def find_device_by_address(addr: str, timeout: float = 0) -> object | None:
            state["scans"].append(addr)
            # Retry probes (before the transfer succeeds) consume app_visible;
            # post-DFU verification afterwards sees the rebooted app.
            if state["app_visible"]:
                return object() if state["app_visible"].pop(0) else None
            return object()

        @staticmethod
        async def discover(timeout: float = 0, return_adv: bool = False) -> dict[str, Any]:
            return {}

    monkeypatch.setattr(mod, "BleakScanner", Scanner)
    return state


async def _run(state: dict[str, Any], via: str = "ble") -> dict[str, Any]:
    async def enter(cmd: str) -> None:
        state["entries"].append(cmd)

    kwargs: dict[str, Any] = {}
    if via == "ble":
        kwargs["enter_bootloader_via_ble"] = enter
    elif via == "serial":
        kwargs["enter_bootloader_via_serial"] = enter
    # via == "none": DFU-recovery path, device already in its bootloader.
    token = _guard._inside_flash_job_orchestrator.set(True)
    try:
        return await mod._ble_dfu_write_impl(APP, "fw.zip", **kwargs)
    finally:
        _guard._inside_flash_job_orchestrator.reset(token)


async def test_reenters_when_device_still_in_app(harness: dict[str, Any]) -> None:
    harness["transfers"] = [NOT_FOUND, OK]
    harness["app_visible"] = [True]  # retry probe: still advertising as the app
    result = await _run(harness)
    assert result["status"] == "ok"
    assert harness["entries"] == ["bootloader ble", "bootloader ble"]


async def test_no_reentry_when_app_gone(harness: dict[str, Any]) -> None:
    # Device left the app (it is in the bootloader, just not seen yet): don't
    # send the command again, just rescan.
    harness["transfers"] = [NOT_FOUND, OK]
    harness["app_visible"] = [False]
    result = await _run(harness)
    assert result["status"] == "ok"
    assert harness["entries"] == ["bootloader ble"]


async def test_no_reentry_after_transfer_failure(harness: dict[str, Any]) -> None:
    # A failure after START_DFU was written (erase may have begun) must not
    # trigger re-entry: the device is in the bootloader.
    harness["transfers"] = [ERASED_FAIL, OK]
    result = await _run(harness)
    assert result["status"] == "ok"
    assert harness["entries"] == ["bootloader ble"]
    assert APP not in harness["scans"][:-1]  # no app probe before verification


async def test_no_reentry_once_any_transfer_started(harness: dict[str, Any]) -> None:
    # Copilot review: a transfer-stage failure followed by a scan miss must
    # not re-enable re-entry, even with the app advertising.
    harness["transfers"] = [ERASED_FAIL, NOT_FOUND, OK]
    harness["app_visible"] = [True, True]
    result = await _run(harness)
    assert result["status"] == "ok"
    assert harness["entries"] == ["bootloader ble"]


async def test_reentry_after_invalid_state(harness: dict[str, Any]) -> None:
    # INVALID_STATE resets the bootloader without erasing; the device can come
    # back up in its (intact) app. A following scan miss with the app
    # advertising must re-send the entry command, or #218 recurs.
    harness["transfers"] = [INVALID_STATE, NOT_FOUND, OK]
    harness["app_visible"] = [True]
    result = await _run(harness)
    assert result["status"] == "ok"
    assert harness["entries"] == ["bootloader ble", "bootloader ble"]


async def test_reentry_via_serial(harness: dict[str, Any]) -> None:
    harness["transfers"] = [NOT_FOUND, OK]
    harness["app_visible"] = [True]
    result = await _run(harness, via="serial")
    assert result["status"] == "ok"
    assert harness["entries"] == ["bootloader ble", "bootloader ble"]


async def test_recovery_path_never_probes_or_enters(harness: dict[str, Any]) -> None:
    # No entry callables (device already in DFU recovery): no app probe and no
    # entry command, even after a scan miss.
    harness["transfers"] = [NOT_FOUND, OK]
    result = await _run(harness, via="none")
    assert result["status"] == "ok"
    assert harness["entries"] == []
    assert harness["scans"] == [APP]  # only the post-DFU verification scan


async def test_attempts_stay_bounded(harness: dict[str, Any]) -> None:
    harness["transfers"] = [NOT_FOUND] * mod.MAX_DFU_ATTEMPTS
    harness["app_visible"] = [True] * (mod.MAX_DFU_ATTEMPTS - 1)
    result = await _run(harness)
    assert result["status"] == "error"
    assert len(harness["entries"]) == mod.MAX_DFU_ATTEMPTS
    assert harness["transfers"] == []
