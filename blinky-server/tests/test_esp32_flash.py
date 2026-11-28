"""The ESP32-S3 flash path (docs/TEST_BOX.md, "ESP32-S3 flash and recovery"): esptool through the
single flash entry point, chosen by the chip's platform, never an image of the other platform,
and the S3's recovery by a full re-flash of its armed stable image."""

from __future__ import annotations

import asyncio
import struct
import sys
from pathlib import Path
from typing import Any

import pytest

from blinky_server.device import manager as manager_mod
from blinky_server.device.device import Device, DeviceState
from blinky_server.device.manager import ESP32_RECOVERY_GRACE_S, FleetManager
from blinky_server.firmware import esp32_flash, images
from blinky_server.firmware._guard import OutsideFlashJobContextError
from blinky_server.firmware.flash_job import (
    FlashJob,
    FlashJobState,
    FlashTransport,
    NoReachableTransport,
    TransportProbe,
    select_transport,
)

from .mock_transport import MockTransport

MAC = "02:00:5E:10:00:01"
HEX = b":00000001FF\n" * 200


def s3_image(chip_id: int = 9, fill: bytes = b"\x22") -> bytes:
    img = bytearray(fill * 4096)
    img[0] = 0xE9
    struct.pack_into("<H", img, 12, chip_id)
    return bytes(img)


class PortTransport(MockTransport):
    """A serial transport whose port is a real path (enumerated while the file exists)."""

    def __init__(self, port: Path, **kw: Any) -> None:
        super().__init__(transport_type="serial", **kw)
        self._port = port

    @property
    def address(self) -> str:
        return str(self._port)


def s3_device(fleet: FleetManager, tmp_path: Path, state: DeviceState) -> Device:
    port = tmp_path / "usb-Espressif_USB_JTAG_serial_debug_unit_02:00:5E:10:00:01-if00"
    port.write_text("")
    d = Device(MAC, str(port), "esp32s3", PortTransport(port))  # type: ignore[arg-type]
    d.state = state
    fleet._devices[MAC] = d
    return d


def nrf_device(fleet: FleetManager, tmp_path: Path) -> Device:
    port = tmp_path / "usb-Seeed_XIAO_nRF52840_Sense_0123456789ABCDEF-if00"
    port.write_text("")
    d = Device("0123456789ABCDEF", str(port), "nrf52840", PortTransport(port))  # type: ignore[arg-type]
    d.state = DeviceState.CONNECTED
    fleet._devices[d.id] = d
    return d


@pytest.fixture
def fleet() -> FleetManager:
    return FleetManager(enable_ble=False, enable_serial=False, staging=True)


# -- images: the platform is in the bytes ------------------------------------------------------


def test_platform_comes_from_the_bytes() -> None:
    assert images.platform_of(HEX) == "nrf52840"
    assert images.platform_of(s3_image()) == "esp32s3"
    assert images.platform_of(s3_image(chip_id=0)) is None  # an ESP32 image
    assert images.platform_of(s3_image(chip_id=5)) is None  # an ESP32-C3 image
    assert images.platform_of(b"UF2\n" + b"\0" * 600) is None


def test_s3_images_are_stored_as_bin_with_their_platform() -> None:
    img = images.store(s3_image(), version="b230", stable=True, source="deploy")
    assert img.path.suffix == ".bin" and img.platform == "esp32s3"
    loaded = images.load(img.path)
    assert loaded is not None and loaded.platform == "esp32s3" and loaded.stable


def test_a_build_names_its_hex_and_each_chip_takes_its_platforms_image() -> None:
    hexi = images.store(HEX, version="b230", stable=True, source="deploy")
    s3 = images.store(s3_image(), version="b230", stable=True, source="deploy")
    assert images.for_platform(hexi.path, "esp32s3") is None  # not linked yet
    images.link(hexi.path, s3.path)
    got_s3, got_nrf = (
        images.for_platform(hexi.path, "esp32s3"),
        images.for_platform(hexi.path, "nrf52840"),
    )
    assert got_s3 is not None and got_s3.path == s3.path
    assert got_nrf is not None and got_nrf.path == hexi.path
    # Storing the hex again (the next deploy of the same build) keeps the link.
    images.store(HEX, version="b230", stable=True, source="deploy")
    again = images.for_platform(hexi.path, "esp32s3")
    assert again is not None and again.path == s3.path


