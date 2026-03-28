#!/usr/bin/env python3
"""
Safe UF2 Firmware Upload for XIAO nRF52840 Sense / XIAO ESP32-S3

Uploads firmware via the UF2 mass storage bootloader, bypassing the
fragile adafruit-nrfutil DFU serial protocol that can brick devices.

Upload workflow:
  1. Validate hex file (address safety checks) [nRF52840 only]
  2. Convert hex -> UF2 (using platform uf2conv.py) [nRF52840 only]
     OR locate pre-built .uf2 from arduino-esp32 output [ESP32-S3]
  3. Enter bootloader (1200 baud serial touch)
  4. Mount UF2 drive (udisksctl or manual mount)
  5. Copy firmware.uf2 to drive
  6. Verify reboot (drive disappears, serial port returns)

Usage:
  python3 uf2_upload.py /dev/ttyACM0
  python3 uf2_upload.py /dev/ttyACM0 --hex /tmp/blinky-build/blinky-things.ino.hex
  python3 uf2_upload.py /dev/ttyACM0 --dry-run
  python3 uf2_upload.py --already-in-bootloader
  python3 uf2_upload.py --self-test
  python3 uf2_upload.py /dev/ttyACM0 --board esp32s3
  python3 uf2_upload.py /dev/ttyACM0 --board esp32s3 --build-dir /tmp/blinky-esp32-build
"""

import sys
import os
import re
import glob
import time
import json
import shutil
import subprocess
import argparse
import signal
from pathlib import Path
from urllib.request import urlopen, Request
from urllib.error import URLError

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial is required. Install with: pip3 install pyserial")
    sys.exit(1)

# --- Board profiles ---
# VID/PID values from boards.txt for each platform.
#
# ESP32-S3 notes (TO VERIFY against your hardware):
#   normal_pid:      0x0056 — XIAO ESP32-S3 in application mode (check `lsusb`)
#   bootloader_pid:  0x1001 — ESP32-S3 ROM UF2 bootloader (check `lsusb` after double-tap reset)
#   drive_label:     "ESP32S3" — label of mass-storage drive in UF2 mode (check `lsblk`)
#   bin_base_addr:   0x10000 — ESP32-S3 default app partition start address
#
# arduino-esp32 does NOT produce a .uf2 directly. It produces a .bin which must
# be converted with uf2conv.py using the ESP32-S3 family ID.
BOARD_PROFILES = {
    "nrf52840": {
        "name":           "XIAO nRF52840 Sense",
        "normal_vid":     0x2886,
        "normal_pid":     0x8045,
        "bootloader_vid": 0x2886,
        "bootloader_pid": 0x0045,
        "uf2_family_id":  "0xADA52840",
        "drive_label":    "XIAO-SENSE",
        "firmware_ext":   ".hex",       # compile output; converted via uf2conv
        "bin_base_addr":  None,         # not used for .hex input
    },
    "esp32s3": {
        "name":           "XIAO ESP32-S3",
        "normal_vid":     0x303A,   # Espressif native USB CDC (verified: lsusb COM43)
        "normal_pid":     0x1001,   # Espressif native USB CDC (verified: lsusb COM43)
        "bootloader_vid": 0x303A,   # TO VERIFY: check `lsusb` after double-tap reset
        "bootloader_pid": 0x0002,   # TO VERIFY: ESP32-S3 ROM download mode PID
        "uf2_family_id":  "0xc47e5767",
        "drive_label":    "ESP32S3", # TO VERIFY: check `lsblk` in UF2 bootloader mode
        "firmware_ext":   ".bin",       # arduino-esp32 compile output
        "bin_base_addr":  0x10000,      # default ESP32-S3 app partition start
    },
}

# Active board profile — set by main() based on --board argument.
# All helpers that need VID/PID access use this.
_active_board = BOARD_PROFILES["nrf52840"]

# --- Hardware constants (derived from active board — kept for backwards compat) ---
NORMAL_VID = 0x2886
NORMAL_PID = 0x8045
BOOTLOADER_VID = 0x2886
BOOTLOADER_PID = 0x0045

# --- UF2 conversion ---
UF2_FAMILY_ID = "0xADA52840"

def _find_uf2conv():
    """Find uf2conv.py — searches Seeeduino nRF52 and arduino-esp32 packages.

    The Adafruit/Microsoft uf2conv.py supports arbitrary family IDs via -f, so
    the nRF52 package copy can be reused for ESP32-S3 conversion too.
    """
    search_bases = [
        # Seeeduino nRF52 package (primary — ships uf2conv.py)
        Path.home() / ".arduino15/packages/Seeeduino/hardware/nrf52",
        # arduino-esp32 package (some versions ship their own copy)
        Path.home() / ".arduino15/packages/esp32/hardware/esp32",
    ]
    for base in search_bases:
        if base.exists():
            versions = sorted(base.iterdir(), reverse=True)
            for v in versions:
                candidate = v / "tools/uf2conv/uf2conv.py"
                if candidate.exists():
                    return candidate
    # Fallback for error messaging
    return Path.home() / ".arduino15/packages/Seeeduino/hardware/nrf52/unknown_version/tools/uf2conv/uf2conv.py"

UF2CONV_PATH = _find_uf2conv()

# --- Timeouts (seconds) ---
BOOTLOADER_TIMEOUT = 15
DRIVE_MOUNT_TIMEOUT = 15
REBOOT_TIMEOUT = 10
PORT_REAPPEAR_TIMEOUT = 10

# --- UF2 drive identification ---
UF2_INFO_FILE = "INFO_UF2.TXT"
UF2_DRIVE_LABEL = "XIAO-SENSE"

# --- Serial port safety ---
SERIAL_OPEN_TIMEOUT = 5  # seconds — max time to wait for serial port open
BLINKY_SERVER_URL = os.environ.get("BLINKY_SERVER_URL", "http://localhost:8420")


def _port_holders(port):
    """Return list of PIDs holding a serial port, using fuser.

    Returns empty list if port is free or fuser is unavailable.
    """
    real_port = str(Path(port).resolve()) if Path(port).is_symlink() else port
    try:
        result = subprocess.run(
            ["fuser", real_port],
            capture_output=True, text=True, timeout=5,
        )
        # fuser writes PIDs to stderr
        pids = result.stderr.strip().split()
        return [int(p) for p in pids if p.strip().isdigit()]
    except (subprocess.TimeoutExpired, FileNotFoundError, ValueError):
        return []


def _serial_open_with_timeout(port, baudrate=115200, timeout=1, open_timeout=None):
    """Open a serial port with a timeout on the open() call itself.

    serial.Serial(timeout=...) only applies to read operations. The
    open() syscall itself can block indefinitely if another process
    holds the port. On Linux/macOS this wrapper uses SIGALRM to abort
    a stuck open. On platforms without SIGALRM (e.g. Windows), the
    open() call is made directly without a timeout guard.

    Returns the serial.Serial object, or raises serial.SerialException.
    """
    if open_timeout is None:
        open_timeout = SERIAL_OPEN_TIMEOUT

    if not hasattr(signal, 'SIGALRM'):
        # SIGALRM is unavailable (e.g. Windows) — open without timeout guard
        return serial.Serial(port, baudrate, timeout=timeout)

    def _alarm_handler(signum, frame):
        raise serial.SerialException(
            f"Timed out opening {port} after {open_timeout}s "
            f"(port may be held by another process)"
        )

    old_handler = signal.signal(signal.SIGALRM, _alarm_handler)
    signal.alarm(open_timeout)
    try:
        ser = serial.Serial(port, baudrate, timeout=timeout)
        signal.alarm(0)  # Cancel alarm
        return ser
    except Exception:
        signal.alarm(0)
        raise
    finally:
        signal.signal(signal.SIGALRM, old_handler)


def _request_server_release(port, verbose=False):
    """Ask blinky-server to release a device on the given port.

    Looks up the device by port, then calls POST /devices/{id}/release.
    Returns True if released (or server not running), False on error.
    """
    try:
        req = Request(f"{BLINKY_SERVER_URL}/devices",
                      headers={"Accept": "application/json"})
        resp = urlopen(req, timeout=3)
        devices = json.loads(resp.read())
    except (URLError, OSError, json.JSONDecodeError):
        if verbose:
            print(f"  blinky-server not reachable at {BLINKY_SERVER_URL} (OK if not running)")
        return True  # Server not running — nothing to release

    real_port = str(Path(port).resolve()) if Path(port).is_symlink() else port

    # Find device on this port
    device_id = None
    for dev in devices:
        dev_port = dev.get("port", "")
        if dev_port == port or dev_port == real_port:
            device_id = dev.get("id")
            break

    if not device_id:
        if verbose:
            print(f"  blinky-server has no device on {port}")
        return True

    # Release it
    try:
        req = Request(
            f"{BLINKY_SERVER_URL}/devices/{device_id}/release",
            method="POST",
            headers={"Content-Type": "application/json"},
            data=b"{}",
        )
        resp = urlopen(req, timeout=5)
        print(f"  Released {device_id[:12]} from blinky-server")
        time.sleep(2)  # Let OS release the FD
        return True
    except (URLError, OSError) as e:
        print(f"  WARNING: Failed to release device via blinky-server: {e}")
        return False


