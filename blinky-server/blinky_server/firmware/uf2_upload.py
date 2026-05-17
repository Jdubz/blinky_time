"""UF2 firmware upload for nRF52840 devices.

Delegates to tools/uf2_upload.py for UF2 drive detection, firmware copy, and
verification.

Upload sequence:
  1. Disconnect the server's serial transport
  2. USB-reset the device (standard Linux USBDEVFS_RESET ioctl) to
     reinitialize the TinyUSB CDC state machine
  3. Launch uf2_upload.py which opens a fresh serial connection, sends the
     bootloader command, detects the UF2 drive, copies firmware, and verifies

The USB-reset in step 2 is required because closing a serial port on nRF52840
with TinyUSB drops DTR, which puts the CDC into a "no host" state that
persists across subsequent port opens. This is a well-known TinyUSB behavior.
The standard fix is USBDEVFS_RESET (via the `usbreset` utility), which
reinitializes the entire USB device without a power-cycle.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import re
import time
from collections.abc import Callable
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .flash_job import FlashJob

log = logging.getLogger(__name__)

# tools/uf2_upload.py emits this exact line once the entire UF2 file has
# landed on the bootloader's MSC drive successfully — i.e., the bytes
# are on flash. Anything after this in the subprocess (verify-reboot
# wait, drive-disappear timer) is informational only; the write itself
# is done. Phase 7 of the flash-job rewrite stops treating subsequent
# timeouts as failures — closes the cascade bug from 2026-05-17.
_WRITE_COMPLETE_RE = re.compile(r"\[PASS\] Wrote ([\d,]+) ?/ ?([\d,]+) bytes")


def _find_uf2_tool() -> Path | None:
    """Find uf2_upload.py in the project."""
    p = Path(__file__).resolve()
    for _ in range(10):
        p = p.parent
        candidate = p / "tools" / "uf2_upload.py"
        if candidate.exists():
            return candidate
    return None


def _get_usb_dev_path(serial_port: str) -> str | None:
    """Get the /dev/bus/usb/BBB/DDD path for a serial port's USB device.

    Must be called while the port is still active (before transport disconnect).
    """
    import glob as _glob

    real_path = str(Path(serial_port).resolve())
    dev_name = Path(real_path).name

    for tty_dir in _glob.glob(f"/sys/class/tty/{dev_name}/device/../.."):
        try:
            p = Path(tty_dir).resolve()
            busnum = (p / "busnum").read_text().strip()
            devnum = (p / "devnum").read_text().strip()
            return f"/dev/bus/usb/{int(busnum):03d}/{int(devnum):03d}"
        except (ValueError, OSError):
            pass
    return None


def _find_usb_reset_tool() -> Path | None:
    """Find the usb_reset.py helper in tools/."""
    p = Path(__file__).resolve()
    for _ in range(10):
        p = p.parent
        candidate = p / "tools" / "usb_reset.py"
        if candidate.exists():
            return candidate
    return None


async def _usb_reset_device(usb_dev_path: str | None) -> bool:
    """Send USBDEVFS_RESET ioctl to reinitialize a USB device's stack.

    This reinitializes the device's USB stack (including TinyUSB CDC) without
    a power-cycle. Required after the server closes a serial connection,
    because pyserial's close drops DTR and TinyUSB enters a "no host" state
    that persists across subsequent port opens.

    Delegates to tools/usb_reset.py via sudo. The helper validates the path
    format (/dev/bus/usb/ prefix) and uses a narrow sudoers rule instead of
    blanket root Python access.
    """
    if not usb_dev_path:
        log.warning("No USB device path — cannot reset")
        return False

    if not Path(usb_dev_path).exists():
        log.warning("USB device path %s does not exist", usb_dev_path)
        return False

    tool = _find_usb_reset_tool()
    if not tool:
        log.warning("tools/usb_reset.py not found — cannot reset USB device")
        return False

    try:
        proc = await asyncio.create_subprocess_exec(
            "sudo",
            "python3",
            str(tool),
            usb_dev_path,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=10)
        output = stdout.decode(errors="replace").strip()
        if proc.returncode == 0 and "ok" in output:
            log.info("USB reset successful: %s", usb_dev_path)
            return True
        else:
            log.warning("USB reset failed (exit %d): %s", proc.returncode, output)
            return False
    except Exception as e:
        log.warning("USB reset failed: %s", e)
        return False


async def upload_uf2(
    serial_port: str,
    firmware_path: str,
    transport: Any,
    protocol: Any = None,
    progress_callback: Callable[..., None] | None = None,
) -> dict[str, Any]:
    """Upload firmware to an nRF52840 device via UF2.

    Disconnects the server transport, USB-resets the device to clear CDC
    state, then launches uf2_upload.py to handle bootloader entry, UF2
    detection, firmware copy, and verification.
    """
    t0 = time.monotonic()
    result = {"status": "error", "message": "", "elapsed_s": 0}

    def progress(phase: str, msg: str, pct: int | None = None) -> None:
        log.info("[FLASH %s] %s", phase, msg)
        if progress_callback is not None:
            progress_callback(phase, msg, pct)

    tool = _find_uf2_tool()
    if not tool:
        result["message"] = "uf2_upload.py not found in tools/"
        return result

    if not os.path.isfile(firmware_path):
        result["message"] = f"Firmware file not found: {firmware_path}"
        return result

    # === Phase 1: Capture USB device info, then release the serial port ===
    #
    # Look up paths BEFORE disconnecting — sysfs entries may become stale.
    usb_dev_path = _get_usb_dev_path(serial_port)
    if usb_dev_path:
        log.info("USB device path: %s", usb_dev_path)

    # Extract USB serial number from /dev/serial/by-id/ symlink name
    device_usb_sn = None
    by_id_dir = Path("/dev/serial/by-id")
    if by_id_dir.exists():
        real_port = str(Path(serial_port).resolve())
        for entry in by_id_dir.iterdir():
            if str(entry.resolve()) == real_port:
                parts = entry.name.rsplit("_", 1)
                if len(parts) == 2:
                    device_usb_sn = parts[1].split("-")[0]
                    log.info("Device USB serial: %s", device_usb_sn)
                break

    progress("prepare", "Releasing serial port...", 10)
    with contextlib.suppress(Exception):
        await transport.disconnect()

    # === Phase 2: USB-reset to reinitialize TinyUSB CDC ===
    #
    # After transport.disconnect(), the CDC is in a "no host" state.
    # USBDEVFS_RESET reinitializes the USB device, bringing the CDC back
    # to a clean state. This is the standard Linux mechanism — equivalent
    # to unplugging and replugging the USB cable.
    progress("prepare", "USB-resetting device for clean CDC state...", 15)
    usb_ok = await _usb_reset_device(usb_dev_path)
    if usb_ok:
        # Wait for the device to re-enumerate after USB reset.
        # The serial port path may change (e.g., ttyACM1 → ttyACM4).
        await asyncio.sleep(3)

        # Re-discover port using the stable /dev/serial/by-id/ path.
        # Match by the USB serial number we captured before disconnect.
        if device_usb_sn and by_id_dir.exists():
            for entry in by_id_dir.iterdir():
                if device_usb_sn in entry.name:
                    new_port = str(entry.resolve())
                    if new_port != serial_port:
                        log.info("Port changed after USB reset: %s -> %s", serial_port, new_port)
                    serial_port = new_port
                    break

    # === Phase 3: Run uf2_upload.py (handles bootloader entry + copy) ===
    progress("upload", f"Running uf2_upload.py on {serial_port}...", 20)
    cmd = [
        "python3",
        str(tool),
        "--hex",
        firmware_path,
        serial_port,
        "-v",
    ]

    try:
        # Stream stdout/stderr to the journal in real time AND collect into
        # `output` for the result message. Without the streaming half, a UF2
        # failure during the multi-minute copy phase shows up as a single
        # "UPLOAD FAILED" line in the journal with no breadcrumb — the actual
        # uf2_upload.py logs (where mount happened, what copy did, etc.) only
        # live in the API response. That makes triage from `journalctl -u`
        # impossible. Verified necessary 2026-05-16 after repeated cart_inner
        # UF2 failures couldn't be diagnosed from the journal alone.
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            cwd=str(tool.parent),
        )

        captured_lines: list[str] = []

        async def _drain() -> None:
            assert proc.stdout is not None
            while True:
                raw = await proc.stdout.readline()
                if not raw:
                    break
                line = raw.decode("utf-8", errors="replace").rstrip()
                if line:
                    log.info("[uf2_upload] %s", line)
                    captured_lines.append(line)

        try:
            await asyncio.wait_for(_drain(), timeout=300)
        except TimeoutError:
            log.warning("[uf2_upload] streaming hit 300s timeout; killing")
            proc.kill()
            raise

        # By here the subprocess should have closed stdout. `wait()` ensures
        # returncode is populated.
        await proc.wait()
        output = "\n".join(captured_lines)

        if proc.returncode == 0:
            progress("done", "Upload complete!", 100)
            elapsed = time.monotonic() - t0
            result.update(
                status="ok",
                message="Firmware uploaded successfully",
                elapsed_s=round(elapsed, 1),
                output=output[-500:],
            )
        else:
            error_lines = [line for line in captured_lines if "ERROR" in line or "FAILED" in line]
            msg = (
                error_lines[-1]
                if error_lines
                else (captured_lines[-1] if captured_lines else "Unknown error")
            )
            result["message"] = msg.strip()
            result["output"] = output[-500:]

    except TimeoutError:
        result["message"] = "Upload timed out after 300s"
    except Exception as e:
        result["message"] = f"Upload failed: {e}"

    result["elapsed_s"] = round(time.monotonic() - t0, 1)
    return result


# ─────────────────────────────────────────────────────────────────────
# Phase 7: FlashJob-aware UF2 path.
#
# Same physical steps as ``upload_uf2`` (disconnect → USB reset → run
# tools/uf2_upload.py) but the success criterion is "we saw [PASS] Wrote
# N / N bytes on stdout," NOT "the subprocess exited 0." The subprocess's
# trailing verify-reboot phase has a 60s wall-clock that fires reliably
# on the 0.8.0-4 bootloader — but the write itself has already succeeded
# by that point, and treating the subsequent timeout as a flash failure
# is what triggered the duplicate-flash cascade we lived through on
# 2026-05-17.
#
# Once the write-complete line is detected, this function:
#   1. Records progress on the job (transitions WRITING bytes counter)
#   2. Terminates the subprocess (we don't need its verify-wait)
#   3. Returns True — the caller (orchestrator) transitions to VERIFYING
#      and hands off to firmware.verify.run_verify, which observes the
#      actual device reboot + handshake on its own poll loop
# ─────────────────────────────────────────────────────────────────────


async def flash_uf2_for_job(
    job: FlashJob,
    serial_port: str,
    firmware_path: str,
    transport: Any,
) -> bool:
    """Write firmware to one device via UF2, bound to a ``FlashJob``.

    Pre-condition: ``job.state == WRITING`` (orchestrator transitioned it).
    Updates ``job.bytes_written`` / ``job.bytes_total`` as ``[PASS] Wrote
    N / N bytes`` appears on the subprocess's stdout.

    Returns True iff that line was seen — meaning the bootloader
    received and committed every UF2 block. False on any subprocess
    failure that happens *before* that line (file-not-found, MSC mount
    failure, bootloader entry failure, etc.).

    Critically, this function does NOT wait for the subprocess to exit
    cleanly. Once write-complete is seen, the subprocess is terminated
    so the orchestrator can begin verify polling without the spurious
    60s tail. The bootloader at that point has the new firmware on
    flash and proceeds to boot it independently of any host action.
    """
    from .flash_job import FlashJobState  # avoid top-level cycle

    assert job.state is FlashJobState.WRITING, (
        f"flash_uf2_for_job expected WRITING, got {job.state.value}"
    )

    tool = _find_uf2_tool()
    if not tool:
        job.set_error("uf2_upload.py not found in tools/")
        return False

    if not os.path.isfile(firmware_path):
        job.set_error(f"firmware file not found: {firmware_path}")
        return False

    # Same prep as upload_uf2: capture USB device path + serial number,
    # disconnect transport, USB-reset, re-discover the (possibly renamed)
    # serial port.
    usb_dev_path = _get_usb_dev_path(serial_port)
    device_usb_sn: str | None = None
    by_id_dir = Path("/dev/serial/by-id")
    if by_id_dir.exists():
        real_port = str(Path(serial_port).resolve())
        for entry in by_id_dir.iterdir():
            if str(entry.resolve()) == real_port:
                parts = entry.name.rsplit("_", 1)
                if len(parts) == 2:
                    device_usb_sn = parts[1].split("-")[0]
                break

    with contextlib.suppress(Exception):
        await transport.disconnect()

    if usb_dev_path:
        usb_ok = await _usb_reset_device(usb_dev_path)
        if usb_ok:
            await asyncio.sleep(3)
            if device_usb_sn and by_id_dir.exists():
                for entry in by_id_dir.iterdir():
                    if device_usb_sn in entry.name:
                        serial_port = str(entry.resolve())
                        break

    cmd = ["python3", str(tool), "--hex", firmware_path, serial_port, "-v"]
    log.info("flash_uf2_for_job %s: launching %s", job.job_id, " ".join(cmd))

    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
        cwd=str(tool.parent),
    )

    write_seen = False
    captured: list[str] = []

    async def _stream() -> None:
        nonlocal write_seen
        assert proc.stdout is not None
        while True:
            raw = await proc.stdout.readline()
            if not raw:
                return
            line = raw.decode("utf-8", errors="replace").rstrip()
            if not line:
                continue
            captured.append(line)
            log.info("[uf2_upload] %s", line)
            m = _WRITE_COMPLETE_RE.search(line)
            if m and not write_seen:
                write_seen = True
                # Strip thousands separators (the subprocess pretty-prints
                # the numbers) before converting.
                bytes_written = int(m.group(1).replace(",", ""))
                bytes_total = int(m.group(2).replace(",", ""))
                try:
                    job.record_progress(bytes_written, bytes_total)
                except Exception as exc:
                    # Don't let a progress-update bug derail the flash.
                    log.warning(
                        "flash_uf2_for_job: record_progress raised %s; continuing",
                        exc,
                    )
                log.info(
                    "flash_uf2_for_job %s: write complete (%d/%d) — "
                    "letting subprocess finish umount/eject; verify handled "
                    "by FlashJob, subprocess exit code ignored",
                    job.job_id,
                    bytes_written,
                    bytes_total,
                )
                # NOTE: do NOT terminate the subprocess here. Phase 7 live
                # test 2026-05-17 showed that on 0.8.0-4 bootloaders, the
                # umount/eject step the subprocess does AFTER this log
                # line is what signals the bootloader "transfer complete."
                # Killing here leaves the MSC drive mounted and the
                # bootloader stuck in MSC mode indefinitely. We let the
                # subprocess run to natural exit; its trailing
                # "wait-for-drive-to-disappear" will time out and exit
                # non-zero, but we ignore that exit code (the write itself
                # succeeded, which is all this function reports).
                # Continue draining stdout — the umount/eject logs are
                # useful to keep in the journal for diagnostics.

    try:
        await asyncio.wait_for(_stream(), timeout=300)
    except TimeoutError:
        # Capture write_seen state in the log line — a timeout AFTER
        # `[PASS] Wrote N/N bytes` still returns success from this
        # function (the write succeeded; the verify-wait phase of the
        # subprocess just couldn't confirm reboot in 60 s), and being
        # able to tell those two cases apart from journalctl alone
        # matters for post-mortems. PR 142 review (claude[bot] item #2).
        log.warning(
            "flash_uf2_for_job %s: stream hit 300s timeout (write_seen=%s)",
            job.job_id,
            write_seen,
        )
        with contextlib.suppress(ProcessLookupError):
            proc.kill()

    # Subprocess should have exited on its own (success path: ran to
    # completion of its umount/eject + verify-wait phases). Drain any
    # residual on best effort.
    with contextlib.suppress(asyncio.TimeoutError):
        await asyncio.wait_for(proc.wait(), timeout=10)

    if write_seen:
        # The subprocess may have exited non-zero (its own verify-wait
        # gave up at 60s — exactly the false-failure we're closing out).
        # We don't propagate that; the write succeeded.
        return True

    # No write-complete line → real failure. Surface the most actionable
    # message we captured.
    error_lines = [li for li in captured if "ERROR" in li or "FAILED" in li]
    msg = (
        error_lines[-1].strip()
        if error_lines
        else (captured[-1].strip() if captured else "UF2 subprocess produced no output")
    )
    job.set_error(f"UF2 write did not complete: {msg}")
    return False