def test_link_refuses_another_build_or_the_same_platform() -> None:
    hexi = images.store(HEX, version="b230", stable=True, source="deploy")
    other = images.store(s3_image(fill=b"\x33"), version="b231", stable=True, source="deploy")
    with pytest.raises(ValueError, match="versions differ"):
        images.link(hexi.path, other.path)
    hex2 = images.store(HEX + b"\n", version="b230", stable=True, source="deploy")
    with pytest.raises(ValueError, match="both images"):
        images.link(hexi.path, hex2.path)


# -- transport: by platform, never a guess -----------------------------------------------------


def test_an_s3_takes_esptool_and_only_esptool() -> None:
    assert (
        select_transport(TransportProbe(has_usb_app=True, has_ble_dfu_advert=False,
                                        platform="esp32s3", has_usb_esp32=True))
        is FlashTransport.ESPTOOL
    )  # fmt: skip
    with pytest.raises(NoReachableTransport, match="ESP32-S3"):
        # App mode on serial or a BLE advert never makes it UF2 or BLE-DFU.
        select_transport(
            TransportProbe(has_usb_app=True, has_ble_dfu_advert=True, platform="esp32s3")
        )


def test_an_nrf52840_never_takes_esptool() -> None:
    probe = TransportProbe(
        has_usb_app=True, has_ble_dfu_advert=False, platform="nrf52840", has_usb_esp32=True
    )
    assert select_transport(probe) is FlashTransport.UF2


def test_probe_sees_an_unanswering_s3_on_usb(fleet: FleetManager, tmp_path: Path) -> None:
    s3_device(fleet, tmp_path, DeviceState.ERROR)
    probe = fleet._build_transport_probe(MAC)
    assert probe.platform == "esp32s3" and probe.has_usb_esp32 and not probe.has_usb_app


# -- the orchestrator refuses an image of the other platform -----------------------------------


@pytest.mark.parametrize("which", ["s3_image_to_nrf", "hex_to_s3"])
async def test_wrong_platform_image_is_refused_before_any_write(
    fleet: FleetManager, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, which: str
) -> None:
    writes: list[str] = []

    async def no_write(*a: Any, **k: Any) -> bool:
        writes.append("write")
        return True

    monkeypatch.setattr(manager_mod, "_esp32_write_impl_for_job", no_write)
    monkeypatch.setattr(manager_mod, "_uf2_write_impl_for_job", no_write)
    fw = tmp_path / "fw.bin"
    if which == "s3_image_to_nrf":
        device = nrf_device(fleet, tmp_path)
        fw.write_bytes(s3_image())
        want = "is an ESP32-S3 image"
    else:
        device = s3_device(fleet, tmp_path, DeviceState.CONNECTED)
        fw.write_bytes(HEX)
        want = "not an ESP32-S3 image"
    job = await fleet.flash_device(device.id, fw)
    await job.wait_until_terminal(timeout=5.0)
    assert job.state is FlashJobState.FAILED and want in (job.error or "")
    assert job.transport is None and writes == []