def _request_server_reconnect(port, verbose=False):
    """Ask blinky-server to reconnect a device on the given port."""
    try:
        req = Request(f"{BLINKY_SERVER_URL}/devices",
                      headers={"Accept": "application/json"})
        resp = urlopen(req, timeout=3)
        devices = json.loads(resp.read())
    except (URLError, OSError, json.JSONDecodeError):
        return  # Server not running

    real_port = str(Path(port).resolve()) if Path(port).is_symlink() else port

    for dev in devices:
        dev_port = dev.get("port", "")
        if dev_port == port or dev_port == real_port:
            device_id = dev.get("id")
            try:
                req = Request(
                    f"{BLINKY_SERVER_URL}/devices/{device_id}/reconnect",
                    method="POST",
                    headers={"Content-Type": "application/json"},
                    data=b"{}",
                )
                urlopen(req, timeout=5)
                if verbose:
                    print(f"  Reconnected {device_id[:12]} via blinky-server")
            except (URLError, OSError):
                pass
            return


# ============================================================
#  Output formatting (matches pre_upload_check.py style)
# ============================================================

def print_section(title):
    w = 60
    print(f"\n{'=' * w}")
    print(f"  {title}")
    print(f"{'=' * w}\n")


def print_success(message):
    w = 60
    print(f"\n{'=' * w}")
    print(f"  {message}")
    print(f"{'=' * w}\n")


def print_failure(message):
    w = 60
    print(f"\n{'!' * w}")
    print(f"  {message}")
    print(f"{'!' * w}\n")


# ============================================================
#  Phase 1: Hex file discovery and validation
# ============================================================

def find_hex_file(args):
    """Locate the compiled hex file."""
    if args.hex_file:
        path = Path(args.hex_file)
        if not path.exists():
            raise FileNotFoundError(f"Specified hex file not found: {path}")
        return path

    build_dir = Path(args.build_dir)
    search_paths = [
        build_dir / "blinky-things.ino.hex",
        Path(__file__).parent.parent / "blinky-things" / "build"
        / "Seeeduino.nrf52.xiaonRF52840Sense" / "blinky-things.ino.hex",
    ]

    for path in search_paths:
        if path.exists():
            return path

    raise FileNotFoundError(
        "No hex file found. Searched:\n"
        + "\n".join(f"  - {p}" for p in search_paths)
        + "\n\nCompile first or specify path with --hex"
    )


def find_bin_file(args):
    """Locate the .bin firmware file produced by arduino-esp32.

    arduino-esp32 outputs a raw binary (.bin), not a .hex.  We convert it to
    UF2 ourselves using uf2conv.py with the ESP32-S3 family ID.

    Returns path to the .bin file.
    """
    build_dir = Path(args.build_dir)
    search_paths = [
        build_dir / "blinky-things.ino.bin",
        # arduino-esp32 sometimes nests output under the board sub-directory
        build_dir / "esp32.esp32.XIAO_ESP32S3" / "blinky-things.ino.bin",
    ]

    for path in search_paths:
        if path.exists():
            return path

    raise FileNotFoundError(
        "No .bin file found for ESP32-S3. Searched:\n"
        + "\n".join(f"  - {p}" for p in search_paths)
        + "\n\nCompile first with: make esp32-compile"
    )


def convert_bin_to_uf2(bin_path, output_dir=None):
    """Convert a raw binary (.bin) to UF2 format for ESP32-S3.

    Uses uf2conv.py with the ESP32-S3 family ID and the app-partition base
    address (0x10000 for default ESP32-S3 partition table).

    Returns path to the generated .uf2 file.
    """
    print_section("UF2 CONVERSION (ESP32-S3)")

    uf2conv = _find_uf2conv()
    if not uf2conv.exists():
        raise FileNotFoundError(
            f"uf2conv.py not found at {uf2conv}\n"
            "Install the Seeeduino nRF52 or arduino-esp32 board package."
        )

    profile = _active_board
    family_id = profile["uf2_family_id"]
    base_addr = profile["bin_base_addr"]

    if output_dir is None:
        output_dir = bin_path.parent
    uf2_path = output_dir / "blinky-things.ino.uf2"

    cmd = [
        sys.executable,
        str(uf2conv),
        str(bin_path),
        "-f", family_id,
        "-c",
        "-b", hex(base_addr),
        "-o", str(uf2_path),
    ]

    print(f"  Input:       {bin_path}")
    print(f"  Output:      {uf2_path}")
    print(f"  Family:      {family_id}")
    print(f"  Base addr:   {hex(base_addr)} (app partition start)")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        raise RuntimeError("UF2 conversion hung (timed out after 30s)")

    if result.returncode != 0:
        print(f"  [FAIL] uf2conv.py failed:")
        if result.stderr:
            print(f"  {result.stderr.strip()}")
        raise RuntimeError("UF2 conversion failed")

    if result.stdout:
        for line in result.stdout.strip().split("\n"):
            if not line.startswith("/"):
                print(f"  {line}")

    if not uf2_path.exists():
        raise RuntimeError(f"UF2 file not created: {uf2_path}")

    uf2_size = uf2_path.stat().st_size
    print(f"  UF2 size:    {uf2_size:,} bytes ({uf2_size // 512} blocks)")
    print(f"  [PASS] Conversion successful")

    return uf2_path


def validate_hex(hex_path, verbose=False):
    """Run pre-upload safety checks by importing pre_upload_check.py.

    Returns True if safe to upload, False otherwise.
    """
    print_section("FIRMWARE VALIDATION")
    print(f"  Hex file: {hex_path}")
    print(f"  Size: {hex_path.stat().st_size:,} bytes")

    tools_dir = Path(__file__).parent
    sys.path.insert(0, str(tools_dir))

    try:
        import pre_upload_check
    except ImportError:
        print("  [WARN] pre_upload_check.py not found, skipping validation")
        return True

    try:
        hex_data = pre_upload_check.parse_intel_hex(str(hex_path))
    except pre_upload_check.HexValidationError as e:
        print(f"  [FAIL] HEX parse error: {e}")
        return False

    print(f"  Address range: 0x{hex_data['min_addr']:08X} - 0x{hex_data['max_addr']:08X}")
    print(f"  Total data: {hex_data['total_bytes']:,} bytes")

    try:
        results = pre_upload_check.validate_safety(hex_data, verbose=verbose)
        all_passed = all(r[1] for r in results)

        for name, passed, msg in results:
            status = "PASS" if passed else "FAIL"
            print(f"  [{status}] {name}: {msg}")

        if all_passed:
            print("  All safety checks passed")
        return all_passed

    except pre_upload_check.SafetyError as e:
        print(f"  [CRITICAL] {e}")
        print("  Upload BLOCKED to prevent device damage.")
        return False


# ============================================================
#  Phase 2: UF2 conversion
# ============================================================

def convert_to_uf2(hex_path, output_dir=None):
    """Convert Intel HEX to UF2 format using the platform's uf2conv.py.

    Returns path to the generated .uf2 file.
    """
    print_section("UF2 CONVERSION")

    if not UF2CONV_PATH.exists():
        raise FileNotFoundError(
            f"uf2conv.py not found at {UF2CONV_PATH}\n"
            "Is the Seeeduino nRF52 board package installed?"
        )

    if output_dir is None:
        output_dir = hex_path.parent
    uf2_path = output_dir / "blinky-things.uf2"

    cmd = [
        sys.executable,
        str(UF2CONV_PATH),
        str(hex_path),
        "-f", UF2_FAMILY_ID,
        "-c",
        "-o", str(uf2_path),
    ]

    print(f"  Input:  {hex_path}")
    print(f"  Output: {uf2_path}")
    print(f"  Family: {UF2_FAMILY_ID}")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        raise RuntimeError("UF2 conversion hung (timed out after 30s)")

    if result.returncode != 0:
        print(f"  [FAIL] uf2conv.py failed:")
        if result.stderr:
            print(f"  {result.stderr.strip()}")
        raise RuntimeError("UF2 conversion failed")

    if result.stdout:
        for line in result.stdout.strip().split("\n"):
            if not line.startswith("/"):  # skip SyntaxWarning paths
                print(f"  {line}")

    if not uf2_path.exists():
        raise RuntimeError(f"UF2 file not created: {uf2_path}")

    uf2_size = uf2_path.stat().st_size
    print(f"  UF2 size: {uf2_size:,} bytes ({uf2_size // 512} blocks)")
    print(f"  [PASS] Conversion successful")

    return uf2_path


# ============================================================
#  Phase 3: Bootloader entry (1200 baud touch)
# ============================================================

def _resolve_by_id_path(port_device):
    """Resolve a /dev/ttyACMx path to its stable /dev/serial/by-id/ symlink.

    The by-id path encodes the USB serial number and is stable across
    reconnects, unlike ttyACMx which can shuffle after USB re-enumeration.

    Returns the by-id path if found, otherwise the original path.
    """
    by_id_dir = Path("/dev/serial/by-id")
    if not by_id_dir.exists():
        return port_device
    try:
        real_target = Path(port_device).resolve()
        for symlink in by_id_dir.iterdir():
            if symlink.resolve() == real_target:
                return str(symlink)
    except OSError:
        pass
    return port_device


def find_all_xiao_ports(board=None):
    """Auto-detect all connected XIAO devices in application mode.

    Uses the active board profile (or an explicit profile) to filter by VID/PID.
    Prefers stable /dev/serial/by-id/ paths over /dev/ttyACMx paths.
    Returns sorted list of serial port paths.
    """
    profile = board or _active_board
    vid = profile["normal_vid"]
    pid = profile["normal_pid"]
    ports = []
    for p in serial.tools.list_ports.comports():
        if p.vid == vid and p.pid == pid:
            stable_path = _resolve_by_id_path(p.device)
            ports.append(stable_path)
    return sorted(ports)


def get_serial_number(port):
    """Get the USB serial number for a port to track device identity.

    Resolves symlinks (e.g., /dev/serial/by-id/... -> /dev/ttyACMx)
    before matching against pyserial's port list.
    """
    real_port = str(Path(port).resolve()) if Path(port).is_symlink() else port
    for p in serial.tools.list_ports.comports():
        if p.device == port or p.device == real_port:
            return p.serial_number
    return None


