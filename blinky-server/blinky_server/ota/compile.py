"""Firmware compilation for OTA updates.

Compiles Arduino firmware and generates DFU packages.
Uses arduino-cli (must be installed and configured).
"""
from __future__ import annotations

import json
import logging
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path

log = logging.getLogger(__name__)

# Supported board FQBNs
FQBN_NRF52840 = "Seeeduino:nrf52:xiaonRF52840Sense"
FQBN_ESP32S3 = ("esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=default,"
                "MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240")

# Project paths (relative to blinky_time root)
SKETCH_DIR = "blinky-things"


def find_arduino_cli() -> str | None:
    """Find arduino-cli binary."""
    for path in ["/usr/local/bin/arduino-cli", "/usr/bin/arduino-cli",
                 str(Path.home() / "bin" / "arduino-cli")]:
        if Path(path).is_file():
            return path
    # Try PATH
    result = subprocess.run(["which", "arduino-cli"], capture_output=True, text=True)
    if result.returncode == 0:
        return result.stdout.strip()
    return None


def find_project_root() -> Path:
    """Find the blinky_time project root."""
    # Walk up from this file to find blinky-things/
    p = Path(__file__).resolve()
    for _ in range(10):
        p = p.parent
        if (p / SKETCH_DIR / f"{SKETCH_DIR}.ino").exists():
            return p
    raise FileNotFoundError("Cannot find blinky_time project root")


def compile_firmware(platform: str = "nrf52840",
                     ble_dfu_recovery: bool = True) -> dict:
    """Compile firmware for the given platform.

    Args:
        platform: "nrf52840" or "esp32s3"
        ble_dfu_recovery: If True, SafeBootWatchdog enters BLE DFU mode
            instead of UF2 on crash recovery. Use for physically installed
            devices without USB access. Default True for fleet management.

    Returns:
        dict with status, hex_path/bin_path, message
    """
    cli = find_arduino_cli()
    if not cli:
        return {"status": "error", "message": "arduino-cli not found"}

    root = find_project_root()

    if platform == "nrf52840":
        fqbn = FQBN_NRF52840
        build_dir = tempfile.mkdtemp(prefix="blinky-build-nrf-")
        hex_path = Path(build_dir) / f"{SKETCH_DIR}.ino.hex"
    elif platform == "esp32s3":
        fqbn = FQBN_ESP32S3
        build_dir = tempfile.mkdtemp(prefix="blinky-build-esp32-")
        hex_path = Path(build_dir) / f"{SKETCH_DIR}.ino.bin"
    else:
        return {"status": "error", "message": f"Unknown platform: {platform}"}

    # Delete any stale output file so we don't mistake it for a fresh build
    if hex_path.exists():
        hex_path.unlink()

    # Build command
    cmd = [cli, "compile", "--fqbn", fqbn, SKETCH_DIR,
           "--build-path", build_dir]

    # Enable BLE DFU recovery for fleet devices (nRF52840 only)
    if ble_dfu_recovery and platform == "nrf52840":
        cmd += ["--build-property",
                "compiler.cpp.extra_flags=-DSAFEBOOT_BLE_DFU_RECOVERY"]

    log.info("Compiling %s firmware (%s, ble_dfu_recovery=%s)...",
             platform, fqbn, ble_dfu_recovery)
    result = subprocess.run(
        cmd,
        capture_output=True, text=True, timeout=600,
        cwd=str(root),
    )

    # arduino-cli may return exit code 2 from post-build genpkg failure
    # but the hex is still generated — check for the output file
    if hex_path.exists():
        log.info("Compiled: %s (%d bytes)", hex_path, hex_path.stat().st_size)
        return {
            "status": "ok",
            "hex_path": str(hex_path),
            "build_dir": build_dir,
            "message": f"Compiled {platform} firmware",
        }
    else:
        return {
            "status": "error",
            "message": f"Compilation failed: {result.stderr[-500:]}",
        }


def _find_objcopy() -> str | None:
    """Find arm-none-eabi-objcopy for hex→bin conversion."""
    import glob as _glob
    # Check standard locations
    for pattern in [
        str(Path.home() / ".arduino15/packages/*/tools/arm-none-eabi-gcc/*/bin/arm-none-eabi-objcopy"),
        "/usr/bin/arm-none-eabi-objcopy",
    ]:
        matches = _glob.glob(pattern)
        if matches:
            return sorted(matches)[-1]  # Latest version
    result = subprocess.run(["which", "arm-none-eabi-objcopy"],
                            capture_output=True, text=True)
    if result.returncode == 0:
        return result.stdout.strip()
    return None


