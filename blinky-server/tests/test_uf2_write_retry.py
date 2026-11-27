"""A UF2 copy the bootloader never fully received is caught and retried once.

2026-11-25, job ecc8a4780e9f: a USB reset hit the bootloader 3 s into the copy's
writeback. fsync failed, uf2_upload.py swallowed it and printed ``[PASS] Wrote N / N``
from the page-cache byte count, the bootloader sat waiting for the missing blocks, and
the chip stayed parked until auto-recovery's 30-minute guard ran out.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

import blinky_server.device.manager as mgr
from blinky_server.device.manager import FleetManager
from blinky_server.firmware.anomalies import UF2_WRITE_RETRIED, UF2_WRITE_UNCONFIRMED
from blinky_server.firmware.flash_job import FlashJobState
from blinky_server.firmware.uf2_upload import uf2_drive_present, usb_serial_of_port

from .test_flash_job_manager import _serial_device_in

# --- server helpers -------------------------------------------------------------


def test_usb_serial_of_port_reads_the_by_id_name(tmp_path: Path) -> None:
    tty = tmp_path / "ttyACM0"
    tty.write_text("")
    by_id = tmp_path / "by-id"
    by_id.mkdir()
    (by_id / "usb-Seeed_XIAO_nRF52840_Sense_0123456789ABCDEF-if00").symlink_to(tty)
    assert usb_serial_of_port(str(tty), by_id) == "0123456789ABCDEF"
    assert usb_serial_of_port(str(tmp_path / "ttyACM9"), by_id) is None
    assert usb_serial_of_port(str(tty), tmp_path / "missing") is None


def test_uf2_drive_present_needs_a_serial() -> None:
    assert uf2_drive_present(None) is False
    assert uf2_drive_present("") is False


# --- manager._run_uf2_flash: one retry while the drive stays ---------------------


@pytest.fixture
def fleet() -> FleetManager:
    return FleetManager(enable_ble=False, enable_serial=False)


def _wire(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
    results: list[bool],
    drive: list[bool],
) -> list[dict[str, Any]]:
    """Each write pops the next result; each drive check pops the next presence (the
    last one repeats)."""
    calls: list[dict[str, Any]] = []

    async def fake_write(job: Any, **kwargs: Any) -> bool:
        calls.append(kwargs)
        ok = results.pop(0)
        if not ok:
            job.set_error("UF2 write did not complete: [FAIL] Write not confirmed")
        return ok

    def fake_present(_sn: str | None) -> bool:
        return drive.pop(0) if len(drive) > 1 else drive[0]

    async def fake_verify(*_: Any) -> None:
        return None

    monkeypatch.setattr(mgr, "_uf2_write_impl_for_job", fake_write)
    monkeypatch.setattr(mgr, "uf2_drive_present", fake_present)
    monkeypatch.setattr(mgr, "usb_serial_of_port", lambda _p: "0123456789ABCDEF")
    monkeypatch.setattr(mgr, "UF2_RETRY_SETTLE_S", 0.0)
    monkeypatch.setattr(fleet, "_verify_with_anomaly_checks", fake_verify)
    return calls


@pytest.mark.asyncio
@pytest.mark.parametrize("first_ok", [False, True])
async def test_drive_still_attached_retries_the_copy_once(
    fleet: FleetManager, monkeypatch: pytest.MonkeyPatch, first_ok: bool
) -> None:
    """Whether or not the writer claimed success, a drive that stays means the
    bootloader is waiting for blocks: one more copy, into the bootloader by serial."""
    dev = "TEST_RETRY"
    await _serial_device_in(fleet, dev)
    calls = _wire(fleet, monkeypatch, [first_ok, True], [True, False])
    job = await fleet.flash_device(dev, Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=5.0)

    assert job.state is FlashJobState.COMPLETED
    assert len(calls) == 2
    assert calls[1]["already_in_bootloader"] is True
    assert calls[1]["usb_serial"] == "0123456789ABCDEF"
    assert UF2_WRITE_RETRIED in job.anomalies
    assert job.error is None


@pytest.mark.asyncio
async def test_retry_happens_once_only(
    fleet: FleetManager, monkeypatch: pytest.MonkeyPatch
) -> None:
    """No flash loop: a second failure with the drive still there ends the job."""
    dev = "TEST_RETRY_ONCE"
    await _serial_device_in(fleet, dev)
    calls = _wire(fleet, monkeypatch, [False, False], [True])
    job = await fleet.flash_device(dev, Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=5.0)

    assert job.state is FlashJobState.FAILED
    assert len(calls) == 2
    assert job.error and "Write not confirmed" in job.error


@pytest.mark.asyncio
async def test_no_drive_no_retry(fleet: FleetManager, monkeypatch: pytest.MonkeyPatch) -> None:
    """A failure with no UF2 drive (e.g. bootloader entry failed) fails as before."""
    dev = "TEST_NO_DRIVE"
    await _serial_device_in(fleet, dev)
    calls = _wire(fleet, monkeypatch, [False], [False])
    job = await fleet.flash_device(dev, Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=5.0)

    assert job.state is FlashJobState.FAILED
    assert len(calls) == 1
    assert UF2_WRITE_RETRIED not in job.anomalies


@pytest.mark.asyncio
async def test_unconfirmed_write_whose_drive_left_goes_to_verify(
    fleet: FleetManager, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The writer couldn't confirm, but the bootloader rebooted after all: verify decides."""
    dev = "TEST_LEFT"
    await _serial_device_in(fleet, dev)
    calls = _wire(fleet, monkeypatch, [False], [True, False])
    monkeypatch.setattr(mgr, "UF2_RETRY_SETTLE_S", 5.0)  # gone at the first 1 s poll
    job = await fleet.flash_device(dev, Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=5.0)

    assert job.state is FlashJobState.COMPLETED
    assert len(calls) == 1
    assert UF2_WRITE_UNCONFIRMED in job.anomalies
    assert job.error is None
