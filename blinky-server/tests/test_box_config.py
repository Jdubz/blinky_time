"""The box's chips take their device config from the box's own entries (testing/box_config.py).

On a --staging server only, and only for a chip enrolled in box_chips.json: compare first, upload only when
the stored config differs, reboot once to apply it when the chip has been up past markStable
and nothing uses it, and never twice for the same entry.
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Any

import pytest

from blinky_server.device.device import DeviceState
from blinky_server.testing import box_config, leds

GRID = "0123456789ABCDEF"  # made up; the box's real serials are in its local config only
ENTRY = {
    "deviceId": "grid",
    "deviceName": "Grid",
    "ledWidth": 8,
    "ledHeight": 8,
    "ledPin": 10,
    "ledType": 82,
    "orientation": 5,
    "layoutType": 0,
    "brightness": 0,
}


def full_config(entry: dict[str, Any]) -> dict[str, Any]:
    """What `device show` prints once `entry` is stored: every field, defaults filled in."""
    return {**box_config.FIRMWARE_DEFAULTS, **entry}


class FakeProtocol:
    def __init__(self, chip: FakeChip) -> None:
        self.chip = chip

    async def send_command(self, command: str, timeout: float | None = None) -> str:
        self.chip.sent.append(command)
        return self.chip.reply(command)


class FakeChip:
    """A chip with a stored config (None: unconfigured) and the one it booted with."""

    def __init__(self, dev_id: str, stored: dict[str, Any] | None, millis: int = 600_000):
        self.id = dev_id
        self.hardware_sn = dev_id
        self.platform = "nrf52840"
        self.state = DeviceState.CONNECTED
        self.stored = stored
        self.running = stored
        self.millis = millis
        self.sent: list[str] = []
        self.protocol = FakeProtocol(self)
        self.last_info = self.info()

    def info(self) -> dict[str, Any]:
        r = self.running
        dev = (
            {"configured": True, "id": r["deviceId"], "width": r["ledWidth"]}
            | {"height": r["ledHeight"]}
            if r
            else {"configured": False}
        )
        return {"version": "b300", "millis": self.millis, "device": dev}

    def reply(self, command: str) -> str:
        if command == "device show":
            if self.stored is None:
                return '{"error":"No device config","status":"unconfigured","safeMode":true}\n'
            return json.dumps(self.stored, indent=2) + "\n"
        if command.startswith("device upload "):
            self.stored = full_config(json.loads(command[len("device upload ") :]))
            return "✓ Device config saved to flash\n\n**REBOOT DEVICE TO APPLY CONFIGURATION**\n"
        if command == "json info":
            return json.dumps(self.info()) + "\n"
        if command == "reboot":
            self.running, self.millis = self.stored, 0
            return "Rebooting...\n"
        raise AssertionError(command)


class FakeFleet:
    def __init__(self, staging: bool = True) -> None:
        self.staging_enabled = staging

    def get_flash_job(self, device_id: str) -> None:
        return None


@pytest.fixture(autouse=True)
def _state(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    config = tmp_path / "devices"
    config.mkdir()
    (config / "box_chips.json").write_text(json.dumps({"_comment": "test", GRID: "grid"}))
    (config / "grid.json").write_text(json.dumps(ENTRY))
    monkeypatch.setattr(box_config, "CONFIG_DIR", config)
    monkeypatch.setattr(box_config, "state_path", lambda: tmp_path / "box-config.json")
    monkeypatch.setattr(box_config, "MIN_UPTIME_S", 0.0)
    box_config._pending.clear()


async def connect(chip: FakeChip, fleet: FakeFleet | None = None) -> None:
    """A connect, then the pending reboot (if any) run to its end."""
    await box_config.reconcile(fleet or FakeFleet(), chip)  # type: ignore[arg-type]
    task = box_config._pending.get(chip.id)
    if task is not None:
        await asyncio.wait_for(task, 5)


async def test_a_matching_chip_costs_one_device_show() -> None:
    chip = FakeChip(GRID, full_config(ENTRY))
    await connect(chip)
    assert chip.sent == ["device show"]


async def test_nothing_off_the_box_or_for_a_chip_without_an_entry() -> None:
    off = FakeChip(GRID, None)
    await connect(off, FakeFleet(staging=False))
    stranger = FakeChip("02:00:5E:10:00:02", None)
    await connect(stranger)
    assert off.sent == [] and stranger.sent == []


async def test_an_unconfigured_chip_is_uploaded_and_rebooted_once() -> None:
    chip = FakeChip(GRID, None)
    await connect(chip)
    assert chip.sent[0] == "device show"
    assert chip.sent[1].startswith("device upload ")
    assert json.loads(chip.sent[1][len("device upload ") :]) == ENTRY
    assert chip.sent[2:] == ["json info", "reboot"]
    assert chip.running == full_config(ENTRY)
    # It reconnects with the entry applied: one compare, and the reboot record is cleared.
    chip.sent, chip.last_info = [], chip.info()
    await connect(chip)
    assert chip.sent == ["device show"]
    assert json.loads(box_config.state_path().read_text()) == {}


async def test_a_drifted_field_is_uploaded() -> None:
    chip = FakeChip(GRID, full_config({**ENTRY, "brightness": 40}))
    await connect(chip)
    assert chip.sent[1].startswith("device upload ") and chip.sent[-1] == "reboot"
    assert chip.running is not None and chip.running["brightness"] == 0


async def test_a_chip_that_still_differs_after_its_reboot_is_left_alone() -> None:
    chip = FakeChip(GRID, None)
    chip.reply_orig = chip.reply  # type: ignore[attr-defined]

    def ignores_brightness(command: str) -> str:
        out = chip.reply_orig(command)  # type: ignore[attr-defined]
        if command.startswith("device upload ") and chip.stored is not None:
            chip.stored["brightness"] = 100  # a firmware that doesn't keep it
        return out

    chip.reply = ignores_brightness  # type: ignore[method-assign]
    await connect(chip)
    assert chip.sent[-1] == "reboot"
    chip.sent, chip.last_info = [], chip.info()
    await connect(chip)
    assert chip.sent == ["device show"]  # no second upload, no second reboot


async def test_no_reboot_while_a_run_holds_the_chip(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setitem(leds._holders, GRID, 1)
    chip = FakeChip(GRID, None)
    await connect(chip)
    assert chip.sent[1].startswith("device upload ")
    assert "reboot" not in chip.sent  # its next reboot (the run's flash) applies it
    assert not box_config.state_path().exists()


async def test_waits_out_markstable_and_skips_a_chip_that_reset_meanwhile(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(box_config, "MIN_UPTIME_S", 0.2)
    chip = FakeChip(GRID, None, millis=100)
    await box_config.reconcile(FakeFleet(), chip)  # type: ignore[arg-type]
    chip.millis = 50  # a flash reset it while the server waited
    await asyncio.wait_for(box_config._pending[GRID], 5)
    assert chip.sent[-1] == "json info"


def test_an_entry_compares_equal_to_its_full_stored_config() -> None:
    shown = full_config(ENTRY) | {"upVectorZ": 1.0, "rotationDegrees": 0.0}
    assert box_config.stored_diff(shown, ENTRY) == []
    assert box_config.stored_diff({"error": "No device config"}, ENTRY) == ["unconfigured"]
    assert box_config.stored_diff(shown | {"orientation": 0}, ENTRY) == ["orientation: 0 -> 5"]


async def test_no_local_config_leaves_every_chip_alone() -> None:
    """A box without its chips file, or a chip whose entry file is missing: logged, untouched."""
    (box_config.CONFIG_DIR / "grid.json").unlink()
    chip = FakeChip(GRID, None)
    await connect(chip)
    (box_config.CONFIG_DIR / "box_chips.json").unlink()
    await connect(chip)
    assert box_config.box_chips() == {} and chip.sent == []
