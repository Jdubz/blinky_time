#!/bin/bash
# deploy-bootloader.sh — safe deploy of an nRF52840 bootloader update.
#
# Wraps a bootloader UF2 flash with three safety layers that did not exist
# during the 2026-05-15 hat incident:
#
#   1. Source-level static verification (scripts/verify_bootloader.py).
#      Refuses to proceed if check_dfu_mode() has any `if (_ota_dfu)` branch
#      that skips usb_init() — the exact pattern that bricked the hat.
#
#   2. Last-known-good backup. Before flashing the new BL, copies the
#      currently-running BL's INFO_UF2.TXT version to a session log.
#
#   3. Post-flash verification. Reads INFO_UF2.TXT after the device reboots
#      and confirms the version string matches what was intended.
#
# This script does NOT bypass deploy.sh for firmware updates — it's for the
# BOOTLOADER ONLY. Firmware deploys still go through deploy.sh.
#
# Usage:
#   ./scripts/deploy-bootloader.sh                                 # flash staged BL to connected device
#   ./scripts/deploy-bootloader.sh --target /dev/ttyACM0           # specify port
#   ./scripts/deploy-bootloader.sh --bootloader path/to/bl.uf2     # specify BL
#   ./scripts/deploy-bootloader.sh --skip-verify                   # bypass source check (BANNED for fleet)
#   ./scripts/deploy-bootloader.sh --allow-experimental            # flash an unverified BL ONCE
#
# Without --allow-experimental, an unverified BL is REFUSED. This is the
# primary defense against the failure mode that bricked the hat.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_BL_UF2="$REPO/bootloader/update-bootloader-qspi-ota-default.uf2"

# BL_REPO defaults to the bootloader/src submodule (fork of
# adafruit/Adafruit_nRF52_Bootloader on the blinky-local-patches branch,
# pinned at the commit that built the staged .uf2). Override with env
# BL_REPO=/path/... for other layouts. We fail loud (rather than silently
# noop'ing verify_bootloader.py) if the directory isn't populated —
# PR #140 review: the previous hardcoded $HOME path was author-machine-
# specific and would silently do nothing on CI / other operators' boxes.
BL_REPO="${BL_REPO:-$REPO/bootloader/src}"
if [ ! -d "$BL_REPO" ] || [ ! -d "$BL_REPO/src" ]; then
    cat >&2 <<EOF
ERROR: BL_REPO is missing or not populated: $BL_REPO

The bootloader-source verifier (scripts/verify_bootloader.py) needs the
Adafruit_nRF52_Bootloader source tree to run its source-level invariant
checks. The blinky_time repo ships this source as a submodule at
bootloader/src; if the directory is empty you probably need to init it:

      git -C "$REPO" submodule update --init --recursive bootloader/src

Or, if you maintain the source tree somewhere else, point BL_REPO at it:

      BL_REPO=/your/path ./scripts/deploy-bootloader.sh ...

EOF
    exit 2
fi

PORT=""
BL_UF2="$DEFAULT_BL_UF2"
SKIP_VERIFY=0
ALLOW_EXPERIMENTAL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --target) PORT="$2"; shift 2 ;;
        --bootloader) BL_UF2="$2"; shift 2 ;;
        --skip-verify) SKIP_VERIFY=1; shift ;;
        --allow-experimental) ALLOW_EXPERIMENTAL=1; shift ;;
        --help|-h)
            sed -n '2,30p' "$0"
            exit 0
            ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

if [ ! -f "$BL_UF2" ]; then
    echo "ERROR: bootloader UF2 not found: $BL_UF2" >&2
    exit 1
fi

echo "===================================================================="
echo "  deploy-bootloader.sh  $(date '+%Y-%m-%d %H:%M:%S')"
echo "===================================================================="
echo "  Bootloader UF2 : $BL_UF2"
echo "  BL source repo : $BL_REPO"