def _crc16(data: bytes) -> int:
    """CRC-16/CCITT for Nordic DFU init packet."""
    crc = 0xFFFF
    for byte in data:
        crc = ((crc >> 8) & 0xFF) | (crc << 8)
        crc ^= byte
        crc ^= (crc & 0xFF) >> 4
        crc ^= crc << 12
        crc ^= (crc & 0xFF) << 5
        crc &= 0xFFFF
    return crc


def generate_dfu_package(hex_path: str, sd_req: str = "0xFFFE") -> dict:
    """Generate a DFU zip package from firmware hex.

    Pure-Python implementation — avoids broken adafruit-nrfutil on Python 3.13.
    Uses arm-none-eabi-objcopy for hex→bin conversion, then builds the
    Nordic Legacy DFU zip format directly.

    Args:
        hex_path: Path to .hex firmware file
        sd_req: SoftDevice requirement (default: 0xFFFE = any SoftDevice)

    Returns:
        dict with status, zip_path, message
    """
    hex_file = Path(hex_path)
    if not hex_file.exists():
        return {"status": "error", "message": f"Hex file not found: {hex_path}"}

    zip_path = str(hex_file.with_suffix(".dfu.zip"))
    bin_path = str(hex_file.with_suffix(".bin"))

    # Convert hex → bin using objcopy
    objcopy = _find_objcopy()
    if not objcopy:
        return {"status": "error", "message": "arm-none-eabi-objcopy not found"}

    result = subprocess.run(
        [objcopy, "-I", "ihex", "-O", "binary", hex_path, bin_path],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode != 0:
        return {"status": "error", "message": f"objcopy failed: {result.stderr}"}

    firmware = Path(bin_path).read_bytes()
    if len(firmware) == 0:
        return {"status": "error", "message": "Converted bin file is empty"}

    # Build Nordic Legacy DFU init packet (SDK v11 format)
    # Format: device_type (uint16 LE) + device_rev (uint16 LE) +
    #         app_version (uint32 LE) + softdevice_count (uint16 LE) +
    #         sd_req[0] (uint16 LE) + CRC16 of firmware (uint16 LE)
    # ADAFRUIT_DEVICE_TYPE constant from bootloader's dfu_init.c — the
    # bootloader validates device_type == 0x0052 exactly. Verified working
    # end-to-end on Seeeduino nRF52840 bootloader v0.6.1 (Mar 30, 2026).
    device_type = 0x0052
    device_rev = 0xFFFF   # Any revision
    app_version = 0xFFFFFFFF  # Any version (no version check)
    sd_req_val = int(sd_req, 16)
    firmware_crc = _crc16(firmware)

    init_packet = struct.pack('<HHIHH',
                              device_type, device_rev, app_version,
                              1, sd_req_val)  # 1 = softdevice count
    init_packet += struct.pack('<H', firmware_crc)

    # Build the DFU zip
    dat_name = "application.dat"
    bin_name = "application.bin"
    manifest = {
        "manifest": {
            "application": {
                "bin_file": bin_name,
                "dat_file": dat_name,
                "init_packet_data": {
                    "application_version": 0xFFFFFFFF,
                    "device_revision": 0xFFFF,
                    "device_type": 0x0052,
                    "firmware_crc16": firmware_crc,
                    "softdevice_req": [sd_req_val],
                }
            }
        }
    }

    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(bin_name, firmware)
        zf.writestr(dat_name, init_packet)
        zf.writestr("manifest.json", json.dumps(manifest, indent=2))

    zip_size = Path(zip_path).stat().st_size
    log.info("DFU package: %s (%d bytes, firmware %d bytes, CRC 0x%04X)",
             zip_path, zip_size, len(firmware), firmware_crc)
    return {"status": "ok", "zip_path": zip_path, "message": "DFU package generated"}


def ensure_dfu_zip(firmware_path: str) -> str:
    """Ensure firmware_path is a .dfu.zip, generating one from .hex if needed.

    Returns path to the .dfu.zip file.
    Raises ValueError if the file can't be used for BLE DFU.
    """
    p = Path(firmware_path)
    if p.suffix == ".zip":
        return firmware_path
    if p.suffix == ".hex":
        result = generate_dfu_package(firmware_path)
        if result["status"] != "ok":
            raise ValueError(f"Failed to generate DFU zip from {firmware_path}: {result['message']}")
        return result["zip_path"]
    raise ValueError(
        f"BLE DFU requires .dfu.zip or .hex file, got: {p.name}. "
        f"Use POST /ota/compile-dfu to generate one."
    )