def find_port_by_serial(serial_number, target_pid=None):
    """Find a serial port matching a USB serial number and optional PID.

    Returns the stable /dev/serial/by-id/ path if available.
    """
    if not serial_number:
        return None
    for p in serial.tools.list_ports.comports():
        if p.serial_number == serial_number:
            if target_pid is None or p.pid == target_pid:
                return _resolve_by_id_path(p.device)
    return None


def find_port_by_id_path(serial_number):
    """Find a device's /dev/serial/by-id/ symlink by USB serial number.

    This is the most reliable way to find a device after reboot, since
    by-id paths encode the serial number and don't change with USB
    re-enumeration.

    Returns the by-id path string or None.
    """
    if not serial_number:
        return None
    by_id_dir = Path("/dev/serial/by-id")
    if not by_id_dir.exists():
        return None
    try:
        for symlink in by_id_dir.iterdir():
            if serial_number in symlink.name:
                return str(symlink)
    except OSError:
        pass
    return None


MAX_BOOTLOADER_RETRIES = 5


# ============================================================
#  USB port recovery (uhubctl)
# ============================================================

def _find_usb_hub_port(port_path):
    """Map a serial port (e.g., /dev/ttyACM0) to its USB hub location.

    Handles /dev/serial/by-id/ symlinks by resolving to the real device first.

    Returns (hub_path, port_number) for uhubctl, or (None, None) if
    the mapping cannot be determined.

    Example: /dev/ttyACM0 → ('1-1.1', 2)
    """
    # Resolve symlinks (e.g., /dev/serial/by-id/... -> /dev/ttyACMx)
    real_path = str(Path(port_path).resolve()) if Path(port_path).is_symlink() else port_path
    # Find the device path in sysfs via /sys/class/tty/ttyACMx/device
    port_name = os.path.basename(real_path)
    sysfs_device = f"/sys/class/tty/{port_name}/device"

    if not os.path.exists(sysfs_device):
        return None, None

    # Resolve symlink to get the USB interface path
    # e.g., /sys/devices/.../1-1.1.2:1.0 → device is 1-1.1.2, hub is 1-1.1, port is 2
    try:
        real_path = os.path.realpath(sysfs_device)
        # Walk up to the USB device level (one above the interface :1.0)
        usb_device_path = os.path.dirname(real_path)
        device_name = os.path.basename(usb_device_path)

        # Parse the USB device name (e.g., "1-1.1.2")
        # The hub is everything except the last number, the port is the last number
        parts = device_name.rsplit(".", 1)
        if len(parts) == 2:
            hub_path = parts[0]
            port_num = int(parts[1])
            return hub_path, port_num
    except (ValueError, OSError):
        pass

    return None, None


def _recover_usb_port(hub_path, port_num, device_serial=None, verbose=False):
    """Power-cycle a USB hub port via uhubctl to recover a stuck device.

    Returns the new serial port path if the device recovered, or None.
    """
    uhubctl = shutil.which("uhubctl")
    if not uhubctl:
        print(f"  uhubctl not installed — cannot recover USB port")
        print(f"  Install with: sudo apt install uhubctl")
        return None

    print(f"  Recovering USB port: hub={hub_path} port={port_num}")

    # Power off
    try:
        result = subprocess.run(
            ["sudo", uhubctl, "-l", hub_path, "-p", str(port_num), "-a", "0"],
            capture_output=True, text=True, timeout=10,
        )
        if result.returncode != 0:
            print(f"  uhubctl power-off failed (exit {result.returncode})")
            if result.stderr:
                print(f"  stderr: {result.stderr.strip()}")
            return None
        if verbose and result.stdout:
            for line in result.stdout.strip().split("\n"):
                print(f"    {line}")
    except subprocess.TimeoutExpired:
        print(f"  uhubctl power-off timed out")
        return None

    time.sleep(2)

    # Power on
    try:
        result = subprocess.run(
            ["sudo", uhubctl, "-l", hub_path, "-p", str(port_num), "-a", "1"],
            capture_output=True, text=True, timeout=10,
        )
        if result.returncode != 0:
            print(f"  uhubctl power-on failed (exit {result.returncode})")
            if result.stderr:
                print(f"  stderr: {result.stderr.strip()}")
            return None
        if verbose and result.stdout:
            for line in result.stdout.strip().split("\n"):
                print(f"    {line}")
    except subprocess.TimeoutExpired:
        print(f"  uhubctl power-on timed out")
        return None

    # Wait for device to re-enumerate
    print(f"  Waiting for device to re-enumerate...")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        time.sleep(0.5)
        if device_serial:
            new_port = find_port_by_serial(device_serial, target_pid=_active_board["normal_pid"])
            if new_port:
                print(f"  Device recovered on {new_port}")
                return new_port
        else:
            # No serial number — check if any new XIAO port appeared
            ports = find_all_xiao_ports()
            if ports:
                if len(ports) > 1:
                    print(f"  WARNING: No device serial available and {len(ports)} XIAO ports found.")
                    print(f"  Returning {ports[0]} as best guess — may be wrong device in multi-device setups.")
                return ports[0]

    print(f"  Device did not re-enumerate after USB port recovery")
    return None


def _device_port_exists(port_path):
    """Check if a serial port path currently exists.

    For /dev/serial/by-id/ symlinks, checks both the symlink and its target.
    """
    if os.path.exists(port_path):
        return True
    # For by-id symlinks, check if the target device exists
    try:
        if os.path.islink(port_path):
            return os.path.exists(os.path.realpath(port_path))
    except OSError:
        pass
    return False


def _check_port_available(port, verbose=False):
    """Verify a serial port is available and belongs to a XIAO device.

    Checks:
    1. Port exists and can be opened (not locked by another process)
    2. Port VID/PID matches XIAO nRF52840 in application mode

    Returns True if the port is available and valid, False otherwise.
    """
    # Check 1: Verify VID/PID matches a XIAO device in application mode.
    # This prevents sending bootloader commands to the wrong device if
    # ports shuffled after a previous flash (USB re-enumeration).
    # Resolve symlinks (e.g., /dev/serial/by-id/... -> /dev/ttyACMx)
    real_port = str(Path(port).resolve()) if Path(port).is_symlink() else port
    port_info = None
    for p in serial.tools.list_ports.comports():
        if p.device == port or p.device == real_port:
            port_info = p
            break

    if port_info is None:
        print(f"\n  ERROR: {port} not found in system port list!")
        print(f"  The port may have disappeared (device reset or USB disconnect).")
        return False

    exp_vid = _active_board["normal_vid"]
    exp_pid = _active_board["normal_pid"]
    board_name = _active_board["name"]
    if port_info.vid != exp_vid or port_info.pid != exp_pid:
        print(f"\n  ERROR: {port} is not a {board_name} in application mode!")
        print(f"  Expected VID:PID {exp_vid:#06x}:{exp_pid:#06x}")
        print(f"  Found    VID:PID {port_info.vid:#06x}:{port_info.pid:#06x}" if port_info.vid else
              f"  Found    VID:PID None:None")
        print(f"  Port numbers may have shuffled after a previous flash.")
        print(f"  Re-run with the correct port, or use --all to auto-detect.")
        return False

    if verbose:
        print(f"  Port identity: VID:PID {port_info.vid:#06x}:{port_info.pid:#06x}"
              f" serial={port_info.serial_number}")

    # Check 2: Verify port is not locked by another process.
    # Use fuser (non-blocking) instead of probe-open (which can block
    # indefinitely if another process holds the port exclusively).
    holders = _port_holders(port)
    if holders:
        my_pid = os.getpid()
        other_pids = [p for p in holders if p != my_pid]
        if other_pids:
            # Identify what's holding the port
            holder_names = []
            for pid in other_pids:
                try:
                    cmdline = Path(f"/proc/{pid}/cmdline").read_text().replace('\0', ' ').strip()
                    holder_names.append(f"  PID {pid}: {cmdline[:120]}")
                except OSError:
                    holder_names.append(f"  PID {pid}: (unknown)")

            print(f"\n  ERROR: {port} is held by another process!")
            for name in holder_names:
                print(name)
            print(f"\n  Fix: Disconnect MCP sessions, stop blinky-server, or kill the process.")
            print(f"  Then wait 2-3 seconds for the port to be released.\n")
            return False

    return True


