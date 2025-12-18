#!/usr/bin/env python3
"""
Pre-Upload Safety Validator for nRF52840

Validates compiled firmware BEFORE upload to prevent bootloader corruption.
Run this after compile but before upload.

Usage:
    python pre_upload_check.py path/to/firmware.hex
    python pre_upload_check.py  # Uses default build path

Checks:
1. All flash addresses are in safe application region (not bootloader)
2. Total binary size is within limits
3. No addresses below APPLICATION_START
4. No addresses above FLASH_END
5. Validates Intel HEX file integrity (checksums)
"""

import sys
import os
import argparse
from pathlib import Path

# nRF52840 Memory Layout for Seeeduino XIAO BLE (mbed core)
# Conservative safety margins to prevent bootloader corruption
FLASH_START = 0x00000000
FLASH_END = 0x00100000       # 1MB total flash
BOOTLOADER_REGION_END = 0x00027000  # UF2 bootloader typically ends here
APPLICATION_START = 0x00027000      # Safe application region starts here
CONFIG_REGION_START = 0x000FF000    # Last 4KB reserved for config storage
MAX_APPLICATION_SIZE = CONFIG_REGION_START - APPLICATION_START  # ~856KB

# Safety margin - refuse if code starts below this
ABSOLUTE_MIN_ADDRESS = 0x00020000  # 128KB - absolute minimum safe start

class HexValidationError(Exception):
    """Raised when hex file validation fails"""
    pass

class SafetyError(Exception):
    """Raised when a safety check fails - DO NOT UPLOAD"""
    pass

def parse_intel_hex(hex_path: str) -> dict:
    """
    Parse Intel HEX file and extract address ranges.

    Returns dict with:
        - 'addresses': list of (start_addr, end_addr) tuples for each data block
        - 'min_addr': lowest address in file
        - 'max_addr': highest address in file
        - 'total_bytes': total data bytes
        - 'segments': list of segment base addresses
    """
    addresses = []
    segments = []
    min_addr = float('inf')
    max_addr = 0
    total_bytes = 0

    extended_addr = 0  # Extended address from type 02 or 04 records

    with open(hex_path, 'r') as f:
        line_num = 0
        for line in f:
            line_num += 1
            line = line.strip()

            if not line or not line.startswith(':'):
                continue

            # Parse record
            try:
                byte_count = int(line[1:3], 16)
                address = int(line[3:7], 16)
                record_type = int(line[7:9], 16)
                data = line[9:-2]  # Exclude checksum
                checksum = int(line[-2:], 16)

                # Verify checksum
                calc_sum = byte_count + (address >> 8) + (address & 0xFF) + record_type
                for i in range(0, len(data), 2):
                    calc_sum += int(data[i:i+2], 16)
                calc_sum = (~calc_sum + 1) & 0xFF

                if calc_sum != checksum:
                    raise HexValidationError(
                        f"Line {line_num}: Checksum mismatch (expected {checksum:02X}, got {calc_sum:02X})"
                    )

            except ValueError as e:
                raise HexValidationError(f"Line {line_num}: Parse error - {e}")

            # Handle record types
            if record_type == 0x00:  # Data record
                physical_addr = extended_addr + address
                end_addr = physical_addr + byte_count

                addresses.append((physical_addr, end_addr))
                min_addr = min(min_addr, physical_addr)
                max_addr = max(max_addr, end_addr)
                total_bytes += byte_count

            elif record_type == 0x01:  # End of file
                break

            elif record_type == 0x02:  # Extended segment address
                # Address = (segment << 4) + offset
                segment = int(data, 16)
                extended_addr = segment << 4
                segments.append(extended_addr)

            elif record_type == 0x04:  # Extended linear address
                # Address = (upper16 << 16) + offset
                upper = int(data, 16)
                extended_addr = upper << 16
                segments.append(extended_addr)

            elif record_type == 0x03:  # Start segment address (ignored)
                pass

            elif record_type == 0x05:  # Start linear address (ignored)
                pass

            else:
                print(f"  Warning: Unknown record type {record_type:02X} at line {line_num}")

    if min_addr == float('inf'):
        min_addr = 0

    return {
        'addresses': addresses,
        'min_addr': min_addr,
        'max_addr': max_addr,
        'total_bytes': total_bytes,
        'segments': segments
    }

