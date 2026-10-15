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
OK = {"status": "ok", "message": "transfer ok"}


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


async def _run(state: dict[str, Any]) -> dict[str, Any]:
    async def enter(cmd: str) -> None:
        state["entries"].append(cmd)

    token = _guard._inside_flash_job_orchestrator.set(True)
    try:
        return await mod._ble_dfu_write_impl(APP, "fw.zip", enter_bootloader_via_ble=enter)
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
    # A failure after the bootloader was found (transfer started) must not
    # trigger re-entry: the device is in the bootloader.
    harness["transfers"] = [{"status": "error", "message": "write failed"}, OK]
    result = await _run(harness)
    assert result["status"] == "ok"
    assert harness["entries"] == ["bootloader ble"]
    assert APP not in harness["scans"][:-1]  # no app probe before verification


async def test_attempts_stay_bounded(harness: dict[str, Any]) -> None:
    harness["transfers"] = [NOT_FOUND] * mod.MAX_DFU_ATTEMPTS
    harness["app_visible"] = [True] * (mod.MAX_DFU_ATTEMPTS - 1)
    result = await _run(harness)
    assert result["status"] == "error"
    assert len(harness["entries"]) == mod.MAX_DFU_ATTEMPTS
    assert harness["transfers"] == []
