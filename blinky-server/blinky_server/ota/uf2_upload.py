"""UF2 firmware upload for nRF52840 devices connected via serial.

Called by blinky-server when it already owns the serial connection.
No port contention — the server disconnects its transport, enters
bootloader, copies firmware, and reconnects.

Event-driven: uses pyudev to listen for USB device events instead of
polling with sleep loops. Reacts immediately when hardware state changes.
"""
from __future__ import annotations

import asyncio
import logging
import os
import shutil
import subprocess
import time
from pathlib import Path

log = logging.getLogger(__name__)

UF2_FAMILY_NRF52840 = 0xADA52840


async def _wait_for_uf2_drive(timeout: float = 30.0) -> Path | None:
    """Wait for UF2 mass storage drive using udev events.

    Listens for block device add events instead of polling. Returns
    immediately when the drive appears, or None on timeout.
    """
    # Check if already present
    label_path = Path("/dev/disk/by-label/XIAO-SENSE")
    if label_path.exists():
        return label_path.resolve()

    loop = asyncio.get_event_loop()
    found = asyncio.Event()
    result_path: list[Path] = []

    def _monitor_thread():
        """Run pyudev monitor in a thread (it blocks)."""
        try:
            import pyudev
            context = pyudev.Context()
            monitor = pyudev.Monitor.from_netlink(context)
            monitor.filter_by(subsystem='block', device_type='disk')
            monitor.start()

            deadline = time.monotonic() + timeout
            for device in iter(monitor.poll, None):
                if time.monotonic() > deadline:
                    break
                if device.action == 'add':
                    # Check if this is the UF2 drive
                    label = device.get('ID_FS_LABEL', '')
                    model = device.get('ID_MODEL', '')
                    if label == 'XIAO-SENSE' or 'UF2' in model:
                        dev_path = Path(device.device_node)
                        result_path.append(dev_path)
                        loop.call_soon_threadsafe(found.set)
                        return
        except ImportError:
            # pyudev not available — fall back to polling
            log.debug("pyudev not available, falling back to polling")
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if label_path.exists():
                    result_path.append(label_path.resolve())
                    loop.call_soon_threadsafe(found.set)
                    return
                time.sleep(0.5)

    thread_task = asyncio.to_thread(_monitor_thread)

    try:
        # Wait for either the event or timeout
        done, pending = await asyncio.wait(
            [asyncio.create_task(found.wait()), thread_task],
            timeout=timeout,
            return_when=asyncio.FIRST_COMPLETED,
        )
        for t in pending:
            t.cancel()
    except Exception:
        pass

    return result_path[0] if result_path else None


async def _wait_for_uf2_gone(timeout: float = 30.0) -> bool:
    """Wait for UF2 drive to disappear (device rebooted) using udev events."""
    label_path = Path("/dev/disk/by-label/XIAO-SENSE")
    if not label_path.exists():
        return True

    loop = asyncio.get_event_loop()
    gone = asyncio.Event()

    def _monitor_thread():
        try:
            import pyudev
            context = pyudev.Context()
            monitor = pyudev.Monitor.from_netlink(context)
            monitor.filter_by(subsystem='block', device_type='disk')
            monitor.start()

            deadline = time.monotonic() + timeout
            for device in iter(monitor.poll, None):
                if time.monotonic() > deadline:
                    break
                if device.action == 'remove':
                    loop.call_soon_threadsafe(gone.set)
                    return
        except ImportError:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if not label_path.exists():
                    loop.call_soon_threadsafe(gone.set)
                    return
                time.sleep(0.5)

    thread_task = asyncio.to_thread(_monitor_thread)

    try:
        done, pending = await asyncio.wait(
            [asyncio.create_task(gone.wait()), thread_task],
            timeout=timeout,
            return_when=asyncio.FIRST_COMPLETED,
        )
        for t in pending:
            t.cancel()
    except Exception:
        pass

    return gone.is_set()


def _mount_uf2(block_dev: Path) -> Path:
    """Mount UF2 block device, return mount point."""
    mount_point = Path("/tmp/blinky-uf2-upload")
    mount_point.mkdir(exist_ok=True)

    # Try udisksctl first (no sudo needed if available)
    try:
        result = subprocess.run(
            ["udisksctl", "mount", "-b", str(block_dev)],
            capture_output=True, text=True, timeout=10
        )
        if result.returncode == 0:
            # Parse mount point from output: "Mounted /dev/sda at /media/..."
            for word in result.stdout.split():
                if word.startswith("/media/") or word.startswith("/run/media/"):
                    return Path(word.rstrip("."))
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass

    # Fallback: sudo mount with user-friendly permissions
    subprocess.run(
        ["sudo", "mount", "-o", "uid=1000,gid=1000", str(block_dev), str(mount_point)],
        capture_output=True, timeout=10, check=True
    )
    return mount_point


def _unmount_uf2(mount_point: Path) -> None:
    """Unmount UF2 drive."""
    subprocess.run(["sudo", "umount", str(mount_point)],
                   capture_output=True, timeout=10)



def _find_uf2conv() -> Path | None:
    """Find uf2conv.py in Arduino packages or project tools."""
    search_paths = [
        # Arduino Seeeduino core (installed via arduino-cli)
        Path.home() / ".arduino15" / "packages" / "Seeeduino" / "hardware" /
        "nrf52" / "1.1.12" / "tools" / "uf2conv" / "uf2conv.py",
        # Project tools directory
        Path(__file__).parent.parent.parent.parent / "tools" / "uf2conv.py",
    ]
    for p in search_paths:
        if p.exists():
            return p
    return None