def validate_safety(hex_data: dict, verbose: bool = True) -> list:
    """
    Run safety checks on parsed hex data.

    Returns list of (check_name, passed, message) tuples.
    Raises SafetyError for critical failures.
    """
    results = []
    critical_failure = False

    min_addr = hex_data['min_addr']
    max_addr = hex_data['max_addr']
    total_bytes = hex_data['total_bytes']

    # Check 1: Minimum address (CRITICAL)
    if min_addr < ABSOLUTE_MIN_ADDRESS:
        results.append((
            "BOOTLOADER PROTECTION",
            False,
            f"CRITICAL: Code starts at 0x{min_addr:08X}, below safe minimum 0x{ABSOLUTE_MIN_ADDRESS:08X}!"
        ))
        critical_failure = True
    elif min_addr < APPLICATION_START:
        results.append((
            "BOOTLOADER PROTECTION",
            False,
            f"WARNING: Code at 0x{min_addr:08X} is below expected app start 0x{APPLICATION_START:08X}"
        ))
    else:
        results.append((
            "BOOTLOADER PROTECTION",
            True,
            f"OK - Code starts at 0x{min_addr:08X} (safe region)"
        ))

    # Check 2: Maximum address
    if max_addr > FLASH_END:
        results.append((
            "FLASH BOUNDS",
            False,
            f"CRITICAL: Code extends to 0x{max_addr:08X}, beyond flash end 0x{FLASH_END:08X}!"
        ))
        critical_failure = True
    elif max_addr > CONFIG_REGION_START:
        results.append((
            "FLASH BOUNDS",
            False,
            f"WARNING: Code at 0x{max_addr:08X} overlaps config region 0x{CONFIG_REGION_START:08X}"
        ))
    else:
        results.append((
            "FLASH BOUNDS",
            True,
            f"OK - Code ends at 0x{max_addr:08X}"
        ))

    # Check 3: Total size
    code_size = max_addr - min_addr
    if code_size > MAX_APPLICATION_SIZE:
        results.append((
            "APPLICATION SIZE",
            False,
            f"Code size {code_size:,} bytes exceeds max {MAX_APPLICATION_SIZE:,} bytes"
        ))
    else:
        pct = (code_size / MAX_APPLICATION_SIZE) * 100
        results.append((
            "APPLICATION SIZE",
            True,
            f"OK - {code_size:,} bytes ({pct:.1f}% of {MAX_APPLICATION_SIZE:,} available)"
        ))

    # Check 4: Suspicious addresses (any data in bootloader region)
    bootloader_writes = []
    for start, end in hex_data['addresses']:
        if start < BOOTLOADER_REGION_END:
            bootloader_writes.append((start, end))

    if bootloader_writes:
        results.append((
            "BOOTLOADER REGION",
            False,
            f"CRITICAL: {len(bootloader_writes)} data blocks target bootloader region!"
        ))
        for start, end in bootloader_writes[:5]:  # Show first 5
            if verbose:
                print(f"    - 0x{start:08X} - 0x{end:08X}")
        critical_failure = True
    else:
        results.append((
            "BOOTLOADER REGION",
            True,
            "OK - No writes to bootloader region"
        ))

    # Check 5: Segment addresses look reasonable
    if hex_data['segments']:
        seg_ok = all(s >= ABSOLUTE_MIN_ADDRESS for s in hex_data['segments'])
        if seg_ok:
            results.append((
                "SEGMENT ADDRESSES",
                True,
                f"OK - {len(hex_data['segments'])} segments, all in safe region"
            ))
        else:
            bad_segs = [s for s in hex_data['segments'] if s < ABSOLUTE_MIN_ADDRESS]
            results.append((
                "SEGMENT ADDRESSES",
                False,
                f"WARNING: Segments point to low addresses: {[hex(s) for s in bad_segs]}"
            ))

    if critical_failure:
        raise SafetyError("CRITICAL SAFETY FAILURES DETECTED - DO NOT UPLOAD")

    return results

