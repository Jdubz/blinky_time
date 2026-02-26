#!/usr/bin/env python3
"""
Safe UF2 Firmware Upload for XIAO nRF52840 Sense

Uploads firmware via the UF2 mass storage bootloader, bypassing the
fragile adafruit-nrfutil DFU serial protocol that can brick devices.

Upload workflow:
  1. Validate hex file (address safety checks)
  2. Convert hex -> UF2 (using platform uf2conv.py)
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
"""

import sys
import os
import time
import json
import shutil
import subprocess
import argparse
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial is required. Install with: pip3 install pyserial")
    sys.exit(1)

# --- Hardware constants (from boards.txt) ---
NORMAL_VID = 0x2886
NORMAL_PID = 0x8045
BOOTLOADER_VID = 0x2886
BOOTLOADER_PID = 0x0045

# --- UF2 conversion ---
UF2_FAMILY_ID = "0xADA52840"
def _find_uf2conv():
    """Find uf2conv.py in the installed Seeeduino nRF52 board package."""
    base = Path.home() / ".arduino15/packages/Seeeduino/hardware/nrf52"
    if base.exists():
        # Find the latest installed version
        versions = sorted(base.iterdir(), reverse=True)
        for v in versions:
            candidate = v / "tools/uf2conv/uf2conv.py"
            if candidate.exists():
                return candidate
    # Fallback to a fixed path for error messaging
    return base / "unknown_version" / "tools/uf2conv/uf2conv.py"

UF2CONV_PATH = _find_uf2conv()

# --- Timeouts (seconds) ---
BOOTLOADER_TIMEOUT = 15
DRIVE_MOUNT_TIMEOUT = 15
REBOOT_TIMEOUT = 15
PORT_REAPPEAR_TIMEOUT = 15

# --- UF2 drive identification ---
UF2_INFO_FILE = "INFO_UF2.TXT"


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

    result = subprocess.run(cmd, capture_output=True, text=True)

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

def find_all_xiao_ports():
    """Auto-detect all connected XIAO nRF52840 devices in application mode.

    Returns sorted list of serial port paths (e.g., ['/dev/ttyACM0', '/dev/ttyACM1']).
    """
    return sorted([
        p.device for p in serial.tools.list_ports.comports()
        if p.vid == NORMAL_VID and p.pid == NORMAL_PID
    ])


def get_serial_number(port):
    """Get the USB serial number for a port to track device identity."""
    for p in serial.tools.list_ports.comports():
        if p.device == port:
            return p.serial_number
    return None


def find_port_by_serial(serial_number, target_pid=None):
    """Find a serial port matching a USB serial number and optional PID."""
    if not serial_number:
        return None
    for p in serial.tools.list_ports.comports():
        if p.serial_number == serial_number:
            if target_pid is None or p.pid == target_pid:
                return p.device
    return None


MAX_BOOTLOADER_RETRIES = 5

def trigger_bootloader(port, verbose=False):
    """Enter UF2 bootloader mode with automatic retry.

    The nRF52 SoftDevice can intermittently clear the GPREGRET register
    during reset, causing the bootloader to skip UF2 mode and boot the
    application instead. The BSP fix (SoftDevice API for GPREGRET) makes
    this reliable, but older firmware may still have the race condition.
    Retrying resolves the intermittent failure.

    Returns the USB serial number for tracking the device across
    the mode switch.
    """
    print_section("ENTERING BOOTLOADER")

    device_serial = get_serial_number(port)
    if device_serial:
        print(f"  Device serial: {device_serial}")
    else:
        print(f"  Warning: Could not read serial number from {port}")

    for attempt in range(1, MAX_BOOTLOADER_RETRIES + 1):
        if attempt > 1:
            print(f"  Retry {attempt}/{MAX_BOOTLOADER_RETRIES}...")
            time.sleep(2)  # Let device settle between retries

        pre_existing_blocks = _get_usb_block_devices()

        # Send 'bootloader' serial command
        if attempt == 1:
            print(f"  Trying serial command: bootloader")
        try:
            ser = serial.Serial(port, 115200, timeout=1)
            time.sleep(0.3)
            ser.reset_input_buffer()
            ser.write(b'bootloader\n')
            ser.flush()
            time.sleep(0.3)
            try:
                ser.close()
            except (BrokenPipeError, OSError):
                pass

            if _wait_for_uf2_drive(pre_existing_blocks, timeout=8, verbose=verbose):
                return device_serial

        except (serial.SerialException, OSError) as e:
            if verbose:
                print(f"  Serial error: {e}")

        # On first attempt only, also try 1200-baud touch as fallback
        if attempt == 1:
            pre_existing_blocks = _get_usb_block_devices()
            print(f"  Trying 1200 baud touch on {port}...")
            try:
                ser = serial.Serial()
                ser.port = port
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
                    if verbose:
                        print(f"  Device reset during DTR toggle (expected)")
                try:
                    ser.close()
                except (BrokenPipeError, OSError):
                    pass
                print(f"  1200 baud touch complete")

                if _wait_for_uf2_drive(pre_existing_blocks, timeout=8, verbose=verbose):
                    return device_serial

            except serial.SerialException as e:
                if verbose:
                    print(f"  1200 baud touch failed: {e}")

        print(f"  UF2 drive not detected (attempt {attempt})")

    print(f"  Warning: UF2 drive not detected after {MAX_BOOTLOADER_RETRIES} attempts")
    return device_serial


