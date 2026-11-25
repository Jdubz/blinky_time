"""The test box's LEDs: dark unless a test runs (testing/leds.py).

Only the box's chips are ever sent ``leds``: nothing off a --staging server, nothing to an
unconfigured chip, and the API refuses the command elsewhere. A run lights its chips and
always sets them back (``leds config``), failure included; firmware without the command fails a
run that needs light and only logs for one that doesn't.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest
from fastapi import HTTPException

from blinky_server.api.deps import assert_command_allowed, is_test_box_only_command
from blinky_server.device.device import Device, DeviceState
from blinky_server.testing import leds

from .mock_transport import MockTransport


class FakeProtocol:
    def __init__(self, chip: FakeChip) -> None:
        self.chip = chip

    async def send_command(self, command: str, timeout: float | None = None) -> str:
        self.chip.sent.append(command)
        return self.chip.reply(command)


class FakeChip:
    """A chip whose firmware has ``leds`` (``supported``) or not, at config brightness 0."""

    def __init__(self, dev_id: str, *, configured: bool = True, supported: bool = True) -> None:
        self.id = dev_id
        self.version = "b300" if supported else "b211"
        self.configured = configured
        self.supported = supported
        self.sent: list[str] = []
        self.brightness = 0
        self.override = False
        self.protocol = FakeProtocol(self)

    def reply(self, command: str) -> str:
        if not self.supported:
            return "Unknown command. Try 'settings' for help.\n"
        arg = command.split(" ", 1)[1]
        if arg == "config":
            self.brightness, self.override = 0, False
        else:
            self.brightness, self.override = int(arg), True
        return (
            json.dumps({"brightness": self.brightness, "config": 0, "override": self.override})
            + "\n"
        )


@pytest.fixture
def on_box(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(leds, "on_box", lambda: True)


@pytest.fixture
def off_box(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(leds, "on_box", lambda: False)


def test_parse_reply() -> None:
    assert leds.parse_reply('{"brightness":40,"config":0,"override":true}\n') == {
        "brightness": 40,
        "config": 0,
        "override": True,
    }
    # A log line inside the reply doesn't hide the status.
    assert leds.parse_reply('[NN] ready\n{"brightness":0,"config":0,"override":false}')
    assert leds.parse_reply("Unknown command. Try 'settings' for help.") is None
    with pytest.raises(ValueError):
        leds.parse_reply("ERROR: Usage: leds [status] | off | config | 0-255")


async def test_off_box_sends_nothing(off_box: None) -> None:
    chip = FakeChip("A")
    async with leds.lit([chip], needs_light=True, purpose="test"):  # type: ignore[list-item]
        pass
    assert chip.sent == []


async def test_lit_for_the_block_then_config(on_box: None) -> None:
    chip = FakeChip("A")
    async with leds.lit([chip], needs_light=True, purpose="test"):  # type: ignore[list-item]
        assert chip.sent == [f"leds {leds.TEST_BRIGHTNESS}"]
        assert chip.brightness == leds.TEST_BRIGHTNESS
    assert chip.sent[-1] == "leds config"
    assert (chip.brightness, chip.override) == (0, False)


async def test_set_back_on_failure(on_box: None) -> None:
    chip = FakeChip("A")
    with pytest.raises(RuntimeError, match="boom"):
        async with leds.lit([chip], needs_light=False, purpose="test"):  # type: ignore[list-item]
            raise RuntimeError("boom")
    assert chip.sent == [f"leds {leds.TEST_BRIGHTNESS}", "leds config"]


async def test_nested_holders_darken_once_at_the_end(on_box: None) -> None:
    chip = FakeChip("A")
    async with leds.lit([chip], needs_light=False, purpose="suite"):  # type: ignore[list-item]
        async with leds.lit([chip], needs_light=False, purpose="session"):  # type: ignore[list-item]
            pass
        assert chip.override is True  # the inner run's end leaves the suite's light on
    assert chip.sent == [f"leds {leds.TEST_BRIGHTNESS}", "leds config"]


async def test_unconfigured_chip_is_never_sent_leds(on_box: None) -> None:
    other = FakeChip("ESP32", configured=False)
    async with leds.lit([other], needs_light=True, purpose="test"):  # type: ignore[list-item]
        pass
    await leds.reconcile(other, staging=True)  # type: ignore[arg-type]
    assert other.sent == []


async def test_old_firmware_fails_a_run_that_needs_light(on_box: None) -> None:
    chip = FakeChip("A", supported=False)
    ran = False
    with pytest.raises(leds.LedsError, match="no `leds` command"):
        async with leds.lit([chip], needs_light=True, purpose="camera review"):  # type: ignore[list-item]
            ran = True
    assert not ran
    assert chip.sent == [f"leds {leds.TEST_BRIGHTNESS}", "leds config"]


async def test_old_firmware_logs_for_a_run_that_does_not(
    on_box: None, caplog: pytest.LogCaptureFixture
) -> None:
    chip = FakeChip("A", supported=False)
    ran = False
    async with leds.lit([chip], needs_light=False, purpose="validation"):  # type: ignore[list-item]
        ran = True
    assert ran
    assert any("LEDs not lit for validation" in r.message for r in caplog.records)


async def test_reconcile(on_box: None) -> None:
    chip = FakeChip("A")
    await leds.reconcile(chip, staging=False)  # type: ignore[arg-type]
    assert chip.sent == []  # a fleet server never sends leds
    chip.override, chip.brightness = True, 40  # a run cut off by a crash left it lit
    await leds.reconcile(chip, staging=True)  # type: ignore[arg-type]
    assert chip.sent == ["leds config"] and chip.override is False
    async with leds.lit([chip], needs_light=False, purpose="test"):  # type: ignore[list-item]
        chip.brightness, chip.override = 0, False  # rebooted mid-run
        await leds.reconcile(chip, staging=True)  # type: ignore[arg-type]
        assert chip.brightness == leds.TEST_BRIGHTNESS  # the run still holds it: lit again


async def test_reconcile_never_raises(on_box: None) -> None:
    chip = FakeChip("A", supported=False)
    await leds.reconcile(chip, staging=True)  # type: ignore[arg-type]
    chip.protocol.send_command = None  # type: ignore[assignment]  # any failure
    await leds.reconcile(chip, staging=True)  # type: ignore[arg-type]


async def test_session_lights_after_setup_and_sets_back_after(
    monkeypatch: pytest.MonkeyPatch, on_box: None
) -> None:
    """A test session on the box: reset and test commands, then lit; ``leds config`` at the
    end, also when the run fails."""
    from types import SimpleNamespace

    from blinky_server.testing import workflows as wf

    chips = {"a": FakeChip("a"), "esp": FakeChip("esp", configured=False)}

    async def nothing(*a: object) -> None:
        return None

    async def connected(ids: list[str]) -> list[FakeChip]:
        return [chips[i] for i in ids]

    async def configure(device: FakeChip, commands: list[str]) -> None:
        device.sent.extend(["restore_runtime_settings", *commands])

    monkeypatch.setattr(wf, "jobs", SimpleNamespace(report=lambda *a: None))
    monkeypatch.setattr(wf, "acquire_audio_lock", lambda ids: True)
    monkeypatch.setattr(wf, "release_audio_lock", lambda: None)
    monkeypatch.setattr(wf, "kill_orphan_audio", nothing)
    monkeypatch.setattr(wf, "stop_audio", nothing)
    monkeypatch.setattr(wf, "_start_streaming", nothing)
    monkeypatch.setattr(wf, "_stop_streaming", nothing)
    monkeypatch.setattr(wf, "_connected", connected)
    monkeypatch.setattr(wf, "_configure_device", configure)
    with pytest.raises(RuntimeError, match="run failed"):
        async with wf.session(["a", "esp"], ["gen audio"], needs_light=True, purpose="review"):
            assert chips["a"].override is True
            raise RuntimeError("run failed")
    assert chips["a"].sent == [
        "restore_runtime_settings",
        "gen audio",
        f"leds {leds.TEST_BRIGHTNESS}",
        "leds config",
    ]
    assert chips["esp"].sent == ["restore_runtime_settings", "gen audio"]  # never `leds`


async def test_connect_runs_the_hook_before_connected(mock_transport: MockTransport) -> None:
    device = Device("SN1", "/dev/ttyTEST0", "nrf52840", mock_transport)  # type: ignore[arg-type]
    seen: list[DeviceState] = []

    async def hook(d: Device) -> None:
        seen.append(d.state)
        raise RuntimeError("hook failure must not fail the connect")

    device.post_connect = hook
    await device.connect()
    assert seen == [DeviceState.CONNECTING]
    assert device.state == DeviceState.CONNECTED


def test_leds_is_refused_off_the_box() -> None:
    assert is_test_box_only_command("leds 40")
    assert is_test_box_only_command("  LEDS   config ")
    assert is_test_box_only_command("leds")
    assert not is_test_box_only_command("ledsx")
    with pytest.raises(HTTPException) as e:
        assert_command_allowed("leds 40", None)  # a fleet server, or any /fleet/command
    assert e.value.status_code == 403
    with pytest.raises(HTTPException):
        assert_command_allowed("leds config", "deploy.sh-abc")  # deploy.sh doesn't open it
    assert_command_allowed("leds 40", None, test_box=True)  # a --staging server's chip


def test_calibration_lights_at_the_test_brightness() -> None:
    """camera_calibrate.py (the box's system Python, no server import) keeps its own copy."""
    path = Path(__file__).resolve().parents[2] / "scripts/testbox/camera/camera_calibrate.py"
    m = re.search(r"^TEST_BRIGHTNESS = (\d+)$", path.read_text(), re.M)
    assert m and int(m.group(1)) == leds.TEST_BRIGHTNESS