# --- Safety 1: source-level verify ---
if [ "$SKIP_VERIFY" -ne 1 ]; then
    echo
    echo "[1/3] source-level invariant check..."
    if ! python3 "$REPO/scripts/verify_bootloader.py" "$BL_REPO"; then
        echo
        echo "BLOCKED: bootloader source FAILED the recoverability invariant." >&2
        echo "The compiled BL would brick devices that fall into auto-recovery" >&2
        echo "without a BLE host nearby (the 2026-05-15 hat incident pattern)." >&2
        echo >&2
        if [ "$ALLOW_EXPERIMENTAL" -eq 1 ]; then
            echo "  --allow-experimental was passed; proceeding anyway." >&2
            echo "  BACK UP / KEEP SWD ACCESS HANDY." >&2
        else
            echo "  Pass --allow-experimental to bypass (you MUST have SWD access)." >&2
            exit 2
        fi
    fi
else
    echo
    echo "[1/3] SKIPPED (--skip-verify) — refusing to proceed without explicit"
    echo "       --allow-experimental as well."
    if [ "$ALLOW_EXPERIMENTAL" -ne 1 ]; then
        echo "BLOCKED: --skip-verify requires --allow-experimental." >&2
        exit 2
    fi
fi

# --- Locate UF2 drive ---
echo
echo "[2/3] locating XIAO UF2 drive..."
DFU_DRIVE=""
WE_MOUNTED=""   # set to a temp mountpoint if WE mounted it (so we unmount after)

# Desktop case: udisks auto-mounts the DFU volume under /media. Detect by
# the presence of INFO_UF2.TXT, NOT bare directory existence — udisks leaves
# stale empty mountpoints behind after a DFU session unmounts, and `[ -d ]`
# false-positives on them (the script would then cp into a non-drive).
find_automount() {
    local d
    for d in /media/$USER/XIAO-SENSE /media/$USER/XIAO_SENSE /run/media/$USER/XIAO-SENSE; do
        [ -f "$d/INFO_UF2.TXT" ] && { echo "$d"; return 0; }
    done
    return 1
}

# Headless case (e.g. blinkyhost): no desktop session, so udisks never
# auto-mounts. Find the DFU block device by label and mount it ourselves
# with the invoking uid (mirrors tools/uf2_upload.py). Echoes the mountpoint
# on success; the caller records it in WE_MOUNTED so it gets unmounted later.
mount_by_label() {
    local bylabel="/dev/disk/by-label/XIAO-SENSE" dev mp
    [ -e "$bylabel" ] || return 1
    dev="$(readlink -f "$bylabel")"
    mp="$(mktemp -d)"
    if sudo mount -t vfat -o uid="$(id -u)",gid="$(id -g)" "$dev" "$mp" 2>/dev/null \
        && [ -f "$mp/INFO_UF2.TXT" ]; then
        echo "$mp"; return 0
    fi
    sudo umount "$mp" 2>/dev/null || true
    rmdir "$mp" 2>/dev/null || true
    return 1
}

# Unmount + remove a temp mountpoint that WE created via mount_by_label.
# Safe to call with an empty/unset arg (no-op), so callers can pass a maybe-unset
# var without guarding. The device is usually rebooting when we clean up, so the
# umount races the disappearing volume — tolerate failure and just drop the dir.
# Centralised here so the three call sites (initial flash, post-DFU poll, verify)
# can't drift out of sync.
cleanup_mount() {
    local mp="${1:-}"
    [ -n "$mp" ] || return 0
    sudo umount "$mp" 2>/dev/null || true
    rmdir "$mp" 2>/dev/null || true
}

# True (0) when $1 is a non-empty BL version string that differs from PRE_BL.
# If PRE_BL could not be read (empty — INFO_UF2.TXT was missing pre-flash),
# any non-empty POST_BL is the best confirmation we can offer. A POST_BL equal
# to a non-empty PRE_BL means the UF2 copy did NOT take (stale mount, full fs,
# perms) — that is NOT success.
bl_changed() {
    local post="${1:-}"
    [ -n "$post" ] || return 1
    [ -z "$PRE_BL" ] && return 0
    [ "$post" != "$PRE_BL" ]
}