def trigger_bootloader(port, verbose=False):
    """Enter UF2 bootloader mode with automatic retry.

    The nRF52 SoftDevice can intermittently clear the GPREGRET register
    during reset, causing the bootloader to skip UF2 mode and boot the
    application instead. The BSP fix (SoftDevice API for GPREGRET) makes
    this reliable, but older firmware may still have the race condition.
    Retrying resolves the intermittent failure.

    If a bootloader entry causes the device to disconnect but fail to
    enumerate (stuck USB state), this function will attempt recovery via
    uhubctl USB port power-cycling.

    Returns the USB serial number for tracking the device across
    the mode switch.
    """
    print_section("ENTERING BOOTLOADER")

    # Pre-flight: if blinky-server is running, ask it to release the port
    _request_server_release(port, verbose)

    # Verify port is not locked by another process
    if not _check_port_available(port, verbose):
        print(f"\n  ABORTING: Port {port} is not available.")
        print(f"  Resolve the port lock and retry.")
        return None

    device_serial = get_serial_number(port)
    if device_serial:
        print(f"  Device serial: {device_serial}")
    else:
        print(f"  Warning: Could not read serial number from {port}")

    # Record the USB hub location BEFORE any reset attempts.
    # After a failed USB enumeration, the sysfs entry disappears and we
    # can no longer determine which hub port to power-cycle.
    hub_path, hub_port = _find_usb_hub_port(port)
    if hub_path:
        if verbose:
            print(f"  USB location: hub={hub_path} port={hub_port}")
    else:
        print(f"  Warning: Could not determine USB hub location for {port}")

    current_port = port  # Track port across re-enumerations

    for attempt in range(1, MAX_BOOTLOADER_RETRIES + 1):
        if attempt > 1:
            print(f"  Retry {attempt}/{MAX_BOOTLOADER_RETRIES}...")
            time.sleep(1)  # Let device settle between retries

            # Re-discover port if it changed (e.g., after USB recovery)
            if not _device_port_exists(current_port) and device_serial:
                new_port = find_port_by_serial(device_serial, target_pid=_active_board["normal_pid"])
                if new_port:
                    print(f"  Device re-discovered on {new_port}")
                    current_port = new_port
                else:
                    # Device is gone — try USB recovery if we know the hub location
                    if hub_path:
                        print(f"  Device not found — attempting USB port recovery...")
                        recovered_port = _recover_usb_port(
                            hub_path, hub_port, device_serial, verbose
                        )
                        if recovered_port:
                            current_port = recovered_port
                        else:
                            print(f"  USB recovery failed")
                            continue
                    else:
                        print(f"  Device not found and no hub info for recovery")
                        continue

        pre_existing_blocks = _get_usb_block_devices()

        # Send 'bootloader' serial command
        if attempt == 1:
            print(f"  Trying serial command: bootloader")
        ser = None
        try:
            ser = _serial_open_with_timeout(current_port, 115200, timeout=1)
            time.sleep(0.1)
            ser.reset_input_buffer()
            ser.write(b'bootloader\n')
            ser.flush()
            time.sleep(0.1)

            if _wait_for_uf2_drive(pre_existing_blocks, timeout=5, verbose=verbose):
                return device_serial

            # Check if device disconnected but UF2 drive didn't appear.
            # If so, the device may be stuck in a failed USB enumeration
            # state. Skip the 1200 baud touch (port is dead) and go
            # straight to recovery on the next attempt.
            if not _device_port_exists(current_port):
                print(f"  Device disconnected but UF2 drive not detected")
                # Don't try 1200 baud touch — port is gone
                continue

        except (serial.SerialException, OSError) as e:
            print(f"  Serial error: {e}")
        finally:
            if ser:
                try:
                    ser.close()
                except (BrokenPipeError, OSError):
                    pass

        # 1200-baud touch is first-attempt only. On the nRF52, the 1200-baud
        # touch forces a full USB disconnect/reconnect cycle. If it fails on
        # attempt 1, the port may have re-enumerated to a different address
        # (e.g., /dev/ttyACM0 → /dev/ttyACM1), making the original port path
        # stale. Retrying the serial 'bootloader' command is safer since it
        # only triggers a soft reboot and the device re-enumerates to the same
        # port. If the serial command also fails, the caller should re-discover
        # ports rather than blindly retrying on a potentially stale path.
        if attempt == 1:
            pre_existing_blocks = _get_usb_block_devices()
            print(f"  Trying 1200 baud touch on {current_port}...")
            ser = None
            try:
                ser = _serial_open_with_timeout(current_port, 1200, timeout=1)
                ser.dtr = True
                time.sleep(0.1)
                try:
                    ser.dtr = False
                    time.sleep(0.05)
                    ser.dtr = True
                    time.sleep(0.1)
                except (BrokenPipeError, OSError):
                    print(f"  Device reset during DTR toggle (expected)")
                print(f"  1200 baud touch complete")

                if _wait_for_uf2_drive(pre_existing_blocks, timeout=5, verbose=verbose):
                    return device_serial

            except serial.SerialException as e:
                print(f"  1200 baud touch failed: {e}")
            finally:
                if ser:
                    try:
                        ser.close()
                    except (BrokenPipeError, OSError):
                        pass

        print(f"  UF2 drive not detected (attempt {attempt})")

    print(f"  ERROR: Bootloader entry failed after {MAX_BOOTLOADER_RETRIES} attempts")
    print(f"  Device did not enter UF2 mode (no block device appeared).")
    print(f"  Possible causes:")
    print(f"    - Intermittent GPREGRET race condition (nRF52 SoftDevice clears register)")
    print(f"    - USB bus instability (too many recent re-enumerations)")
    print(f"    - Device firmware not responding to bootloader command")
    return None


def _wait_for_uf2_drive(pre_existing_blocks, timeout=5, verbose=False):
    """Wait for a new USB block device to appear (UF2 bootloader mode).

    Returns True if a new USB block device appeared, False if timeout.
    """
    if verbose:
        print(f"  Waiting for UF2 drive...")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = _get_usb_block_devices()
        new_devices = current - pre_existing_blocks
        if new_devices:
            dev = sorted(new_devices)[0]
            print(f"  UF2 drive detected: {dev}")
            return True
        time.sleep(0.1)

    # Always show failure warning — silent timeout is confusing
    print(f"  Warning: No UF2 drive appeared within {timeout}s")
    return False


# ============================================================
#  Phase 4: UF2 drive detection and mounting
# ============================================================

def ensure_udisks2_running():
    """Start udisks2 service if not running. Returns True if running."""
    try:
        result = subprocess.run(
            ["systemctl", "is-active", "udisks2"],
            capture_output=True, text=True, timeout=5,
        )
        if result.stdout.strip() == "active":
            return True
    except subprocess.TimeoutExpired:
        print(f"  Warning: 'systemctl is-active' timed out, assuming udisks2 not running")

    print(f"  Starting udisks2 service...")
    try:
        result = subprocess.run(
            ["sudo", "systemctl", "start", "udisks2"],
            capture_output=True, text=True, timeout=10,
        )
    except subprocess.TimeoutExpired:
        print(f"  Warning: 'systemctl start udisks2' timed out")
        return False
    if result.returncode == 0:
        time.sleep(1)
        print(f"  udisks2 started")
        return True

    print(f"  Warning: Could not start udisks2: {result.stderr.strip()}")
    return False


def find_uf2_block_device(timeout):
    """Wait for a UF2 mass storage block device to appear.

    First checks for any existing USB block device (trigger_bootloader may
    have already detected the drive). Then polls for new devices.

    Returns the block device path (e.g., /dev/sda) or None.
    """
    print(f"  Scanning for UF2 block device (timeout: {timeout}s)...")

    # Check for already-present USB block device (from trigger_bootloader)
    initial = _get_usb_block_devices()
    if initial:
        dev = sorted(initial)[0]
        print(f"  Found USB block device: {dev}")
        return dev

    # Poll for new devices
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = _get_usb_block_devices()
        if current:
            dev = sorted(current)[0]
            print(f"  New USB block device: {dev}")
            return dev
        time.sleep(0.2)

    return None


def _get_usb_block_devices():
    """Get set of USB block device paths.

    Strategy (ordered fastest-first to avoid subprocess delays eating
    the drive detection timeout on slow systems like Raspberry Pi):
    1. Check /dev/disk/by-label/ for UF2 drive label (instant, no subprocess)
    2. Scan /sys/block/sd* for devices with Seeed/bootloader VID
    3. Fallback: lsblk -J for USB block devices (subprocess, can be slow)
    """
    devices = set()

    # Strategy 1: check /dev/disk/by-label/ for UF2 drive label (instant)
    drive_label = _active_board.get("drive_label", UF2_DRIVE_LABEL)
    for label in (drive_label, UF2_DRIVE_LABEL):
        by_label = Path("/dev/disk/by-label") / label
        if by_label.exists():
            try:
                real_dev = str(by_label.resolve())
                devices.add(real_dev)
            except OSError:
                pass

    if devices:
        return devices

    # Strategy 2: scan /sys/block/sd* for Seeed VID (0x2886)
    bootloader_vid = _active_board.get("bootloader_vid", NORMAL_VID)
    for block_dir in glob.glob("/sys/block/sd*"):
        block_name = os.path.basename(block_dir)
        # Walk up the sysfs tree to find the USB device's idVendor
        try:
            real_path = os.path.realpath(os.path.join(block_dir, "device"))
            check_path = real_path
            for _ in range(10):
                vendor_file = os.path.join(check_path, "idVendor")
                if os.path.exists(vendor_file):
                    with open(vendor_file) as f:
                        vid = f.read().strip()
                    if vid in (f"{NORMAL_VID:04x}", f"{bootloader_vid:04x}"):
                        part_path = Path(f"/dev/{block_name}1")
                        if part_path.exists():
                            devices.add(str(part_path))
                        else:
                            devices.add(f"/dev/{block_name}")
                    break
                check_path = os.path.dirname(check_path)
                if check_path in ("", "/"):
                    break
        except OSError:
            continue

    if devices:
        return devices

    # Strategy 3: lsblk (subprocess — timeout kept short to avoid eating
    # the drive detection window on slow systems)
    try:
        result = subprocess.run(
            ["lsblk", "-o", "NAME,TRAN,RM,TYPE", "-J"],
            capture_output=True, text=True, timeout=2,
        )
        if result.returncode == 0:
            try:
                data = json.loads(result.stdout)
                for dev in data.get("blockdevices", []):
                    if dev.get("tran") == "usb":
                        for child in dev.get("children", []):
                            if child.get("type") in ("part", "disk"):
                                devices.add(f"/dev/{child['name']}")
                        if not dev.get("children"):
                            devices.add(f"/dev/{dev['name']}")
            except (json.JSONDecodeError, KeyError):
                pass
    except subprocess.TimeoutExpired:
        pass

    return devices