def hex_to_uf2(hex_path: str, uf2_path: str) -> bool:
    """Convert Intel HEX to UF2."""
    uf2conv = _find_uf2conv()
    if not uf2conv:
        log.error("uf2conv.py not found in Arduino packages or tools/")
        return False

    result = subprocess.run(
        ["python3", str(uf2conv), hex_path, "-c", "-f", hex(UF2_FAMILY_NRF52840),
         "-o", uf2_path],
        capture_output=True, text=True, timeout=30
    )
    if result.returncode != 0:
        log.error("UF2 conversion failed: %s", result.stderr)
        return False
    return True


async def upload_uf2(
    serial_port: str,
    firmware_path: str,
    transport,
    protocol=None,
    progress_callback: callable | None = None,
) -> dict:
    """Upload firmware to an nRF52840 device via UF2.

    Args:
        serial_port: The serial port path (e.g., /dev/ttyACM3)
        firmware_path: Path to .hex or .uf2 firmware file
        transport: The serial Transport instance
        protocol: The DeviceProtocol instance (for send_command)
        progress_callback: optional callable(phase, message, pct) for progress

    Returns:
        dict with status, message, and timing info
    """
    t0 = time.monotonic()
    result = {"status": "error", "message": "", "elapsed_s": 0}

    def progress(phase, msg, pct=None):
        log.info("[OTA %s] %s", phase, msg)
        if progress_callback:
            progress_callback(phase, msg, pct)

    # Determine firmware format
    fw_path = Path(firmware_path)
    if fw_path.suffix == ".hex":
        uf2_path = str(fw_path.with_suffix(".uf2"))
        progress("convert", "Converting HEX to UF2...")
        if not hex_to_uf2(firmware_path, uf2_path):
            result["message"] = "HEX to UF2 conversion failed"
            return result
        firmware_path = uf2_path
    elif fw_path.suffix != ".uf2":
        result["message"] = f"Unsupported firmware format: {fw_path.suffix}"
        return result

    # Validate UF2 file exists and has reasonable size
    fw_size = os.path.getsize(firmware_path)
    if fw_size < 1000 or fw_size > 2_000_000:
        result["message"] = f"Firmware size suspicious: {fw_size} bytes"
        return result

    # Ensure transport is connected
    if not transport.is_connected:
        progress("bootloader", "Reconnecting transport...", 8)
        try:
            await asyncio.wait_for(transport.connect(), timeout=10)
        except Exception as e:
            result["message"] = f"Cannot connect to device: {e}"
            return result

    # GPREGRET persistence is intermittent (~20-50% success) due to a race
    # with the SoftDevice shutdown. Retry: send "bootloader", listen for
    # UF2 drive via udev events, reconnect if needed.
    max_attempts = 5
    block_dev = None

    for attempt in range(1, max_attempts + 1):
        progress("bootloader", f"Attempt {attempt}/{max_attempts}...", 10)

        # Start listening for UF2 drive BEFORE sending the command.
        # This way we don't miss fast appearances.
        uf2_task = asyncio.create_task(_wait_for_uf2_drive(timeout=15))

        # Send bootloader command
        try:
            if protocol:
                try:
                    resp = await protocol.send_command("bootloader", timeout=3.0)
                    log.info("Attempt %d: %s", attempt,
                             resp[:80] if resp else "(no response)")
                except Exception as e:
                    log.info("Attempt %d: device reset (%s)", attempt,
                             type(e).__name__)
            else:
                await transport.write_line("bootloader")
        except Exception as e:
            log.warning("Attempt %d: send failed: %s", attempt, e)
            uf2_task.cancel()
            break

        # Wait for UF2 drive event (no polling — pyudev listener or fallback)
        block_dev = await uf2_task
        if block_dev:
            log.info("UF2 drive appeared on attempt %d: %s", attempt, block_dev)
            break

        log.info("Attempt %d: no UF2 drive", attempt)
        if attempt < max_attempts:
            # Reconnect transport for next attempt
            try:
                await transport.disconnect()
            except Exception:
                pass
            await asyncio.sleep(1)
            try:
                await asyncio.wait_for(transport.connect(), timeout=10)
            except Exception as e:
                log.warning("Reconnect: %s", e)

    if not block_dev:
        result["message"] = (f"Bootloader entry failed after {max_attempts} attempts "
                             "(GPREGRET race condition)")
        return result

    progress("mount", "Mounting UF2 drive...", 30)
    try:
        mount_point = await asyncio.to_thread(_mount_uf2, block_dev)
    except Exception as e:
        result["message"] = f"Failed to mount UF2 drive: {e}"
        return result

    # Verify it's actually a UF2 bootloader drive
    info_file = mount_point / "INFO_UF2.TXT"
    if not info_file.exists():
        result["message"] = "Mounted drive is not a UF2 bootloader"
        await asyncio.to_thread(_unmount_uf2, mount_point)
        return result

    progress("flash", f"Copying firmware ({fw_size} bytes)...", 50)
    try:
        dest = mount_point / Path(firmware_path).name
        shutil.copy2(firmware_path, dest)
        os.sync()
    except Exception as e:
        result["message"] = f"Firmware copy failed: {e}"
        await asyncio.to_thread(_unmount_uf2, mount_point)
        return result

    progress("reboot", "Waiting for device to reboot...", 80)
    rebooted = await _wait_for_uf2_gone(timeout=30)
    if not rebooted:
        result["message"] = "Device did not reboot after firmware copy"
        return result

    progress("done", "Upload complete!", 100)
    elapsed = time.monotonic() - t0
    result.update(status="ok", message="Firmware uploaded successfully",
                  elapsed_s=round(elapsed, 1))
    return result
