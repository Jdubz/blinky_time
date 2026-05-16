#!/usr/bin/env python3
"""
verify_bootloader.py — verify the Adafruit_nRF52_Bootloader source preserves
the device-recoverability invariant before allowing a build to be flashed.

THE INVARIANT
-------------
The bootloader's DFU-entry function (check_dfu_mode in src/main.c) MUST call
usb_init() on EVERY code path that enters DFU mode. This guarantees that a
device entering DFU — for ANY reason, including invalid-app auto-recovery
via DEFAULT_TO_OTA_DFU — exposes its USB MSC drive to the host. Without
USB, a device that falls into auto-recovery is unreachable unless a BLE
host is in range.

The 2026-05-15 hat incident: commit 5d85d25 introduced an OTA-only branch
that called ble_stack_init() but skipped usb_init(). When the hat's firmware
quarantined itself, the BL booted into the OTA path, did not initialize USB,
and the device disappeared from the host. Recovery required SWD wiring.
This script enforces that this pattern cannot recur.

WHAT IT CHECKS
--------------
Scans the bootloader source tree for:

  1. EVERY `if (_ota_dfu)` / `else` block inside check_dfu_mode MUST have
     usb_init() called in BOTH branches (or unconditionally before/after).
  2. If usb_init is never called inside check_dfu_mode, FAIL.
  3. If only one branch of an _ota_dfu conditional calls usb_init, FAIL.

The script is intentionally conservative — it requires literal `usb_init`
text in both arms of every relevant conditional. Refactor-friendly: as long
as the calls are visible in source, the check passes.

USAGE
-----
  python3 scripts/verify_bootloader.py <path-to-Adafruit_nRF52_Bootloader>

  # In CI / deploy-bootloader.sh:
  python3 scripts/verify_bootloader.py /path/to/bootloader || exit 1

Returns 0 if invariant holds, non-zero with explanation otherwise.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path


def slurp(path: Path) -> str:
    with open(path, "r") as f:
        return f.read()


def find_function_body(src: str, name: str) -> tuple[int, int] | None:
    """Return (start, end) byte offsets of `name(...) { ... }` in source.

    Naive but adequate for this BL — looks for the function definition line
    (no preceding `if`/`else`/etc.) and brace-matches to find the body.
    """
    # Function definition pattern: "static void check_dfu_mode(void)" or similar
    pat = re.compile(r"^\s*(?:static\s+)?\w[\w\s*]*?\b" + re.escape(name) + r"\s*\([^)]*\)\s*$",
                     re.MULTILINE)
    for m in pat.finditer(src):
        # Find the opening brace after this header
        i = m.end()
        while i < len(src) and src[i] in " \t\r\n":
            i += 1
        if i >= len(src) or src[i] != "{":
            continue
        # Brace match
        depth = 0
        body_start = i
        while i < len(src):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    return (body_start, i + 1)
            i += 1
    return None


def strip_comments(src: str) -> str:
    """Remove C/C++ comments to simplify pattern matching."""
    # Block comments
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    # Line comments
    src = re.sub(r"//[^\n]*", "", src)
    return src


def find_ota_branches(body: str) -> list[tuple[str, str, int, int]]:
    """
    Find all if/else blocks gated on _ota_dfu inside the body.
    Returns a list of (if_branch_text, else_branch_text, if_start_pos,
    block_end_pos) tuples, where block_end_pos is the position immediately
    after the closing brace of the else branch (or after the if branch if
    there's no else).
    """
    results: list[tuple[str, str, int, int]] = []
    # Search for "if ( _ota_dfu )" then brace-match for if-body, then look
    # for `else` { ... }
    if_pat = re.compile(r"\bif\s*\(\s*_ota_dfu\s*\)\s*\{")
    for m in if_pat.finditer(body):
        # Brace match the if body
        i = m.end() - 1  # at the '{'
        depth = 0
        if_start = i + 1
        while i < len(body):
            if body[i] == "{":
                depth += 1
            elif body[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if_end = i  # at the closing '}'
        if_branch = body[if_start:if_end]
        block_end = if_end + 1

        # Look for `else` immediately after (allowing whitespace)
        j = if_end + 1
        while j < len(body) and body[j] in " \t\r\n":
            j += 1
        if body[j:j+4] == "else":
            j += 4
            while j < len(body) and body[j] in " \t\r\n":
                j += 1
            else_branch = ""
            if j < len(body) and body[j] == "{":
                # block-style else
                depth = 0
                start = j + 1
                k = j
                while k < len(body):
                    if body[k] == "{":
                        depth += 1
                    elif body[k] == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    k += 1
                else_branch = body[start:k]
                block_end = k + 1
            else:
                # statement-style else (no braces) — capture until ';'
                k = j
                while k < len(body) and body[k] != ";":
                    k += 1
                else_branch = body[j:k]
                block_end = k + 1
            results.append((if_branch, else_branch, m.start(), block_end))
        else:
            # No else clause — treat as one-branch
            results.append((if_branch, "", m.start(), block_end))
    return results


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 1

    bl_root = Path(sys.argv[1])
    main_c = bl_root / "src" / "main.c"
    if not main_c.exists():
        print(f"ERROR: cannot find {main_c}", file=sys.stderr)
        return 1

    src = strip_comments(slurp(main_c))

    rng = find_function_body(src, "check_dfu_mode")
    if rng is None:
        print(
            "FAIL: cannot locate check_dfu_mode() in main.c. The source may "
            "have been refactored — this checker needs updating.",
            file=sys.stderr,
        )
        return 2
    body = src[rng[0]:rng[1]]
    print(f"  check_dfu_mode body: {rng[1] - rng[0]} bytes")

    # Sanity: usb_init must appear somewhere in check_dfu_mode.
    if "usb_init" not in body:
        print(
            "FAIL: usb_init() is NOT called anywhere in check_dfu_mode. The\n"
            "  bootloader will NEVER initialize USB on DFU entry. Devices\n"
            "  falling into auto-recovery without a BLE host are unreachable.\n"
            "  This is the 2026-05-15 hat-incident failure mode.",
            file=sys.stderr,
        )
        return 3

    # Find every if (_ota_dfu) / else pair, restricted to the INIT region.
    # The invariant only applies to blocks BEFORE the LAST bootloader_dfu_start()
    # call. Anything after is teardown — usb_init isn't called there.
    # Use the last call because there may be multiple (e.g. a serial-DFU
    # early-return path AND the main DFU entry).
    dfu_start_positions = [m.start() for m in re.finditer(
        r"\bbootloader_dfu_start\s*\(", body
    )]
    if not dfu_start_positions:
        print(
            "FAIL: cannot find call to bootloader_dfu_start() inside\n"
            "  check_dfu_mode. The source may have been refactored — this\n"
            "  checker needs updating.",
            file=sys.stderr,
        )
        return 5
    dfu_start_idx = dfu_start_positions[-1]
    init_region = body[:dfu_start_idx]
    print(f"  init region: {dfu_start_idx} bytes "
          f"(up to LAST of {len(dfu_start_positions)} bootloader_dfu_start calls)")

    branches = find_ota_branches(init_region)
    print(f"  found {len(branches)} `if (_ota_dfu)` block(s) in init region")

    failures = []
    for idx, (if_b, else_b, pos, block_end) in enumerate(branches):
        if_calls = "usb_init" in if_b
        else_calls = "usb_init" in else_b
        # post_init: usb_init called AFTER the entire if/else block, with
        # no `return`/`break`/`goto` in either branch that could skip past.
        post_init = False
        if not (if_calls and else_calls):
            after_block = init_region[block_end:]
            if "usb_init" in after_block:
                # Reject if either branch can exit early
                if_exits = bool(re.search(r"\breturn\b|\bgoto\b", if_b))
                else_exits = bool(re.search(r"\breturn\b|\bgoto\b", else_b))
                if not (if_exits or else_exits):
                    post_init = True

        if if_calls and else_calls:
            print(f"  if-block #{idx}: usb_init in BOTH branches — OK")
        elif post_init:
            print(f"  if-block #{idx}: usb_init AFTER block (both branches fall through) — OK")
        else:
            failures.append((idx, if_calls, else_calls, post_init, if_b[:120], else_b[:120]))

    if failures:
        print(
            "\nFAIL: at least one `if (_ota_dfu)` block does NOT call usb_init()\n"
            "in both branches (and does not fall through to a post-block call).\n"
            "This is the exact pattern that bricked the hat on 2026-05-15.\n",
            file=sys.stderr,
        )
        for idx, ic, ec, pi, ib, eb in failures:
            print(
                f"  block #{idx}: if_branch_calls={ic} else_branch_calls={ec} "
                f"post_block_call={pi}",
                file=sys.stderr,
            )
            print(f"    if  {{ {ib.strip()[:80]} ... }}", file=sys.stderr)
            print(f"    else {{ {eb.strip()[:80]} ... }}", file=sys.stderr)
        return 4

    print("  PASS: every _ota_dfu branch in check_dfu_mode initializes USB")

    # ---- Invariant #2: SD-aware flash writes in BOOTLOADER_SETTINGS_SAVE ----
    # Any nrfx_nvmc_* call inside bootloader_settings_save() (or any function
    # that writes to BOOTLOADER_SETTINGS_ADDRESS) must be SD-state gated.
    # The pattern that fails: direct nrfx_nvmc_* on a path where SD may be
    # enabled, without a `sd_softdevice_is_enabled` or `_sd_is_running()`
    # check around it.
    #
    # Per audit 2026-05-15: the existing code uses `is_ota()` as the gate,
    # which means "DFU was triggered by OTA" — NOT "SoftDevice is enabled".
    # In dual-transport mode these diverge; the UF2 path has SD on but
    # is_ota false, so settings_save takes the direct-NVMC branch and
    # silently no-ops.

    bl_dfu_c = bl_root / "lib" / "sdk11" / "components" / "libraries" / \
               "bootloader_dfu" / "bootloader.c"
    if not bl_dfu_c.exists():
        print(
            f"WARNING: cannot find {bl_dfu_c} — skipping settings-save check",
            file=sys.stderr,
        )
        return 0

    bl_dfu_src = strip_comments(slurp(bl_dfu_c))
    save_rng = find_function_body(bl_dfu_src, "bootloader_settings_save")
    if save_rng is None:
        print(
            "WARNING: cannot locate bootloader_settings_save — skipping check",
            file=sys.stderr,
        )
        return 0
    save_body = bl_dfu_src[save_rng[0]:save_rng[1]]

    has_nvmc = bool(re.search(r"\bnrfx_nvmc_(page_erase|words_write)\b", save_body))
    has_sd_check = bool(re.search(
        r"\bsd_softdevice_is_enabled\b|\b_sd_is_running\b", save_body
    ))
    uses_is_ota = bool(re.search(r"\bis_ota\s*\(", save_body))

    if has_nvmc and not has_sd_check:
        print(
            "\nFAIL: bootloader_settings_save() calls nrfx_nvmc_* directly\n"
            "  without a sd_softdevice_is_enabled / _sd_is_running guard.\n"
            "  Per Nordic spec, direct NVMC access silently no-ops while\n"
            "  the SoftDevice is enabled. In dual-transport DFU mode SD\n"
            "  is enabled even on UF2-triggered DFU (is_ota()==false), so\n"
            "  the existing is_ota() gate is the wrong check.\n"
            "  Reference: docs/BOOTLOADER_PRODUCTION_AUDIT_2026_05_15.md",
            file=sys.stderr,
        )
        if uses_is_ota:
            print(
                "  Detected `is_ota()` in this function — it's NOT a substitute\n"
                "  for `sd_softdevice_is_enabled`. They mean different things\n"
                "  in dual-transport mode.",
                file=sys.stderr,
            )
        return 6

    print("  PASS: bootloader_settings_save SD-state aware")
    return 0


if __name__ == "__main__":
    sys.exit(main())