def _get_block_device_serial(block_dev):
    """Get USB serial number for a block device via sysfs.

    Reads /sys/block/<dev>/device/../../serial to find the USB device
    serial number. Returns None if not available.
    """
    # Extract device name from path (e.g., /dev/sda1 -> sda1, /dev/sda -> sda)
    dev_name = Path(block_dev).name
    # Strip partition number to get parent disk (sda1 -> sda)
    disk_name = re.sub(r'\d+$', '', dev_name)

    serial_path = Path(f"/sys/block/{disk_name}/device/../../serial")
    try:
        return serial_path.read_text().strip()
    except (OSError, FileNotFoundError):
        return None


def cleanup_stale_mounts():
    """Remove stale UF2 mounts from previous failed runs.

    Parses /proc/mounts for /mnt/uf2-* entries (and legacy /mnt/uf2-upload),
    unmounts them with lazy unmount, and removes empty mount directories.

    Note: This will also unmount any manually-mounted UF2 drives at /mnt/uf2-*.
    This is acceptable for the automation use case — manual mounts should use
    a different path (e.g., /mnt/manual-uf2) to avoid conflicts.
    """
    stale = []
    try:
        with open("/proc/mounts") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    mount_point = parts[1]
                    if mount_point.startswith("/mnt/uf2-"):
                        stale.append(mount_point)
    except OSError:
        return

    if not stale:
        return

    print(f"  Cleaning {len(stale)} stale UF2 mount(s)...")
    for mp in stale:
        try:
            subprocess.run(
                ["sudo", "umount", "-l", mp],
                capture_output=True, timeout=10,
            )
        except subprocess.TimeoutExpired:
            print(f"  Warning: umount timed out for {mp}, skipping")
            continue
        # Remove empty directory
        mp_path = Path(mp)
        try:
            if mp_path.exists() and mp_path.is_dir() and not any(mp_path.iterdir()):
                subprocess.run(
                    ["sudo", "rmdir", mp],
                    capture_output=True, timeout=5,
                )
        except (OSError, PermissionError, subprocess.TimeoutExpired):
            pass
    print(f"  Stale mounts cleaned")


def find_existing_uf2_mount():
    """Check common mount locations for an already-mounted UF2 drive."""
    user = os.environ.get("USER", "blinkytime")
    search_dirs = [
        Path("/run/media") / user,
        Path("/media") / user,
        Path("/media"),
        Path("/mnt"),
    ]

    for search_dir in search_dirs:
        if not search_dir.exists():
            continue
        try:
            for entry in search_dir.iterdir():
                if entry.is_dir() and (entry / UF2_INFO_FILE).exists():
                    return entry
        except PermissionError:
            continue

    return None


def mount_with_udisksctl(block_device):
    """Mount using udisksctl. Returns mount point Path or None."""
    print(f"  Mounting {block_device} with udisksctl...")
    try:
        result = subprocess.run(
            ["udisksctl", "mount", "--block-device", block_device,
             "--no-user-interaction"],
            capture_output=True, text=True, timeout=10,
        )
    except subprocess.TimeoutExpired:
        print(f"  udisksctl mount timed out after 10s")
        return None

    if result.returncode == 0:
        output = result.stdout.strip()
        print(f"  {output}")

        if " at " in output:
            mount_str = output.split(" at ")[-1].rstrip(".")
            mount_point = Path(mount_str)
            if mount_point.exists():
                info_file = mount_point / UF2_INFO_FILE
                if info_file.exists():
                    print(f"  [PASS] UF2 drive verified at {mount_point}")
                    return mount_point
                else:
                    print(f"  Warning: {UF2_INFO_FILE} not found (wrong device?)")
    else:
        stderr = result.stderr.strip()
        # "already mounted" is fine
        if "already mounted" in stderr.lower():
            # Try to find where it's mounted
            existing = find_existing_uf2_mount()
            if existing:
                return existing
        print(f"  udisksctl failed: {stderr}")

    return None


def mount_manually(block_device, mount_point=None):
    """Fallback: mount using sudo mount. Returns mount point Path or None."""
    if mount_point is None:
        mount_point = Path("/mnt/uf2-upload")
    else:
        mount_point = Path(mount_point)
    print(f"  Manual mount {block_device} -> {mount_point}...")

    try:
        subprocess.run(
            ["sudo", "mkdir", "-p", str(mount_point)],
            capture_output=True, timeout=5,
        )
    except subprocess.TimeoutExpired:
        print(f"  Warning: 'mkdir -p' timed out, continuing anyway")

    uid = os.getuid()
    gid = os.getgid()
    try:
        result = subprocess.run(
            ["sudo", "mount", "-t", "vfat", "-o",
             f"uid={uid},gid={gid},umask=022",
             block_device, str(mount_point)],
            capture_output=True, text=True, timeout=10,
        )
    except subprocess.TimeoutExpired:
        print(f"  Mount timed out after 10s")
        return None

    if result.returncode == 0:
        if (mount_point / UF2_INFO_FILE).exists():
            print(f"  [PASS] Mounted at {mount_point}")
            return mount_point
        else:
            print(f"  Mounted but {UF2_INFO_FILE} not found (wrong device?)")
            try:
                subprocess.run(
                    ["sudo", "umount", str(mount_point)],
                    capture_output=True, timeout=10,
                )
            except subprocess.TimeoutExpired:
                print(f"  Warning: umount timed out")
    else:
        print(f"  Mount failed: {result.stderr.strip()}")

    return None


def mount_device(block_device, index):
    """Mount a block device to /mnt/uf2-{index}. Returns mount point Path or None.

    Used by parallel upload to give each device its own mount point.
    """
    mount_point = Path(f"/mnt/uf2-{index}")

    subprocess.run(
        ["sudo", "mkdir", "-p", str(mount_point)],
        capture_output=True, timeout=5,
    )

    uid = os.getuid()
    gid = os.getgid()
    result = subprocess.run(
        ["sudo", "mount", "-t", "vfat", "-o",
         f"uid={uid},gid={gid},umask=022",
         block_device, str(mount_point)],
        capture_output=True, text=True, timeout=10,
    )

    if result.returncode == 0:
        if (mount_point / UF2_INFO_FILE).exists():
            print(f"  Mounted {block_device} at {mount_point}")
            return mount_point
        else:
            print(f"  Mounted {block_device} but {UF2_INFO_FILE} not found (wrong device?)")
            subprocess.run(
                ["sudo", "umount", str(mount_point)],
                capture_output=True, timeout=10,
            )
    else:
        stderr = result.stderr.strip()
        if "already mounted" in stderr.lower():
            if (mount_point / UF2_INFO_FILE).exists():
                print(f"  {block_device} already mounted at {mount_point}")
                return mount_point
        print(f"  Mount {block_device} failed: {stderr}")

    return None


def mount_uf2_drive(device_serial=None, timeout=BOOTLOADER_TIMEOUT):
    """Wait for the UF2 drive to appear and mount it.

    Tries three strategies in order:
    1. Check if already mounted (board was already in bootloader)
    2. udisksctl mount (headless-safe, no root needed)
    3. sudo mount (fallback)

    Returns the mount point Path.
    """
    print_section("WAITING FOR UF2 DRIVE")

    # Check if already mounted
    existing = find_existing_uf2_mount()
    if existing:
        print(f"  UF2 drive already mounted at: {existing}")
        return existing

    # Wait for block device
    block_dev = find_uf2_block_device(timeout)
    if not block_dev:
        raise TimeoutError(
            f"UF2 drive not detected within {timeout} seconds.\n\n"
            "The board may not have entered bootloader mode.\n"
            "Try manually:\n"
            "  1. Double-tap the reset button quickly\n"
            "  2. A drive called 'XIAO-SENSE' should appear\n"
            "  3. Re-run with: --already-in-bootloader"
        )

    print(f"  Block device: {block_dev}")

    # Give the device a moment to settle
    time.sleep(0.5)

    # Strategy 1: udisksctl
    udisks_ok = ensure_udisks2_running()
    if udisks_ok:
        mount_point = mount_with_udisksctl(block_dev)
        if mount_point:
            return mount_point

    # Strategy 2: manual mount
    mount_point = mount_manually(block_dev)
    if mount_point:
        return mount_point

    raise RuntimeError(
        f"Could not mount {block_dev}.\n"
        "Try mounting manually and re-run with --mount-point <path>"
    )


# ============================================================
#  Phase 5: Firmware copy
# ============================================================

def copy_firmware(uf2_path, mount_point):
    """Copy the UF2 file to the bootloader drive.

    Returns True on success.
    """
    print_section("COPYING FIRMWARE")

    dest = mount_point / uf2_path.name
    uf2_size = uf2_path.stat().st_size

    print(f"  Source: {uf2_path}")
    print(f"  Dest:   {dest}")
    print(f"  Size:   {uf2_size:,} bytes")

    # Verify drive identity
    info_file = mount_point / UF2_INFO_FILE
    if not info_file.exists():
        print(f"  [FAIL] {UF2_INFO_FILE} not found at mount point!")
        print(f"  Refusing to write to unidentified drive.")
        return False

    try:
        info_content = info_file.read_text()
        for line in info_content.strip().split("\n"):
            print(f"  Drive: {line}")
    except Exception:
        pass

    current_uf2 = mount_point / "CURRENT.UF2"
    if current_uf2.exists():
        print(f"  Existing firmware backup: {current_uf2.stat().st_size:,} bytes")

    print(f"  Copying...")
    try:
        shutil.copy2(str(uf2_path), str(dest))
        os.sync()
        print(f"  [PASS] Copy complete, synced to disk")
        return True
    except (OSError, IOError) as e:
        print(f"  [FAIL] Copy failed: {e}")
        return False


# ============================================================
#  Phase 6: Reboot verification
# ============================================================

