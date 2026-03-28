"""UF2 firmware upload for nRF52840 devices connected via serial.

Called by blinky-server when it already owns the serial connection.
No port contention — the server disconnects its transport, enters
bootloader, copies firmware, and reconnects.
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
BOOTLOADER_ENTRY_TIMEOUT = 15  # seconds to wait for UF2 drive
UF2_REBOOT_TIMEOUT = 15  # seconds to wait for device to reboot after flash


def _find_uf2_drive(timeout: float = 10.0) -> Path | None:
    """Wait for a UF2 mass storage drive to appear."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # Check by label first (most reliable)
        label_path = Path("/dev/disk/by-label/XIAO-SENSE")
        if label_path.exists():
            return label_path.resolve()
        # Fallback: check /sys/block for UF2 devices
        for dev in Path("/sys/block").iterdir():
            if dev.name.startswith("sd"):
                model = (dev / "device" / "model").read_text().strip() \
                    if (dev / "device" / "model").exists() else ""
                if "UF2" in model or "nRF" in model:
                    return Path(f"/dev/{dev.name}")
        time.sleep(0.3)
    return None


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


def _wait_uf2_gone(timeout: float = 15.0) -> bool:
    """Wait for UF2 drive to disappear (device rebooted)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not Path("/dev/disk/by-label/XIAO-SENSE").exists():
            return True
        time.sleep(0.3)
    return False


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
    send_command: callable,
    disconnect: callable,
    progress_callback: callable | None = None,
) -> dict:
    """Upload firmware to an nRF52840 device via UF2.

    Args:
        serial_port: The serial port path (e.g., /dev/ttyACM3)
        firmware_path: Path to .hex or .uf2 firmware file
        send_command: async callable to send serial command (from DeviceProtocol)
        disconnect: async callable to disconnect the transport
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

    progress("bootloader", "Sending bootloader command...", 10)
    try:
        # Send bootloader command via the server's existing serial connection.
        # The device will print "[OK] GPREGRET=0x57" and then reset.
        resp = await send_command("bootloader")
        if "GPREGRET" not in resp and "bootloader" not in resp.lower():
            log.warning("Unexpected bootloader response: %s", resp)
    except Exception as e:
        # The device may disconnect before we read the response — that's OK
        log.debug("Bootloader command result (may disconnect): %s", e)

    progress("bootloader", "Disconnecting transport...", 15)
    await disconnect()

    # Wait a moment for USB re-enumeration
    await asyncio.sleep(3)

    progress("detect", "Waiting for UF2 drive...", 20)
    block_dev = await asyncio.to_thread(_find_uf2_drive, BOOTLOADER_ENTRY_TIMEOUT)
    if not block_dev:
        result["message"] = "UF2 drive not detected — bootloader entry may have failed"
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
    rebooted = await asyncio.to_thread(_wait_uf2_gone, UF2_REBOOT_TIMEOUT)
    if not rebooted:
        result["message"] = "Device did not reboot after firmware copy"
        return result

    progress("done", "Upload complete!", 100)
    elapsed = time.monotonic() - t0
    result.update(status="ok", message="Firmware uploaded successfully",
                  elapsed_s=round(elapsed, 1))
    return result
