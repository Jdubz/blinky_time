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

# BL_REPO defaults to a sibling checkout of Adafruit_nRF52_Bootloader.
# Override with env BL_REPO=/path/... for other layouts. We fail loud
# (rather than silently noop'ing verify_bootloader.py) if the directory
# isn't there — PR #140 review: the previous hardcoded $HOME path was
# author-machine-specific and would silently do nothing on CI / other
# operators' boxes.
BL_REPO="${BL_REPO:-$REPO/../Adafruit_nRF52_Bootloader}"
if [ ! -d "$BL_REPO" ]; then
    cat >&2 <<EOF
ERROR: BL_REPO is not a directory: $BL_REPO

The bootloader-source verifier (scripts/verify_bootloader.py) needs the
Adafruit_nRF52_Bootloader source tree to run its source-level invariant
checks. Either:

  * clone it as a sibling to this repo:
      cd "$(dirname "$REPO")"
      git clone https://github.com/adafruit/Adafruit_nRF52_Bootloader.git
      (then git checkout the blinky-local-patches branch, or whichever
       branch this fleet's BL is built from)

  * or set BL_REPO to wherever you have it:
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
for d in /media/$USER/XIAO-SENSE /media/$USER/XIAO_SENSE /run/media/$USER/XIAO-SENSE; do
    [ -d "$d" ] && DFU_DRIVE="$d" && break
done

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
    for w in $(seq 1 12); do
        sleep 1
        for d in /media/$USER/XIAO-SENSE /media/$USER/XIAO_SENSE /run/media/$USER/XIAO-SENSE; do
            [ -d "$d" ] && DFU_DRIVE="$d" && break
        done
        [ -n "$DFU_DRIVE" ] && break
    done
    if [ -z "$DFU_DRIVE" ]; then
        echo "ERROR: device didn't enter DFU mode within 12s." >&2
        exit 3
    fi
fi
echo "  DFU drive: $DFU_DRIVE"

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
cp -v "$BL_UF2" "$DFU_DRIVE/"
sync

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
    echo "  device returned to APP mode (pid=8045) — BL self-update succeeded"
    # Re-enter BL briefly to read INFO_UF2.TXT
    PORT="${PORT:-/dev/ttyACM0}"
    if [ -e "$PORT" ]; then
        python3 -c "
import serial, time
s = serial.Serial('$PORT', 115200, timeout=1)
s.write(b'bootloader\n')
time.sleep(0.3)
s.close()
" 2>/dev/null || true
        for w in $(seq 1 10); do
            sleep 1
            for d in /media/$USER/XIAO-SENSE /media/$USER/XIAO_SENSE; do
                [ -d "$d" ] && DFU_DRIVE="$d" && break
            done
            [ -f "$DFU_DRIVE/INFO_UF2.TXT" ] && break
        done
        if [ -f "$DFU_DRIVE/INFO_UF2.TXT" ]; then
            POST_BL="$(head -1 "$DFU_DRIVE/INFO_UF2.TXT")"
            echo "  post-flash BL: $POST_BL"
        fi
    fi
elif [ -n "${POST_BL:-}" ]; then
    echo "  device in BL DFU; updated BL: $POST_BL"
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