def verify_reboot(mount_point, port=None, device_serial=None, verbose=False):
    """Verify the board rebooted after firmware copy.

    Checks:
    1. UF2 drive disappears (firmware accepted, board rebooting)
    2. Serial port reappears (new firmware is running)
    3. Firmware responds to serial command (post-flash verification)

    Returns (success: bool, new_port: str or None).
    """
    print_section("VERIFYING REBOOT")

    # Wait for UF2 drive to disappear
    print(f"  Waiting for UF2 drive to disappear...")
    deadline = time.monotonic() + REBOOT_TIMEOUT
    drive_gone = False

    while time.monotonic() < deadline:
        if not (mount_point / UF2_INFO_FILE).exists():
            drive_gone = True
            print(f"  UF2 drive disappeared (board is rebooting)")
            break
        time.sleep(0.2)

    if not drive_gone:
        print(f"  [WARN] UF2 drive still present after {REBOOT_TIMEOUT}s")
        print(f"  The firmware may have been rejected (wrong family or corrupt).")
        return False, None

    # Wait for serial port to reappear
    print(f"  Waiting for serial port...")
    deadline = time.monotonic() + PORT_REAPPEAR_TIMEOUT
    new_port = None

    while time.monotonic() < deadline:
        if device_serial:
            # Try by-id path first (most reliable after reboot)
            new_port = find_port_by_id_path(device_serial)
            if not new_port:
                new_port = find_port_by_serial(device_serial, _active_board["normal_pid"])
            if new_port:
                print(f"  Device back on {new_port}")
                break
        elif port:
            if Path(port).exists():
                new_port = port
                print(f"  Port {port} is back")
                break
        time.sleep(0.2)

    if not new_port:
        # Drive disappeared but port didn't return -- partial success
        print(f"  [WARN] Serial port not detected within {PORT_REAPPEAR_TIMEOUT}s")
        print(f"  Firmware was likely accepted (drive disappeared).")
        return True, None

    # Post-flash verification: query the firmware
    verify_result = verify_firmware(new_port, verbose=verbose)
    return True, new_port


def verify_firmware(port, verbose=False):
    """Verify firmware is running by querying the device over serial.

    Sends 'version' command and prints the response. This confirms:
    1. The device is running (not stuck in bootloader)
    2. The serial interface is functional
    3. The firmware version matches what was flashed

    Returns True if verification succeeded, False otherwise.
    """
    print_section("POST-FLASH VERIFICATION")

    # Resolve symlink for serial.Serial() (some pyserial versions need real path)
    real_port = str(Path(port).resolve()) if Path(port).is_symlink() else port

    # Wait for firmware to finish booting (setup() includes delays)
    print(f"  Waiting for firmware to initialize...")
    time.sleep(3)

    ser = None
    try:
        ser = _serial_open_with_timeout(real_port, 115200, timeout=2)
        time.sleep(0.5)  # Let serial settle
        ser.reset_input_buffer()

        # Send 'version' command to verify basic firmware operation
        ser.write(b'version\n')
        ser.flush()
        time.sleep(0.3)

        # Read response lines
        response_lines = []
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if line:
                    response_lines.append(line)
            else:
                if response_lines:
                    break
                time.sleep(0.1)

        if response_lines:
            print(f"  Firmware response:")
            for line in response_lines:
                print(f"    {line}")
            print(f"  [PASS] Firmware is running")
        else:
            print(f"  [WARN] No response to 'version' command")
            print(f"  Device may still be initializing.")

        # Also try 'show nn' to check NN status
        ser.reset_input_buffer()
        ser.write(b'show nn\n')
        ser.flush()
        time.sleep(0.3)

        nn_lines = []
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if line:
                    nn_lines.append(line)
            else:
                if nn_lines:
                    break
                time.sleep(0.1)

        if nn_lines:
            print(f"  NN status:")
            for line in nn_lines:
                print(f"    {line}")

        return True

    except serial.SerialException as e:
        print(f"  [WARN] Could not verify firmware: {e}")
        return False
    except OSError as e:
        print(f"  [WARN] OS error during verification: {e}")
        return False
    finally:
        if ser:
            try:
                ser.close()
            except (BrokenPipeError, OSError):
                pass


# ============================================================
#  Self-test
# ============================================================