def validate_source_constants(sketch_dir: str = None) -> list:
    """
    Validate that SafetyTest.h constants match pre-upload checker.

    This ensures runtime and pre-upload validation agree on memory boundaries.
    Returns list of (check_name, passed, message) tuples.
    """
    results = []

    if sketch_dir is None:
        script_dir = Path(__file__).parent.parent
        sketch_dir = script_dir / "blinky-things"

    safety_header = Path(sketch_dir) / "tests" / "SafetyTest.h"

    if not safety_header.exists():
        results.append((
            "SOURCE CONSTANTS",
            True,  # Don't fail if file missing - may be different project structure
            f"SafetyTest.h not found (skipped)"
        ))
        return results

    try:
        content = safety_header.read_text()

        # Extract constants from source
        import re

        bootloader_match = re.search(r'BOOTLOADER_END\s*=\s*(0x[0-9A-Fa-f]+)', content)
        flash_end_match = re.search(r'FLASH_END\s*=\s*(0x[0-9A-Fa-f]+)', content)

        if bootloader_match:
            source_bootloader = int(bootloader_match.group(1), 16)
            # The source uses a conservative 0x30000, we use 0x27000 based on actual layout
            # Both are safe - source is MORE conservative which is fine
            if source_bootloader >= BOOTLOADER_REGION_END:
                results.append((
                    "SOURCE BOOTLOADER_END",
                    True,
                    f"OK - Source uses 0x{source_bootloader:08X} (>= checker 0x{BOOTLOADER_REGION_END:08X})"
                ))
            else:
                results.append((
                    "SOURCE BOOTLOADER_END",
                    False,
                    f"Source BOOTLOADER_END 0x{source_bootloader:08X} < checker limit 0x{BOOTLOADER_REGION_END:08X}"
                ))
        else:
            results.append((
                "SOURCE BOOTLOADER_END",
                False,
                "Could not find BOOTLOADER_END in SafetyTest.h"
            ))

        if flash_end_match:
            source_flash_end = int(flash_end_match.group(1), 16)
            if source_flash_end == FLASH_END:
                results.append((
                    "SOURCE FLASH_END",
                    True,
                    f"OK - Source and checker agree: 0x{FLASH_END:08X}"
                ))
            else:
                results.append((
                    "SOURCE FLASH_END",
                    False,
                    f"Mismatch: source 0x{source_flash_end:08X} != checker 0x{FLASH_END:08X}"
                ))
        else:
            results.append((
                "SOURCE FLASH_END",
                False,
                "Could not find FLASH_END in SafetyTest.h"
            ))

    except Exception as e:
        results.append((
            "SOURCE CONSTANTS",
            False,
            f"Error reading SafetyTest.h: {e}"
        ))

    return results

def find_hex_file(build_path: str = None) -> str:
    """Find the hex file to validate."""

    if build_path and os.path.isfile(build_path):
        return build_path

    # Default locations to search
    script_dir = Path(__file__).parent.parent
    search_paths = [
        script_dir / "blinky-things" / "build" / "Seeeduino.mbed.xiaonRF52840Sense" / "blinky-things.ino.hex",
        script_dir / "build" / "blinky-things.ino.hex",
        Path.cwd() / "build" / "Seeeduino.mbed.xiaonRF52840Sense" / "blinky-things.ino.hex",
    ]

    for path in search_paths:
        if path.exists():
            return str(path)

    raise FileNotFoundError(
        f"No hex file found. Searched:\n" +
        "\n".join(f"  - {p}" for p in search_paths) +
        "\n\nSpecify path: python pre_upload_check.py <path/to/firmware.hex>"
    )

def run_self_test():
    """
    Run self-test to verify safety checks work correctly.
    Creates fake hex data and ensures checks catch dangerous configurations.
    """
    print("\n" + "="*60)
    print("  PRE-UPLOAD CHECKER SELF-TEST")
    print("="*60 + "\n")

    tests_passed = 0
    tests_failed = 0

    # Test 1: Normal safe firmware should pass
    print("Test 1: Safe firmware addresses...")
    safe_data = {
        'addresses': [(0x27000, 0x30000)],
        'min_addr': 0x27000,
        'max_addr': 0x30000,
        'total_bytes': 0x9000,
        'segments': [0x20000]
    }
    try:
        results = validate_safety(safe_data, verbose=False)
        all_pass = all(r[1] for r in results)
        if all_pass:
            print("  [PASS] Safe firmware passed validation")
            tests_passed += 1
        else:
            print("  [FAIL] Safe firmware should pass but got failures")
            tests_failed += 1
    except SafetyError:
        print("  [FAIL] Safe firmware raised SafetyError incorrectly")
        tests_failed += 1

    # Test 2: Bootloader region write should fail
    print("Test 2: Bootloader region write detection...")
    dangerous_data = {
        'addresses': [(0x10000, 0x15000), (0x27000, 0x30000)],  # First block in bootloader!
        'min_addr': 0x10000,
        'max_addr': 0x30000,
        'total_bytes': 0x14000,
        'segments': [0x10000]
    }
    try:
        results = validate_safety(dangerous_data, verbose=False)
        print("  [FAIL] Dangerous firmware should have raised SafetyError")
        tests_failed += 1
    except SafetyError:
        print("  [PASS] Correctly blocked bootloader region write")
        tests_passed += 1

    # Test 3: Flash overflow should fail
    print("Test 3: Flash overflow detection...")
    overflow_data = {
        'addresses': [(0x27000, 0x110000)],  # Extends past 1MB!
        'min_addr': 0x27000,
        'max_addr': 0x110000,
        'total_bytes': 0xE9000,
        'segments': [0x20000]
    }
    try:
        results = validate_safety(overflow_data, verbose=False)
        print("  [FAIL] Flash overflow should have raised SafetyError")
        tests_failed += 1
    except SafetyError:
        print("  [PASS] Correctly blocked flash overflow")
        tests_passed += 1

    # Test 4: Code starting too low should fail
    print("Test 4: Low address detection...")
    low_data = {
        'addresses': [(0x5000, 0x10000)],  # Way too low!
        'min_addr': 0x5000,
        'max_addr': 0x10000,
        'total_bytes': 0xB000,
        'segments': [0x0]
    }
    try:
        results = validate_safety(low_data, verbose=False)
        print("  [FAIL] Low address should have raised SafetyError")
        tests_failed += 1
    except SafetyError:
        print("  [PASS] Correctly blocked low address firmware")
        tests_passed += 1

    print()
    print(f"Self-test complete: {tests_passed} passed, {tests_failed} failed")
    print()

    return tests_failed == 0

