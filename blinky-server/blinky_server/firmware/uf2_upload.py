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
import logging
import os
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)


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


async def _usb_reset_device(usb_dev_path: str | None) -> bool:
    """Send USBDEVFS_RESET ioctl to reinitialize a USB device's stack.

    This reinitializes the device's USB stack (including TinyUSB CDC) without
    a power-cycle. Required after the server closes a serial connection,
    because pyserial's close drops DTR and TinyUSB enters a "no host" state
    that persists across subsequent port opens.

    Uses the USBDEVFS_RESET ioctl directly (more reliable than the usbreset
    command-line tool which has device-lookup issues).

    Requires root/sudo for /dev/bus/usb access.
    """
    if not usb_dev_path:
        log.warning("No USB device path — cannot reset")
        return False

    if not Path(usb_dev_path).exists():
        log.warning("USB device path %s does not exist", usb_dev_path)
        return False

    # USBDEVFS_RESET ioctl requires root. Run via sudo python3 one-liner.
    USBDEVFS_RESET = 21780
    script = (
        f"import fcntl,os; fd=os.open('{usb_dev_path}',os.O_WRONLY); "
        f"fcntl.ioctl(fd,{USBDEVFS_RESET},0); os.close(fd); print('ok')"
    )
    try:
        proc = await asyncio.create_subprocess_exec(
            "sudo", "python3", "-c", script,
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
    try:
        await transport.disconnect()
    except Exception:
        pass

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
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            cwd=str(tool.parent),
        )
        stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=300)
        output = stdout.decode("utf-8", errors="replace")

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
            lines = output.strip().split("\n")
            error_lines = [line for line in lines if "ERROR" in line or "FAILED" in line]
            msg = error_lines[-1] if error_lines else lines[-1] if lines else "Unknown error"
            result["message"] = msg.strip()
            result["output"] = output[-500:]

    except TimeoutError:
        result["message"] = "Upload timed out after 300s"
    except Exception as e:
        result["message"] = f"Upload failed: {e}"

    result["elapsed_s"] = round(time.monotonic() - t0, 1)
    return result