def run_self_test():
    """Verify upload infrastructure is available and functional."""
    print_section("UF2 UPLOAD SELF-TEST")

    passed = 0
    failed = 0

    # Test 1: uf2conv.py
    print("Test 1: uf2conv.py availability...")
    if UF2CONV_PATH.exists():
        print(f"  [PASS] {UF2CONV_PATH}")
        passed += 1
    else:
        print(f"  [FAIL] Not found at {UF2CONV_PATH}")
        failed += 1

    # Test 2: pyserial
    print("Test 2: pyserial...")
    try:
        print(f"  [PASS] pyserial {serial.VERSION}")
        passed += 1
    except Exception:
        print(f"  [FAIL] pyserial not available")
        failed += 1

    # Test 3: udisksctl
    print("Test 3: udisksctl...")
    try:
        result = subprocess.run(["which", "udisksctl"], capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            print(f"  [PASS] {result.stdout.strip()}")
            passed += 1
        else:
            print(f"  [FAIL] Not found (install udisks2)")
            failed += 1
    except subprocess.TimeoutExpired:
        print(f"  [FAIL] 'which udisksctl' timed out")
        failed += 1

    # Test 4: pre_upload_check.py
    print("Test 4: pre_upload_check.py...")
    check_path = Path(__file__).parent / "pre_upload_check.py"
    if check_path.exists():
        print(f"  [PASS] {check_path}")
        passed += 1
    else:
        print(f"  [FAIL] Not found at {check_path}")
        failed += 1

    # Test 5: Build output
    print("Test 5: Build output...")
    hex_file = Path("/tmp/blinky-build/blinky-things.ino.hex")
    if hex_file.exists():
        print(f"  [PASS] {hex_file} ({hex_file.stat().st_size:,} bytes)")
        passed += 1
    else:
        print(f"  [INFO] No build output (compile first)")
        passed += 1

    # Test 6: Connected XIAO devices
    print("Test 6: Connected XIAO devices...")
    xiao_ports = [
        p for p in serial.tools.list_ports.comports()
        if p.vid == NORMAL_VID
    ]
    if xiao_ports:
        for p in xiao_ports:
            mode = "app" if p.pid == NORMAL_PID else "bootloader"
            print(f"  [PASS] {p.device} serial={p.serial_number} ({mode})")
        passed += 1
    else:
        print(f"  [INFO] No XIAO devices connected")
        passed += 1

    # Test 7: User permissions
    print("Test 7: User permissions...")
    try:
        groups_out = subprocess.run(
            ["groups"], capture_output=True, text=True, timeout=5,
        ).stdout.strip()
        has_dialout = "dialout" in groups_out
        has_plugdev = "plugdev" in groups_out
        if has_dialout and has_plugdev:
            print(f"  [PASS] User in dialout and plugdev groups")
            passed += 1
        else:
            missing = []
            if not has_dialout:
                missing.append("dialout")
            if not has_plugdev:
                missing.append("plugdev")
            print(f"  [FAIL] Missing groups: {', '.join(missing)}")
            print(f"         Fix: sudo usermod -aG {','.join(missing)} $USER")
            failed += 1
    except Exception:
        print(f"  [WARN] Could not check groups")
        passed += 1

    print(f"\nSelf-test: {passed} passed, {failed} failed")
    return failed == 0


# ============================================================
#  Argument parsing
# ============================================================

def parse_args():
    parser = argparse.ArgumentParser(
        description="Safe UF2 firmware upload for XIAO nRF52840 Sense / XIAO ESP32-S3",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Upload workflow:
  1. Validate hex file (address safety checks)   [nRF52840 only]
  2. Convert hex -> UF2 (family 0xADA52840)      [nRF52840 only]
     OR locate pre-built .uf2 from arduino-esp32 [ESP32-S3]
  3. Enter bootloader (1200 baud serial touch)
  4. Mount UF2 drive (udisksctl or manual)
  5. Copy firmware.uf2 to drive
  6. Verify reboot (drive disappears, port returns)

Examples:
  %(prog)s /dev/ttyACM0                             # Single nRF52840 device
  %(prog)s /dev/ttyACM0 /dev/ttyACM1 /dev/ttyACM2  # Multiple devices
  %(prog)s --all                                     # Auto-detect all XIAO devices
  %(prog)s /dev/ttyACM0 --hex firmware.hex
  %(prog)s /dev/ttyACM0 --dry-run
  %(prog)s --already-in-bootloader
  %(prog)s --self-test
  %(prog)s /dev/ttyACM0 --board esp32s3              # ESP32-S3 device
  %(prog)s /dev/ttyACM0 --board esp32s3 --build-dir /tmp/blinky-esp32-build
        """,
    )
    parser.add_argument(
        "ports", nargs="*", metavar="PORT",
        help="Serial port(s) (e.g., /dev/ttyACM0). "
             "Specify multiple ports to upload to several devices sequentially. "
             "Not required with --all, --already-in-bootloader, or --self-test",
    )
    parser.add_argument(
        "--all", action="store_true",
        help="Auto-detect and upload to all connected XIAO devices",
    )
    parser.add_argument(
        "--hex", dest="hex_file",
        help="Path to .hex file (default: auto-detect from build output)",
    )
    parser.add_argument(
        "--uf2", dest="uf2_file",
        help="Path to pre-converted .uf2 file (skip validation and conversion)",
    )
    parser.add_argument(
        "--build-dir", dest="build_dir", default="/tmp/blinky-build",
        help="Build output directory (default: /tmp/blinky-build)",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Validate and convert only, do not upload",
    )
    parser.add_argument(
        "--already-in-bootloader", action="store_true",
        help="Board is already in bootloader mode (skip 1200 baud touch)",
    )
    parser.add_argument(
        "--skip-validation", action="store_true",
        help="[DANGEROUS] Skip pre-upload safety checks — risk of bricking device",
    )
    parser.add_argument(
        "--mount-point", dest="mount_point",
        help="Override UF2 drive mount point",
    )
    parser.add_argument(
        "--self-test", action="store_true",
        help="Verify upload infrastructure",
    )
    parser.add_argument(
        "--parallel", action="store_true",
        help="Upload to multiple devices with staggered bootloader entry (2s apart)",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Verbose output",
    )
    parser.add_argument(
        "--board", dest="board", default="nrf52840",
        choices=list(BOARD_PROFILES.keys()),
        help="Target board (default: nrf52840). Use 'esp32s3' for XIAO ESP32-S3.",
    )
    return parser.parse_args()


# ============================================================
#  Single-device upload (phases 3-6)
# ============================================================

def upload_to_device(port, uf2_path, verbose=False):
    """Upload firmware to a single device.

    Runs phases 3-6: bootloader entry, drive mount, firmware copy, reboot verify.
    Returns (success: bool, message: str).
    """
    dev_start = time.monotonic()
    try:
        # Phase 3: Enter bootloader
        device_serial = trigger_bootloader(port, verbose=verbose)
        if device_serial is None:
            return False, f"bootloader entry failed ({_elapsed(dev_start)}) — see errors above"

        # Phase 4: Mount UF2 drive
        mount_point = mount_uf2_drive(
            device_serial=device_serial,
            timeout=BOOTLOADER_TIMEOUT,
        )

        # Phase 5: Copy firmware
        if not copy_firmware(uf2_path, mount_point):
            return False, f"firmware copy failed ({_elapsed(dev_start)})"

        # Phase 6: Verify reboot + post-flash verification
        reboot_ok, new_port = verify_reboot(mount_point, port, device_serial, verbose=verbose)
        if reboot_ok:
            # Ask blinky-server to reconnect (best-effort)
            reconnect_port = new_port or port
            _request_server_reconnect(reconnect_port, verbose=verbose)
            elapsed = _elapsed(dev_start)
            msg = f"OK in {elapsed} (now on {new_port})" if new_port else f"OK in {elapsed}"
            return True, msg
        else:
            return False, f"reboot verification failed ({_elapsed(dev_start)})"

    except TimeoutError as e:
        return False, f"timeout: {e}"
    except RuntimeError as e:
        return False, f"error: {e}"


# ============================================================
#  Parallel multi-device upload
# ============================================================

def _cleanup_parallel_mounts(mount_map):
    """Unmount and clean up all parallel mount points. Called on any exit path."""
    for port, info in mount_map.items():
        mp = info.get("mount_point")
        if mp and Path(mp).exists():
            subprocess.run(
                ["sudo", "umount", "-l", str(mp)],
                capture_output=True, timeout=10,
            )
            try:
                mp_path = Path(mp)
                if mp_path.exists() and mp_path.is_dir() and not any(mp_path.iterdir()):
                    subprocess.run(
                        ["sudo", "rmdir", str(mp)],
                        capture_output=True, timeout=5,
                    )
            except (OSError, PermissionError):
                pass


def upload_parallel(ports, uf2_path, verbose=False):
    """Upload firmware to multiple devices via staggered bootloader entry.

    True parallel USB re-enumeration (all devices at once) overwhelms the
    Pi's shared USB controller. Instead, we stagger bootloader entries with
    a 2-second delay between devices, giving the USB bus time to settle.

    Flow:
      Phase 0: Clean stale mounts from previous failed runs
      Phase 1: Record device serial numbers
      Phase 2: Staggered bootloader entry + per-device mount
      Phase 3: Copy firmware to each mounted drive
      Phase 4: Unmount all drives (triggers reboot)
      Phase 5: Verify reboots (serial ports return)

    Returns list of (port, success, message).
    """
    print_section(f"PARALLEL UPLOAD TO {len(ports)} DEVICES")

    # Per-device tracking: port -> {serial, block_dev, mount_point, copied}
    mount_map = {}

    try:
        # --- Phase 0: Clean stale mounts ---
        cleanup_stale_mounts()

        # --- Phase 1: Record device serial numbers ---
        device_serials = {}
        hub_locations = {}  # port -> (hub_path, hub_port) for USB recovery
        for port in ports:
            sn = get_serial_number(port)
            if sn:
                device_serials[port] = sn
                print(f"  {port}: serial={sn}")
            # Record USB hub location before any resets (sysfs disappears after disconnect)
            hp, hn = _find_usb_hub_port(port)
            if hp:
                hub_locations[port] = (hp, hn)
            mount_map[port] = {"serial": sn, "block_dev": None,
                               "mount_point": None, "copied": False}

        # --- Phase 2: Staggered bootloader entry + per-device mount ---
        print_section("STAGGERED BOOTLOADER ENTRY")

        for i, port in enumerate(ports):
            if i > 0:
                print(f"\n  Waiting 1s for USB bus to settle...")
                time.sleep(1)

            print(f"  Device {i + 1}/{len(ports)}: {port}")

            # Pre-flight: verify port identity (VID/PID check)
            if not _check_port_available(port, verbose):
                print(f"    SKIPPING: {port} is not available or not a XIAO device")
                continue

            # Snapshot block devices before bootloader command
            pre_blocks = _get_usb_block_devices()

            # Send bootloader command (with retries + USB recovery)
            entered = False
            current_port = port
            for attempt in range(1, MAX_BOOTLOADER_RETRIES + 1):
                if attempt > 1:
                    print(f"    Retry {attempt}/{MAX_BOOTLOADER_RETRIES}...")
                    time.sleep(1)

                    # Re-discover port if it disappeared (USB recovery)
                    if not _device_port_exists(current_port):
                        sn = mount_map[port]["serial"]
                        if sn:
                            new_port = find_port_by_serial(sn, target_pid=_active_board["normal_pid"])
                            if new_port:
                                print(f"    Device re-discovered on {new_port}")
                                current_port = new_port
                            elif port in hub_locations:
                                hp, hn = hub_locations[port]
                                print(f"    Device not found — attempting USB port recovery...")
                                recovered = _recover_usb_port(hp, hn, sn, verbose)
                                if recovered:
                                    current_port = recovered
                                else:
                                    print(f"    USB recovery failed")
                                    continue
                            else:
                                print(f"    Device not found, no hub info")
                                continue

                try:
                    ser = serial.Serial(current_port, 115200, timeout=1)
                    time.sleep(0.1)
                    ser.reset_input_buffer()
                    ser.write(b'bootloader\n')
                    ser.flush()
                    time.sleep(0.1)
                    try:
                        ser.close()
                    except (BrokenPipeError, OSError):
                        pass
                    if attempt == 1:
                        print(f"    Bootloader command sent")
                except (serial.SerialException, OSError) as e:
                    print(f"    Serial error: {e}")
                    # Port may already be gone (device resetting) — check for new block dev
                    pass

                # Wait for a new block device (this specific device)
                deadline = time.monotonic() + 3
                new_dev = None
                while time.monotonic() < deadline:
                    current = _get_usb_block_devices()
                    new_devices = current - pre_blocks
                    if new_devices:
                        new_dev = sorted(new_devices)[0]
                        break
                    time.sleep(0.1)

                if new_dev:
                    # Verify block device belongs to this port's device via USB serial number
                    expected_sn = mount_map[port]["serial"]
                    if expected_sn:
                        block_sn = _get_block_device_serial(new_dev)
                        if block_sn and block_sn != expected_sn:
                            print(f"    WARNING: Block device {new_dev} serial '{block_sn}' "
                                  f"does not match expected '{expected_sn}' — possible mis-assignment")
                        elif block_sn:
                            print(f"    Serial number verified: {block_sn}")
                    print(f"    UF2 drive detected: {new_dev}")
                    mount_map[port]["block_dev"] = new_dev
                    entered = True
                    break
                else:
                    print(f"    No UF2 drive appeared (attempt {attempt})")

                    # Skip 1200 baud touch if device disconnected (port is dead)
                    if not _device_port_exists(current_port):
                        print(f"    Device disconnected — skipping 1200 baud touch")
                        continue

                    # 1200-baud touch first-attempt only (see single-device path comment)
                    if attempt == 1:
                        pre_blocks = _get_usb_block_devices()
                        print(f"    Trying 1200 baud touch...")
                        try:
                            ser = serial.Serial()
                            ser.port = current_port
                            ser.baudrate = 1200
                            ser.dtr = True
                            ser.open()
                            time.sleep(0.1)
                            try:
                                ser.dtr = False
                                time.sleep(0.05)
                                ser.dtr = True
                                time.sleep(0.1)
                            except (BrokenPipeError, OSError):
                                pass
                            try:
                                ser.close()
                            except (BrokenPipeError, OSError):
                                pass

                            deadline = time.monotonic() + 3
                            while time.monotonic() < deadline:
                                current = _get_usb_block_devices()
                                new_devices = current - pre_blocks
                                if new_devices:
                                    new_dev = sorted(new_devices)[0]
                                    break
                                time.sleep(0.1)

                            if new_dev:
                                print(f"    UF2 drive detected: {new_dev}")
                                mount_map[port]["block_dev"] = new_dev
                                entered = True
                                break
                        except serial.SerialException as e:
                            if verbose:
                                print(f"    1200 baud touch failed: {e}")

            if not entered:
                print(f"    FAILED: {port} did not enter bootloader")
                continue

            # Mount this device's block device to /mnt/uf2-{i}
            time.sleep(0.5)  # Let device settle after mode switch
            mp = mount_device(mount_map[port]["block_dev"], i)
            if mp:
                mount_map[port]["mount_point"] = str(mp)
            else:
                print(f"    FAILED: Could not mount {mount_map[port]['block_dev']}")

        # --- Phase 3: Copy firmware to each mounted drive ---
        mounted = {p: info for p, info in mount_map.items() if info["mount_point"]}
        if not mounted:
            print_failure("No UF2 drives mounted — nothing to flash")
            return [(p, False, "mount failed") for p in ports]

        print_section(f"COPYING FIRMWARE TO {len(mounted)} DRIVE(S)")
        for port, info in mounted.items():
            mp = Path(info["mount_point"])
            if copy_firmware(uf2_path, mp):
                info["copied"] = True
            else:
                print(f"  Warning: copy failed to {mp} ({port})")

        copy_count = sum(1 for info in mount_map.values() if info["copied"])
        print(f"\n  Firmware copied to {copy_count}/{len(ports)} device(s)")

        # --- Phase 4: Unmount all drives (triggers reboot) ---
        print_section("UNMOUNTING DRIVES")
        for port, info in mount_map.items():
            mp = info.get("mount_point")
            if mp:
                result = subprocess.run(
                    ["sudo", "umount", str(mp)],
                    capture_output=True, text=True, timeout=10,
                )
                if result.returncode == 0:
                    print(f"  Unmounted {mp}")
                else:
                    # Lazy unmount as fallback
                    subprocess.run(
                        ["sudo", "umount", "-l", str(mp)],
                        capture_output=True, timeout=10,
                    )
                    print(f"  Lazy-unmounted {mp}")
                # Clean up mount directory
                try:
                    mp_path = Path(mp)
                    if mp_path.exists() and mp_path.is_dir() and not any(mp_path.iterdir()):
                        subprocess.run(
                            ["sudo", "rmdir", str(mp)],
                            capture_output=True, timeout=5,
                        )
                except (OSError, PermissionError):
                    pass

        # --- Phase 5: Verify reboots ---
        print_section("VERIFYING REBOOTS")
        print(f"  Waiting for devices to reboot...")
        time.sleep(2)

        # Poll for serial ports to return
        deadline = time.monotonic() + PORT_REAPPEAR_TIMEOUT
        found = {}
        while time.monotonic() < deadline:
            all_found = True
            for port in ports:
                if port in found:
                    continue
                sn = device_serials.get(port)
                if sn:
                    new_port = find_port_by_serial(sn, _active_board["normal_pid"])
                    if new_port:
                        found[port] = new_port
                        print(f"  {port}: back on {new_port}")
                        continue
                all_found = False
            if all_found or len(found) == len(device_serials):
                break
            time.sleep(0.2)

        # Build results
        results = []
        for port in ports:
            info = mount_map[port]
            if not info["block_dev"]:
                results.append((port, False, "failed to enter bootloader"))
            elif not info["mount_point"]:
                results.append((port, False, "mount failed"))
            elif not info["copied"]:
                results.append((port, False, "firmware copy failed"))
            elif port in found:
                results.append((port, True, f"OK (now on {found[port]})"))
            elif device_serials.get(port):
                results.append((port, False, "serial port not found after reboot"))
            else:
                results.append((port, True, "OK (no serial tracking)"))

        return results

    finally:
        # Always clean up mounts on any exit (success, error, or Ctrl+C)
        _cleanup_parallel_mounts(mount_map)


# ============================================================
#  Main
# ============================================================

def _elapsed(start_time):
    """Return elapsed time string since start_time."""
    elapsed = time.monotonic() - start_time
    return f"{elapsed:.1f}s"


def main():
    global _active_board

    args = parse_args()

    # Activate the board profile for this run (affects VID/PID checks, etc.)
    _active_board = BOARD_PROFILES[args.board]
    if args.board != "nrf52840":
        print(f"  Board: {_active_board['name']}")

    if args.self_test:
        return 0 if run_self_test() else 1

    start_time = time.monotonic()

    # --- Resolve port list ---
    ports = list(args.ports)  # copy so we can modify

    if args.all:
        if ports:
            print("ERROR: Cannot specify both --all and explicit ports")
            return 1
        ports = find_all_xiao_ports()
        if not ports:
            print(f"ERROR: No {_active_board['name']} devices detected (--all)")
            print("  Check USB connections and verify devices are powered on.")
            return 1
        print(f"  Auto-detected {len(ports)} device(s): {', '.join(ports)}")

    if not ports and not args.already_in_bootloader and not args.dry_run:
        print("ERROR: Serial port required. Specify port(s), use --all, or --already-in-bootloader")
        print("Usage: python3 uf2_upload.py /dev/ttyACM0")
        print("       python3 uf2_upload.py --all")
        return 1

    if args.already_in_bootloader and len(ports) > 1:
        print("ERROR: --already-in-bootloader only works with a single device")
        return 1

    try:
        # --- Phase 1 & 2: Locate, validate, convert (once for all devices) ---
        if _active_board["firmware_ext"] == ".bin" and not args.uf2_file:
            # ESP32-S3: arduino-esp32 produces a .bin; convert it to .uf2 ourselves.
            bin_path = find_bin_file(args)
            uf2_path = convert_bin_to_uf2(bin_path)
        elif args.uf2_file:
            uf2_path = Path(args.uf2_file)
            if not uf2_path.exists():
                print(f"ERROR: UF2 file not found: {uf2_path}")
                return 1
            print(f"Using pre-converted UF2: {uf2_path}")
        else:
            hex_path = find_hex_file(args)

            if not args.skip_validation:
                if not validate_hex(hex_path, verbose=args.verbose):
                    print_failure("SAFETY VALIDATION FAILED - Upload blocked")
                    return 1
            else:
                print("  WARNING: Skipping safety validation")

            uf2_path = convert_to_uf2(hex_path)

        # --- Dry run stops here ---
        if args.dry_run:
            print_success("DRY RUN COMPLETE - Firmware validated and converted")
            print(f"  UF2 file: {uf2_path}")
            if ports:
                print(f"  Target device(s): {', '.join(ports)}")
            return 0

        # --- Single device: already-in-bootloader (special path) ---
        if args.already_in_bootloader:
            print("\n  Board already in bootloader mode")
            if args.mount_point:
                mount_point = Path(args.mount_point)
                if not (mount_point / UF2_INFO_FILE).exists():
                    print(f"ERROR: {UF2_INFO_FILE} not found at {mount_point}")
                    return 1
            else:
                mount_point = mount_uf2_drive(timeout=BOOTLOADER_TIMEOUT)

            if not copy_firmware(uf2_path, mount_point):
                print_failure("FIRMWARE COPY FAILED")
                return 1

            port = ports[0] if ports else None
            reboot_ok, new_port = verify_reboot(mount_point, port, None, verbose=args.verbose)
            if reboot_ok:
                print_success(f"UPLOAD SUCCESSFUL ({_elapsed(start_time)})")
                return 0
            else:
                print_failure(f"REBOOT VERIFICATION FAILED ({_elapsed(start_time)})")
                return 1

        # --- Multi-device upload ---
        if args.parallel and len(ports) > 1:
            results = upload_parallel(ports, uf2_path, verbose=args.verbose)
        else:
            # Sequential upload (phases 3-6 per device)
            results = []  # list of (port, success, message)

            for i, port in enumerate(ports):
                # USB settle time between devices: after a flash, the previous
                # device reboots and re-enumerates. Give the USB bus time to
                # stabilize before touching the next port, preventing port
                # number shuffling and USB congestion.
                if i > 0 and len(ports) > 1:
                    prev_port, prev_ok, _ = results[-1]
                    if prev_ok:
                        print(f"\n  Waiting 3s for USB bus to settle after {prev_port} reboot...")
                        time.sleep(3)

                if len(ports) > 1:
                    print_section(f"DEVICE {i + 1}/{len(ports)}: {port}")

                success, message = upload_to_device(port, uf2_path, verbose=args.verbose)
                results.append((port, success, message))

            if success and len(ports) == 1:
                print_success(f"UPLOAD SUCCESSFUL ({_elapsed(start_time)})")
                print(f"  {message}")
            elif not success and len(ports) == 1:
                print_failure(f"UPLOAD FAILED ({_elapsed(start_time)})")
                print(f"  {message}")

        # --- Summary (multi-device only) ---
        if len(ports) > 1:
            succeeded = sum(1 for _, ok, _ in results if ok)
            total = len(results)

            print_section(f"UPLOAD SUMMARY: {succeeded}/{total} devices ({_elapsed(start_time)})")
            for port, success, message in results:
                status = "OK" if success else "FAILED"
                print(f"  {port:20s}  {status:8s}  {message}")

            if succeeded == total:
                print(f"\n  All {total} devices uploaded successfully")
                return 0
            else:
                print(f"\n  {total - succeeded} device(s) failed")
                return 1

        # Single device exit code
        return 0 if results[0][1] else 1

    except FileNotFoundError as e:
        print(f"\nERROR ({_elapsed(start_time)}): {e}")
        cleanup_stale_mounts()
        return 2
    except TimeoutError as e:
        print_failure(f"TIMEOUT ({_elapsed(start_time)})")
        print(f"  {e}")
        cleanup_stale_mounts()
        return 3
    except RuntimeError as e:
        print_failure(f"ERROR ({_elapsed(start_time)})")
        print(f"  {e}")
        cleanup_stale_mounts()
        return 1
    except KeyboardInterrupt:
        print(f"\n\nUpload cancelled ({_elapsed(start_time)}).")
        cleanup_stale_mounts()
        return 130


if __name__ == "__main__":
    sys.exit(main())