def main():
    parser = argparse.ArgumentParser(
        description="Pre-upload safety validator for nRF52840 firmware",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Memory regions (nRF52840 Seeeduino XIAO BLE):
  Bootloader:   0x00000000 - 0x00027000  (DO NOT WRITE)
  Application:  0x00027000 - 0x000FF000  (safe zone)
  Config:       0x000FF000 - 0x00100000  (reserved)

This tool prevents bootloader corruption by validating the hex file
addresses BEFORE upload. Run after compile, before upload.
        """
    )
    parser.add_argument('hex_file', nargs='?', help='Path to .hex file (optional)')
    parser.add_argument('-v', '--verbose', action='store_true', help='Verbose output')
    parser.add_argument('-q', '--quiet', action='store_true', help='Only output on failure')
    parser.add_argument('--self-test', action='store_true', help='Run self-test to verify checker works')

    args = parser.parse_args()

    # Run self-test if requested
    if args.self_test:
        success = run_self_test()
        return 0 if success else 1

    try:
        hex_path = find_hex_file(args.hex_file)
    except FileNotFoundError as e:
        print(f"ERROR: {e}")
        return 2

    if not args.quiet:
        print(f"\n{'='*60}")
        print("  PRE-UPLOAD SAFETY VALIDATOR")
        print(f"{'='*60}")
        print(f"\nValidating: {hex_path}")
        print()

    # Parse hex file
    try:
        hex_data = parse_intel_hex(hex_path)
    except HexValidationError as e:
        print(f"HEX FILE ERROR: {e}")
        return 1
    except Exception as e:
        print(f"ERROR reading hex file: {e}")
        return 1

    if not args.quiet:
        print(f"Address range: 0x{hex_data['min_addr']:08X} - 0x{hex_data['max_addr']:08X}")
        print(f"Total data:    {hex_data['total_bytes']:,} bytes")
        print()

    # Run safety checks
    try:
        results = validate_safety(hex_data, verbose=args.verbose)

        # Also validate source code constants match
        source_results = validate_source_constants()
        results.extend(source_results)
    except SafetyError as e:
        print(f"\n{'!'*60}")
        print(f"  {e}")
        print(f"{'!'*60}")
        print("\nUpload BLOCKED to prevent device damage.")
        print("Review the hex file and build configuration.")
        return 1

    # Print results
    all_passed = True
    for name, passed, msg in results:
        if not args.quiet:
            status = "PASS" if passed else "FAIL"
            print(f"  [{status}] {name}")
            print(f"          {msg}")
        if not passed:
            all_passed = False

    print()
    if all_passed:
        if not args.quiet:
            print(f"{'='*60}")
            print("  ALL CHECKS PASSED - Safe to upload")
            print(f"{'='*60}")
        return 0
    else:
        print(f"{'!'*60}")
        print("  WARNINGS DETECTED - Review before uploading")
        print(f"{'!'*60}")
        return 0  # Warnings don't block upload, only critical failures

if __name__ == '__main__':
    sys.exit(main())