DFU_DRIVE="$(find_automount || true)"
if [ -z "$DFU_DRIVE" ]; then
    # Maybe already in DFU but not auto-mounted (headless).
    if DFU_DRIVE="$(mount_by_label)"; then WE_MOUNTED="$DFU_DRIVE"; else DFU_DRIVE=""; fi
fi

if [ -z "$DFU_DRIVE" ]; then
    echo "  no XIAO-SENSE drive mounted; trying to enter DFU via serial..."
    PORT="${PORT:-/dev/ttyACM0}"
    if [ ! -e "$PORT" ]; then
        echo "ERROR: no serial port at $PORT and no DFU drive mounted." >&2
        echo "       Plug device in, OR press reset to enter BL manually." >&2
        exit 3
    fi
    python3 -c "
import serial, time
s = serial.Serial('$PORT', 115200, timeout=1)
s.write(b'bootloader\n')
time.sleep(0.3)
s.close()
" 2>/dev/null || true
    for w in $(seq 1 15); do
        sleep 1
        DFU_DRIVE="$(find_automount || true)"
        [ -n "$DFU_DRIVE" ] && break
        if DFU_DRIVE="$(mount_by_label)"; then WE_MOUNTED="$DFU_DRIVE"; break; else DFU_DRIVE=""; fi
    done
    if [ -z "$DFU_DRIVE" ]; then
        echo "ERROR: device didn't enter DFU mode within 15s." >&2
        exit 3
    fi
fi
echo "  DFU drive: $DFU_DRIVE${WE_MOUNTED:+ (explicit mount)}"

# --- Safety 2: log the current BL version ---
PRE_BL=""
if [ -f "$DFU_DRIVE/INFO_UF2.TXT" ]; then
    PRE_BL="$(head -1 "$DFU_DRIVE/INFO_UF2.TXT")"
    echo "  pre-flash BL: $PRE_BL"
else
    echo "  pre-flash BL: (INFO_UF2.TXT missing — unusual)"
fi

# --- Flash ---
echo
echo "[3/3] flashing $(basename "$BL_UF2")..."
# Pre-write sanity, BEFORE we tolerate the expected mid-write disconnect:
# catch the GENUINE copy failures (missing/unreadable source, stale empty
# mountpoint) loudly. Without this, the `cp ... || true` below would mask
# them as success. INFO_UF2.TXT presence confirms $DFU_DRIVE is a LIVE UF2
# volume rather than a stale automount dir; we deliberately do NOT write a
# probe file to the UF2 MSC (its fake FAT can mishandle non-.uf2 writes) —
# the post-flash BL-version-change assertion below is what catches the
# remaining failure modes (full filesystem / permissions / copy silently
# dropped) by confirming the BL actually changed.
[ -r "$BL_UF2" ] || { echo "ERROR: BL UF2 not readable: $BL_UF2" >&2; exit 5; }
[ -f "$DFU_DRIVE/INFO_UF2.TXT" ] || {
    echo "ERROR: $DFU_DRIVE has no INFO_UF2.TXT — not a live UF2 drive" >&2
    echo "       (stale mountpoint?). Refusing to cp into a non-drive." >&2
    exit 5
}
# The BL applies the UF2 and reboots, which yanks the USB volume — so cp /
# sync may report an error as the device disappears mid-write. That's the
# expected success path, not a failure, hence the `|| true`. (Real failures
# are caught by the pre-write checks above + the version-change gate below.)
cp -v "$BL_UF2" "$DFU_DRIVE/" || true
sync || true
# If WE explicitly mounted the volume (headless path), unmount it now.
cleanup_mount "$WE_MOUNTED"