async def test_s3_flash_runs_esptool_then_verifies(
    fleet: FleetManager, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    device = s3_device(fleet, tmp_path, DeviceState.CONNECTED)
    fw = tmp_path / "fw.bin"
    fw.write_bytes(s3_image())
    calls: list[dict[str, Any]] = []

    async def fake_write(job: FlashJob, **kw: Any) -> bool:
        calls.append({**kw, "held": fleet._reconnect_blackout.get(MAC) is not None})
        job.record_progress(10, 10)
        return True

    verified: list[bool] = []

    async def fake_verify(job: FlashJob, signals: Any, history: Any) -> None:
        verified.append(await signals.has_re_enumerated_since(MAC, 0.0))

    monkeypatch.setattr(manager_mod, "_esp32_write_impl_for_job", fake_write)
    monkeypatch.setattr(fleet, "_verify_with_anomaly_checks", fake_verify)
    fleet._reconnect_failures[MAC] = 5  # past the serial retry limit before the flash
    job = await fleet.flash_device(device.id, fw)
    await job.wait_until_terminal(timeout=5.0)
    assert job.state is FlashJobState.COMPLETED, job.error
    assert job.transport is FlashTransport.ESPTOOL
    assert calls == [
        {
            "serial_port": device.transport.address,
            "firmware_path": str(fw),
            "transport": device.transport,
            "expect_mac": MAC,
            "full": False,  # an operator's flash keeps NVS (the config)
            "held": True,  # the server stays off the port while the tool has it
        }
    ]
    assert verified == [True]  # esptool's reset counts as the reboot
    assert MAC not in fleet._reconnect_blackout and MAC not in fleet._reconnect_failures


async def test_s3_write_failure_fails_the_job(
    fleet: FleetManager, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    device = s3_device(fleet, tmp_path, DeviceState.CONNECTED)
    fw = tmp_path / "fw.bin"
    fw.write_bytes(s3_image())

    async def fail(job: FlashJob, **kw: Any) -> bool:
        job.set_error("ESP32-S3 write did not complete: ERROR: the chip's MAC is X")
        return False

    monkeypatch.setattr(manager_mod, "_esp32_write_impl_for_job", fail)
    job = await fleet.flash_device(device.id, fw)
    await job.wait_until_terminal(timeout=5.0)
    assert job.state is FlashJobState.FAILED and "MAC" in (job.error or "")


# -- the write impl: guarded, and it reads the tool's verdict ----------------------------------


async def test_write_impl_is_guarded() -> None:
    job = FlashJob(device_id=MAC, firmware_path=Path("/tmp/x.bin"))
    with pytest.raises(OutsideFlashJobContextError):
        await esp32_flash._esp32_write_impl_for_job(
            job, "/dev/null", "/tmp/x.bin", MockTransport(), expect_mac=MAC, full=False
        )


def _fake_tool(tmp_path: Path, lines: list[str]) -> Path:
    tool = tmp_path / "esp32_flash.py"
    body = "".join(f"print({line!r}, flush=True)\n" for line in lines)
    tool.write_text("import sys\nprint('ARGS', ' '.join(sys.argv[1:]), flush=True)\n" + body)
    return tool


async def _run_impl(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, lines: list[str], full: bool
) -> tuple[bool, FlashJob, list[str]]:
    from blinky_server.firmware import _guard

    monkeypatch.setattr(esp32_flash, "_find_tool", lambda: _fake_tool(tmp_path, lines))
    logged: list[str] = []
    monkeypatch.setattr(
        esp32_flash.log, "info", lambda fmt, *a: logged.append(fmt % a if a else fmt)
    )
    fw = tmp_path / "fw.bin"
    fw.write_bytes(s3_image())
    job = FlashJob(device_id=MAC, firmware_path=fw)
    job.transition(FlashJobState.SELECTING_TRANSPORT)
    job.transition(FlashJobState.WRITING)
    token = _guard.enter_orchestrator_context()
    try:
        ok = await esp32_flash._esp32_write_impl_for_job(
            job, "/dev/ttyACM1", str(fw), MockTransport(), expect_mac=MAC, full=full
        )
    finally:
        _guard.reset_orchestrator_context(token)
    return ok, job, logged


async def test_write_impl_success_and_arguments(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    ok, job, logged = await _run_impl(
        tmp_path,
        monkeypatch,
        ["[PASS] Wrote 1,024 / 1,024 bytes", "[PASS] Reset into the app"],
        full=True,
    )
    assert ok and job.bytes_written == 1024 and job.error is None
    args = next(li for li in logged if "ARGS" in li)
    assert (
        f"--port /dev/ttyACM1 --image {tmp_path / 'fw.bin'} --expect-mac {MAC} --mode full" in args
    )


async def test_write_impl_reports_the_tools_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    ok, job, _ = await _run_impl(
        tmp_path,
        monkeypatch,
        ["ERROR: /dev/ttyACM1 is 2886:8045, not an ESP32-S3's USB Serial/JTAG (303a:1001)"],
        full=False,
    )
    assert not ok and "not an ESP32-S3" in (job.error or "")


def test_the_impl_runs_the_tool_with_the_servers_interpreter() -> None:
    """esptool is in the server's venv, not the system Python."""
    src = Path(esp32_flash.__file__).read_text()
    assert "sys.executable" in src and sys.executable


# -- recovery: a full re-flash of the armed stable image ---------------------------------------


def test_an_s3_image_arms_the_s3s_own_recovery(fleet: FleetManager, tmp_path: Path) -> None:
    s3_device(fleet, tmp_path, DeviceState.CONNECTED)
    nrf = nrf_device(fleet, tmp_path)
    hexi = images.store(HEX, version="b230", stable=True, source="deploy").path
    s3 = images.store(s3_image(), version="b230", stable=True, source="deploy").path
    fleet.set_recovery_firmware(str(hexi), [nrf.id])
    fleet.set_recovery_firmware(str(s3), [MAC])
    assert fleet.recovery_firmware_path == str(hexi)  # the nRF52840's arm untouched
    assert fleet._esp32_recovery_firmware_path == str(s3)
    assert sorted(fleet.recovery_firmware_paths) == sorted([str(hexi), str(s3)])
    with pytest.raises(ValueError, match="not ESP32-S3s"):
        fleet.set_recovery_firmware(str(s3), [nrf.id])
    with pytest.raises(ValueError, match="are ESP32-S3s"):
        fleet.set_recovery_firmware(str(hexi), [MAC])
    # Persisted, and armed again by the next start.
    again = FleetManager(enable_ble=False, enable_serial=False)
    loaded = again._load_recovery_firmware(again._esp32_recovery_state_path())
    assert loaded == (str(s3), {MAC})


def test_an_unstable_s3_image_never_arms(fleet: FleetManager, tmp_path: Path) -> None:
    s3_device(fleet, tmp_path, DeviceState.CONNECTED)
    s3 = images.store(s3_image(), version="b230-dev", stable=False, source="staging").path
    with pytest.raises(ValueError, match="not a verified stable"):
        fleet.set_recovery_firmware(str(s3), [MAC])


async def test_an_s3_that_stops_answering_is_recovered_once_after_the_grace(
    fleet: FleetManager, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    device = s3_device(fleet, tmp_path, DeviceState.CONNECTED)
    s3 = images.store(s3_image(), version="b230", stable=True, source="deploy").path
    fleet.set_recovery_firmware(str(s3), [MAC])
    clock = [1000.0]
    monkeypatch.setattr(manager_mod._time, "monotonic", lambda: clock[0])
    flashes: list[tuple[str, Path, bool]] = []

    async def fake_flash(device_id: str, path: Path, *, force: bool = True, **k: Any) -> Any:
        flashes.append((device_id, path, force))
        job = FlashJob(device_id=device_id, firmware_path=path, is_auto_recovery=not force)
        job.transition(FlashJobState.FAILED)
        return job

    monkeypatch.setattr(fleet, "flash_device", fake_flash)

    await fleet._auto_recover_esp32_devices()  # answering: nothing
    device.state = DeviceState.ERROR  # a crash: the port stays, json info never answers
    await fleet._auto_recover_esp32_devices()  # starts the grace
    clock[0] += ESP32_RECOVERY_GRACE_S - 1
    await fleet._auto_recover_esp32_devices()
    assert flashes == []
    clock[0] += 2
    await fleet._auto_recover_esp32_devices()
    assert flashes == [(MAC, s3, False)]  # force=False: dedup, backoff and the cap apply
    await asyncio.sleep(0)


async def test_no_recovery_while_held_flashing_unarmed_or_off_usb(
    fleet: FleetManager, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    device = s3_device(fleet, tmp_path, DeviceState.ERROR)
    clock = [1000.0]
    monkeypatch.setattr(manager_mod._time, "monotonic", lambda: clock[0])
    assert fleet._esp32_recovery_candidates() == []  # not whitelisted (unarmed)
    s3 = images.store(s3_image(), version="b230", stable=True, source="deploy").path
    fleet.set_recovery_firmware(str(s3), [MAC])
    fleet._esp32_recovery_candidates()
    clock[0] += ESP32_RECOVERY_GRACE_S + 1
    fleet.hold_reconnect(MAC, 60)  # an operator's release, or a flash's hold
    assert fleet._esp32_recovery_candidates() == []
    fleet.resume_reconnect(MAC)
    fleet._device_in_flight.add(MAC)
    assert fleet._esp32_recovery_candidates() == []
    fleet._device_in_flight.discard(MAC)
    Path(device.transport.address).unlink()  # off USB: nothing to reach
    assert fleet._esp32_recovery_candidates() == []
    Path(device.transport.address).write_text("")
    # The grace starts over after any of these.
    assert fleet._esp32_recovery_candidates() == []
    clock[0] += ESP32_RECOVERY_GRACE_S + 1
    assert [d.id for d in fleet._esp32_recovery_candidates()] == [MAC]


async def test_recovery_flash_erases_the_whole_chip(
    fleet: FleetManager, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    device = s3_device(fleet, tmp_path, DeviceState.ERROR)
    s3 = images.store(s3_image(), version="b230", stable=True, source="deploy").path
    seen: list[bool] = []

    async def fake_write(job: FlashJob, **kw: Any) -> bool:
        seen.append(kw["full"])
        return False

    monkeypatch.setattr(manager_mod, "_esp32_write_impl_for_job", fake_write)
    job = await fleet.flash_device(device.id, s3, force=False)
    assert job is not None
    await job.wait_until_terminal(timeout=5.0)
    assert seen == [True]
    # The dedup window now keeps the next attempt minutes away (never a loop).
    assert not fleet.should_attempt_auto_recovery(MAC)


# -- the hardware checks' board ------------------------------------------------------------------


@pytest.mark.parametrize(
    ("board", "report", "result"),
    [
        ("testbox_esp32", "[BLE] role=advertiser state=ready\n[BLE] packets_tx=12 errors=0", "yes"),
        ("testbox_esp32", "[BLE] role=advertiser state=not_initialized", "no"),
        ("testbox_esp32", "[BLE] Not available on this platform", "no"),
        ("testbox_grid", "[BLE] advertising=yes\n[BLE] role=scanner state=active", "yes"),
        ("testbox_grid", "[BLE] role=advertiser state=ready", "no"),
    ],
)
async def test_ble_stack_is_judged_by_the_boards_own_report(
    board: str, report: str, result: str
) -> None:
    from types import SimpleNamespace

    from blinky_server.testing import hardware_checks as hc

    class Proto:
        async def send_command(self, cmd: str) -> str:
            assert cmd == "ble"
            return report

    d = SimpleNamespace(id=MAC, protocol=Proto())
    got = await hc._ble_stack(d, hc.BOARDS[board])
    assert got["result"] == result, got


def test_the_s3_board_gets_every_check_but_the_walk() -> None:
    from blinky_server.testing import hardware_checks as hc

    b = hc.BOARDS["testbox_esp32"]
    assert (b.platform, b.leds, b.regions, b.ble_stack) == ("esp32s3", 64, None, True)
    # A MAC id is never compared with json info's 16-hex `sn` (that's the nRF52840's FICR id).
    info = {"version": "b230", "millis": 1, "audioOverruns": 0, "audioSamplesLost": 0,
            "fps": 120, "maxFrameMs": 9, "platform": "esp32s3", "device": {"leds": 64},
            "sn": "0011223344556677"}  # fmt: skip
    assert hc.sane(info, MAC, b) == []
    assert hc.sane({**info, "platform": "nrf52840"}, MAC, b) == ["platform 'nrf52840', not esp32s3"]


async def test_a_board_without_a_region_map_still_checks_the_leds_override(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """No camera view of the ESP32-S3's grid: the walk is not applicable, but `leds 40` must
    still apply (the runtime override that lights and darkens the box's chips)."""
    from types import SimpleNamespace

    from blinky_server.testing import hardware_checks as hc

    sent: list[str] = []

    async def fake_send(d: Any, arg: str) -> dict[str, Any]:
        sent.append(arg)
        return {"brightness": int(arg), "config": 0, "override": True}

    monkeypatch.setattr(hc.leds, "send", fake_send)
    d = SimpleNamespace(id=MAC)

    async def chips(ids: list[str]) -> list[Any]:
        return [d]

    monkeypatch.setattr(hc, "_chips", chips)
    out = await hc._lights([MAC], {MAC: "testbox_esp32"}, None, 40)
    assert [(c["name"], c["result"]) for c in out["checks"]] == [
        ("leds", "yes"),
        ("walk", "not applicable"),
    ]
    assert sent == ["40"] and out["rig_error"] is None