def _wait_for_uf2_drive(pre_existing_blocks, timeout=8, verbose=False):
    """Wait for a new USB block device to appear (UF2 bootloader mode).

    Returns True if a new USB block device appeared, False if timeout.
    """
    print(f"  Waiting for UF2 drive...")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = _get_usb_block_devices()
        new_devices = current - pre_existing_blocks
        if new_devices:
            dev = sorted(new_devices)[0]
            print(f"  UF2 drive detected: {dev}")
            return True
        time.sleep(0.3)

    print(f"  Warning: No UF2 drive appeared within {timeout}s")
    return False


# ============================================================
#  Phase 4: UF2 drive detection and mounting
# ============================================================

def ensure_udisks2_running():
    """Start udisks2 service if not running. Returns True if running."""
    result = subprocess.run(
        ["systemctl", "is-active", "udisks2"],
        capture_output=True, text=True,
    )
    if result.stdout.strip() == "active":
        return True

    print(f"  Starting udisks2 service...")
    result = subprocess.run(
        ["sudo", "systemctl", "start", "udisks2"],
        capture_output=True, text=True,
    )
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
        time.sleep(0.5)

    return None


def _get_usb_block_devices():
    """Get set of USB block device paths from lsblk."""
    devices = set()
    result = subprocess.run(
        ["lsblk", "-o", "NAME,TRAN,RM,TYPE", "-J"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        return devices

    try:
        data = json.loads(result.stdout)
        for dev in data.get("blockdevices", []):
            if dev.get("tran") == "usb":
                # Check partitions
                for child in dev.get("children", []):
                    if child.get("type") in ("part", "disk"):
                        devices.add(f"/dev/{child['name']}")
                # If no partitions, use disk itself
                if not dev.get("children"):
                    devices.add(f"/dev/{dev['name']}")
    except (json.JSONDecodeError, KeyError):
        pass

    return devices


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
        subprocess.run(
            ["sudo", "umount", "-l", mp],
            capture_output=True, timeout=10,
        )
        # Remove empty directory
        mp_path = Path(mp)
        try:
            if mp_path.exists() and mp_path.is_dir() and not any(mp_path.iterdir()):
                subprocess.run(
                    ["sudo", "rmdir", mp],
                    capture_output=True, timeout=5,
                )
        except (OSError, PermissionError):
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
    result = subprocess.run(
        ["udisksctl", "mount", "--block-device", block_device,
         "--no-user-interaction"],
        capture_output=True, text=True,
    )

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

    subprocess.run(
        ["sudo", "mkdir", "-p", str(mount_point)],
        capture_output=True,
    )

    uid = os.getuid()
    gid = os.getgid()
    result = subprocess.run(
        ["sudo", "mount", "-t", "vfat", "-o",
         f"uid={uid},gid={gid},umask=022",
         block_device, str(mount_point)],
        capture_output=True, text=True,
    )

    if result.returncode == 0:
        if (mount_point / UF2_INFO_FILE).exists():
            print(f"  [PASS] Mounted at {mount_point}")
            return mount_point
        else:
            print(f"  Mounted but {UF2_INFO_FILE} not found (wrong device?)")
            subprocess.run(
                ["sudo", "umount", str(mount_point)],
                capture_output=True,
            )
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
    time.sleep(1)

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
        time.sleep(0.5)

    if not drive_gone:
        print(f"  [WARN] UF2 drive still present after {REBOOT_TIMEOUT}s")
        print(f"  The firmware may have been rejected (wrong family or corrupt).")
        return False

    # Wait for serial port to reappear
    print(f"  Waiting for serial port...")
    deadline = time.monotonic() + PORT_REAPPEAR_TIMEOUT

    while time.monotonic() < deadline:
        if device_serial:
            new_port = find_port_by_serial(device_serial, NORMAL_PID)
            if new_port:
                print(f"  Device back on {new_port}")
                return True
        elif port:
            if Path(port).exists():
                print(f"  Port {port} is back")
                return True
        time.sleep(0.5)

    # Drive disappeared but port didn't return -- partial success
    print(f"  [WARN] Serial port not detected within {PORT_REAPPEAR_TIMEOUT}s")
    print(f"  Firmware was likely accepted (drive disappeared).")
    return True


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
    result = subprocess.run(["which", "udisksctl"], capture_output=True, text=True)
    if result.returncode == 0:
        print(f"  [PASS] {result.stdout.strip()}")
        passed += 1
    else:
        print(f"  [FAIL] Not found (install udisks2)")
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
            ["groups"], capture_output=True, text=True
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
        description="Safe UF2 firmware upload for XIAO nRF52840 Sense",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Upload workflow:
  1. Validate hex file (address safety checks)
  2. Convert hex -> UF2 (family 0xADA52840)
  3. Enter bootloader (1200 baud serial touch)
  4. Mount UF2 drive (udisksctl or manual)
  5. Copy firmware.uf2 to drive
  6. Verify reboot (drive disappears, port returns)

Examples:
  %(prog)s /dev/ttyACM0                          # Single device
  %(prog)s /dev/ttyACM0 /dev/ttyACM1 /dev/ttyACM2  # Multiple devices
  %(prog)s --all                                  # Auto-detect all XIAO devices
  %(prog)s /dev/ttyACM0 --hex firmware.hex
  %(prog)s /dev/ttyACM0 --dry-run
  %(prog)s --already-in-bootloader
  %(prog)s --self-test
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
    return parser.parse_args()


# ============================================================
#  Single-device upload (phases 3-6)
# ============================================================

def upload_to_device(port, uf2_path, verbose=False):
    """Upload firmware to a single device.

    Runs phases 3-6: bootloader entry, drive mount, firmware copy, reboot verify.
    Returns (success: bool, message: str).
    """
    try:
        # Phase 3: Enter bootloader
        device_serial = trigger_bootloader(port, verbose=verbose)

        # Phase 4: Mount UF2 drive
        mount_point = mount_uf2_drive(
            device_serial=device_serial,
            timeout=BOOTLOADER_TIMEOUT,
        )

        # Phase 5: Copy firmware
        if not copy_firmware(uf2_path, mount_point):
            return False, "firmware copy failed"

        # Phase 6: Verify reboot
        if verify_reboot(mount_point, port, device_serial, verbose=verbose):
            new_port = find_port_by_serial(device_serial, NORMAL_PID) if device_serial else None
            msg = f"OK (now on {new_port})" if new_port else "OK"
            return True, msg
        else:
            return False, "reboot verification failed"

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
        for port in ports:
            sn = get_serial_number(port)
            if sn:
                device_serials[port] = sn
                print(f"  {port}: serial={sn}")
            mount_map[port] = {"serial": sn, "block_dev": None,
                               "mount_point": None, "copied": False}

        # --- Phase 2: Staggered bootloader entry + per-device mount ---
        print_section("STAGGERED BOOTLOADER ENTRY")

        for i, port in enumerate(ports):
            if i > 0:
                print(f"\n  Waiting 2s for USB bus to settle...")
                time.sleep(2)

            print(f"  Device {i + 1}/{len(ports)}: {port}")

            # Snapshot block devices before bootloader command
            pre_blocks = _get_usb_block_devices()

            # Send bootloader command (with retries)
            entered = False
            for attempt in range(1, MAX_BOOTLOADER_RETRIES + 1):
                if attempt > 1:
                    print(f"    Retry {attempt}/{MAX_BOOTLOADER_RETRIES}...")
                    time.sleep(2)

                try:
                    ser = serial.Serial(port, 115200, timeout=1)
                    time.sleep(0.3)
                    ser.reset_input_buffer()
                    ser.write(b'bootloader\n')
                    ser.flush()
                    time.sleep(0.3)
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
                # TODO: Assignment takes the lexicographically first new block device.
                # If a slow device from a previous step appears late, it could be
                # mis-assigned. The 2s stagger makes this unlikely. A robust fix would
                # match by USB serial number via /sys/block/sdX/device/../../serial.
                print(f"    Waiting for UF2 drive...")
                deadline = time.monotonic() + 8
                new_dev = None
                while time.monotonic() < deadline:
                    current = _get_usb_block_devices()
                    new_devices = current - pre_blocks
                    if new_devices:
                        new_dev = sorted(new_devices)[0]
                        break
                    time.sleep(0.3)

                if new_dev:
                    print(f"    UF2 drive detected: {new_dev}")
                    mount_map[port]["block_dev"] = new_dev
                    entered = True
                    break
                else:
                    print(f"    No UF2 drive appeared (attempt {attempt})")
                    # On first attempt, also try 1200-baud touch
                    if attempt == 1:
                        pre_blocks = _get_usb_block_devices()
                        print(f"    Trying 1200 baud touch...")
                        try:
                            ser = serial.Serial()
                            ser.port = port
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

                            deadline = time.monotonic() + 8
                            while time.monotonic() < deadline:
                                current = _get_usb_block_devices()
                                new_devices = current - pre_blocks
                                if new_devices:
                                    new_dev = sorted(new_devices)[0]
                                    break
                                time.sleep(0.3)

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
            time.sleep(1)  # Let device settle after mode switch
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
        time.sleep(3)

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
                    new_port = find_port_by_serial(sn, NORMAL_PID)
                    if new_port:
                        found[port] = new_port
                        print(f"  {port}: back on {new_port}")
                        continue
                all_found = False
            if all_found or len(found) == len(device_serials):
                break
            time.sleep(0.5)

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

def main():
    args = parse_args()

    if args.self_test:
        return 0 if run_self_test() else 1

    # --- Resolve port list ---
    ports = list(args.ports)  # copy so we can modify

    if args.all:
        if ports:
            print("ERROR: Cannot specify both --all and explicit ports")
            return 1
        ports = find_all_xiao_ports()
        if not ports:
            print("ERROR: No XIAO devices detected (--all)")
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
        if args.uf2_file:
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
            if verify_reboot(mount_point, port, None, verbose=args.verbose):
                print_success("UPLOAD SUCCESSFUL")
                return 0
            else:
                print_failure("REBOOT VERIFICATION FAILED")
                return 1

        # --- Multi-device upload ---
        if args.parallel and len(ports) > 1:
            results = upload_parallel(ports, uf2_path, verbose=args.verbose)
        else:
            # Sequential upload (phases 3-6 per device)
            results = []  # list of (port, success, message)

            for i, port in enumerate(ports):
                if len(ports) > 1:
                    print_section(f"DEVICE {i + 1}/{len(ports)}: {port}")

                success, message = upload_to_device(port, uf2_path, verbose=args.verbose)
                results.append((port, success, message))

            if success and len(ports) == 1:
                print_success("UPLOAD SUCCESSFUL")
                print(f"  {message}")
            elif not success and len(ports) == 1:
                print_failure("UPLOAD FAILED")
                print(f"  {message}")

        # --- Summary (multi-device only) ---
        if len(ports) > 1:
            succeeded = sum(1 for _, ok, _ in results if ok)
            total = len(results)

            print_section(f"UPLOAD SUMMARY: {succeeded}/{total} devices")
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
        print(f"\nERROR: {e}")
        cleanup_stale_mounts()
        return 2
    except TimeoutError as e:
        print_failure("TIMEOUT")
        print(f"  {e}")
        cleanup_stale_mounts()
        return 3
    except RuntimeError as e:
        print_failure("ERROR")
        print(f"  {e}")
        cleanup_stale_mounts()
        return 1
    except KeyboardInterrupt:
        print("\n\nUpload cancelled.")
        cleanup_stale_mounts()
        return 130


if __name__ == "__main__":
    sys.exit(main())