# --- Wait for re-enumeration ---
echo "  waiting for device to reboot..."
sleep 5  # BL self-flash + reboot
# Wait for APP mode pid 8045 OR new DFU mount
PID=""
for w in $(seq 1 30); do
    sleep 1
    for dp in /sys/bus/usb/devices/*; do
        if [ -f "$dp/idVendor" ] && [ "$(cat $dp/idVendor 2>/dev/null)" = "2886" ]; then
            PID=$(cat $dp/idProduct 2>/dev/null)
        fi
    done
    [ "$PID" = "8045" ] && break
    # New DFU mount means BL is up
    if [ -f "$DFU_DRIVE/INFO_UF2.TXT" ]; then
        POST_BL="$(head -1 "$DFU_DRIVE/INFO_UF2.TXT" 2>/dev/null || true)"
        if [ "$POST_BL" != "$PRE_BL" ] && [ -n "$POST_BL" ]; then
            echo "  new BL: $POST_BL"
            break
        fi
    fi
done

# --- Safety 3: post-flash verification ---
echo
echo "Result:"
if [ "$PID" = "8045" ]; then
    echo "  device returned to APP mode (pid=8045) — app booted on a working BL"
    # Returning to APP proves *a* working BL is present, but NOT that OURS
    # landed — the app could have booted on the OLD BL if the copy silently
    # failed. Re-enter BL briefly to read INFO_UF2.TXT and CONFIRM the version
    # actually changed before reporting success.
    PORT="${PORT:-/dev/ttyACM0}"
    if [ -e "$PORT" ]; then
        python3 -c "
import serial, time
s = serial.Serial('$PORT', 115200, timeout=1)
s.write(b'bootloader\n')
time.sleep(0.3)
s.close()
" 2>/dev/null || true
        VERIFY_DRIVE=""; VERIFY_MOUNTED=""
        for w in $(seq 1 10); do
            sleep 1
            VERIFY_DRIVE="$(find_automount || true)"
            [ -n "$VERIFY_DRIVE" ] && break
            if VERIFY_DRIVE="$(mount_by_label)"; then VERIFY_MOUNTED="$VERIFY_DRIVE"; break; else VERIFY_DRIVE=""; fi
        done
        if [ -n "$VERIFY_DRIVE" ] && [ -f "$VERIFY_DRIVE/INFO_UF2.TXT" ]; then
            POST_BL="$(head -1 "$VERIFY_DRIVE/INFO_UF2.TXT")"
            echo "  post-flash BL: $POST_BL"
            # Leave the device in the bootloader's DFU after this read — the
            # caller flashed firmware separately. (We don't auto-reboot to app
            # here to avoid masking a genuinely-stuck BL.)
            cleanup_mount "$VERIFY_MOUNTED"
            if bl_changed "$POST_BL"; then
                echo "  BL version confirmed changed (pre='$PRE_BL' -> post='$POST_BL')"
            else
                echo "ERROR: app booted but BL version did NOT change" >&2
                echo "       (pre='$PRE_BL' post='$POST_BL'). The UF2 copy did not" >&2
                echo "       take (stale mount / full fs / perms). BL was NOT updated." >&2
                exit 4
            fi
        else
            cleanup_mount "$VERIFY_MOUNTED"
            echo "WARNING: app booted (pid=8045) but could not re-read INFO_UF2.TXT" >&2
            echo "         to confirm the new BL version. Verify manually before sealing." >&2
        fi
    else
        echo "WARNING: no serial port at $PORT — cannot re-read the BL version to" >&2
        echo "         confirm the update. App booted, but verify manually before sealing." >&2
    fi
elif bl_changed "${POST_BL:-}"; then
    echo "  device in BL DFU; BL version confirmed changed (pre='$PRE_BL' -> post='$POST_BL')"
elif [ -n "${POST_BL:-}" ]; then
    # We read a BL version but it equals the pre-flash one — the copy didn't take.
    echo "ERROR: device in BL DFU but BL version did NOT change (pre='$PRE_BL'" >&2
    echo "       post='$POST_BL'). The UF2 copy did not take (stale mount / full" >&2
    echo "       fs / perms). BL was NOT updated." >&2
    exit 4
else
    echo "WARNING: device did not return to APP mode after BL flash. State:" >&2
    for dp in /sys/bus/usb/devices/*; do
        if [ -f "$dp/idVendor" ] && [ "$(cat $dp/idVendor 2>/dev/null)" = "2886" ]; then
            echo "  pid=$(cat $dp/idProduct)" >&2
        fi
    done
    exit 4
fi

echo
echo "Done."
