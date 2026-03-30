"""Firmware compilation for OTA updates.

Compiles Arduino firmware and generates DFU packages.
Uses arduino-cli (must be installed and configured).
"""
from __future__ import annotations

import logging
import subprocess
import tempfile
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


def generate_dfu_package(hex_path: str, sd_req: str = "0xFFFE") -> dict:
    """Generate a DFU zip package from firmware hex/bin.

    Uses adafruit-nrfutil genpkg for nRF52840.

    Args:
        hex_path: Path to .hex firmware file
        sd_req: SoftDevice requirement (default: 0xFFFE = any SoftDevice)

    Returns:
        dict with status, zip_path, message
    """
    zip_path = str(Path(hex_path).with_suffix(".dfu.zip"))

    result = subprocess.run(
        ["adafruit-nrfutil", "dfu", "genpkg",
         "--application", hex_path,
         "--sd-req", sd_req,
         zip_path],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode != 0:
        return {"status": "error", "message": f"genpkg failed: {result.stderr}"}

    log.info("DFU package: %s", zip_path)
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
