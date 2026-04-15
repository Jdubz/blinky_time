#!/usr/bin/env python3
"""
uf2_upload_win.py - Windows UF2 uploader for nRF52840 (XIAO Sense)

Usage:
    python tools/uf2_upload_win.py COM44 --build-dir /tmp/blinky-build
    python tools/uf2_upload_win.py COM44 --uf2 path/to/firmware.uf2

Steps:
    1. Touch COM port at 1200 baud to trigger bootloader entry
    2. Wait for XIAO-SENSE drive to appear as a Windows drive letter
    3. Copy .uf2 file to the drive
"""

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

import serial

DRIVE_LABEL = "XIAO-SENSE"
BOOTLOADER_TIMEOUT = 30  # seconds to wait for drive to appear


def find_uf2_drive(label=DRIVE_LABEL, timeout=BOOTLOADER_TIMEOUT):
    """Poll for a Windows drive with the given volume label."""
    print(f"  Waiting for {label} drive (up to {timeout}s)...")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = subprocess.run(
            ["powershell", "-Command",
             f"Get-WmiObject Win32_Volume | Where-Object {{$_.Label -eq '{label}'}} | Select-Object -ExpandProperty DriveLetter"],
            capture_output=True, text=True, timeout=5
        )
        drive = result.stdout.strip()
        if drive:
            return drive  # e.g. "E:"
        time.sleep(0.5)
    return None


def touch_bootloader(port):
    """Open port at 1200 baud to trigger UF2 bootloader entry."""
    print(f"  Touching {port} at 1200 baud...")
    try:
        ser = serial.Serial(port, 1200, timeout=1)
        ser.dtr = True
        time.sleep(0.1)
        try:
            ser.dtr = False
            time.sleep(0.05)
            ser.dtr = True
            time.sleep(0.1)
        except (BrokenPipeError, OSError):
            pass  # Device reset during DTR toggle — expected
        ser.close()
        print(f"  Bootloader touch complete")
    except serial.SerialException as e:
        print(f"  Warning: serial touch failed ({e}) — device may already be in bootloader mode")


def find_uf2_file(build_dir):
    candidates = list(Path(build_dir).glob("*.uf2"))
    if not candidates:
        return None
    return candidates[0]


def main():
    parser = argparse.ArgumentParser(description="Flash nRF52840 via UF2 on Windows")
    parser.add_argument("port", help="Serial port (e.g. COM44)")
    parser.add_argument("--build-dir", help="Directory containing .uf2 file")
    parser.add_argument("--uf2", help="Path to .uf2 file directly")
    parser.add_argument("--already-in-bootloader", action="store_true",
                        help="Skip 1200 baud touch (device already in bootloader)")
    args = parser.parse_args()

    # Resolve UF2 file
    if args.uf2:
        uf2_path = Path(args.uf2)
    elif args.build_dir:
        uf2_path = find_uf2_file(args.build_dir)
        if not uf2_path:
            print(f"ERROR: No .uf2 file found in {args.build_dir}")
            sys.exit(1)
    else:
        print("ERROR: Specify --build-dir or --uf2")
        sys.exit(1)

    if not uf2_path.exists():
        print(f"ERROR: UF2 file not found: {uf2_path}")
        sys.exit(1)

    print(f"\n{'='*50}")
    print(f"  UF2 Flash: {uf2_path.name} -> {args.port}")
    print(f"{'='*50}\n")

    # Step 1: Enter bootloader
    if not args.already_in_bootloader:
        touch_bootloader(args.port)
    else:
        print(f"  Skipping bootloader touch (--already-in-bootloader)")

    # Step 2: Wait for drive
    drive = find_uf2_drive(DRIVE_LABEL)
    if not drive:
        print(f"\nERROR: {DRIVE_LABEL} drive did not appear after {BOOTLOADER_TIMEOUT}s")
        print()
        print("  On Windows, the software bootloader trigger (serial command / 1200-baud")
        print("  touch) is unreliable: the Windows USB host power-cycles the port after")
        print("  NVIC_SystemReset(), clearing the RAM magic before the bootloader reads it.")
        print()
        print("  RELIABLE METHOD — double-tap the reset button on the device:")
        print("    1. Press reset once  (device reboots, bootloader starts, writes magic)")
        print("    2. Press reset again within ~0.5s")
        print("    3. XIAO-SENSE drive appears")
        print("    4. Re-run:  python tools/uf2_upload_win.py --already-in-bootloader \\")
        print(f"                    --build-dir {args.build_dir or '<build-dir>'}")
        sys.exit(1)

    print(f"  Drive detected: {drive}\\")

    # Step 3: Copy UF2
    dest = Path(drive + "\\") / uf2_path.name
    print(f"  Copying {uf2_path.name} to {drive}\\...")
    shutil.copy2(str(uf2_path), str(dest))
    print(f"  Copy complete — device will reboot automatically\n")
    print(f"  Flash successful!")


if __name__ == "__main__":
    main()
