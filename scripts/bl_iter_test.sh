#!/bin/bash
# bl_iter_test.sh — closed-loop bootloader iteration test (NO SWD).
#
# Setup required:
#   * Target XIAO connected to dev machine via USB
#   * (Optional) GPIO18 reset wired through swd-flash.local Pi for forcing
#     resets when serial-cmd 'bootloader' isn't reachable
#
# Each iteration:
#   1. Enter DFU mode via serial 'bootloader' command (if in app mode)
#      OR GPIO18 reset + retry if needed
#   2. Drop firmware UF2 to the mounted XIAO-SENSE drive
#   3. Wait for transition to APP mode (pid 8045) within WAIT_TIMEOUT
#   4. Report pass/fail, time, BL version
#
# Run with ITERATIONS=N to characterize hiccup rate. Self-heal calls
# scripts/usb_recovery.sh if kernel disables ports during the run.

set -uo pipefail

PI_HOST=swd-flash.local
FW_UF2="${FW_UF2:-/tmp/blinky-build/blinky-things.uf2}"
ITERATIONS="${ITERATIONS:-10}"
WAIT_TIMEOUT="${WAIT_TIMEOUT:-30}"
USE_GPIO_RESET="${USE_GPIO_RESET:-0}"   # set 1 if serial cmd unavailable
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f "$FW_UF2" ]; then
    echo "ERROR: firmware UF2 not found: $FW_UF2" >&2
    exit 1
fi

declare -i PASS=0 FAIL=0

xiao_pid() {
    for dp in /sys/bus/usb/devices/*; do
        if [ -f "$dp/idVendor" ] && [ "$(cat $dp/idVendor 2>/dev/null)" = "2886" ]; then
            cat "$dp/idProduct" 2>/dev/null
            return
        fi
    done
}

xiao_ttyacm() {
    ls /dev/serial/by-id/usb-Seeed_XIAO_nRF52840_Sense_*-if00 2>/dev/null | head -1
}

dfu_drive() {
    for d in /media/$USER/XIAO-SENSE /media/$USER/XIAO_SENSE; do
        [ -d "$d" ] && echo "$d" && return
    done
    return 1
}

wait_for_pid() {
    local want=$1
    local timeout=$2
    for w in $(seq 1 $timeout); do
        sleep 1
        if [ "$(xiao_pid)" = "$want" ]; then
            echo $w
            return 0
        fi
    done
    return 1
}

# Enter DFU via serial 'bootloader' cmd; fall back to GPIO18 reset
enter_dfu() {
    local tty=$(xiao_ttyacm)
    if [ -n "$tty" ] && [ "$(xiao_pid)" = "8045" ]; then
        python3 -c "
import serial,time
try:
    s = serial.Serial('$tty', 115200, timeout=1)
    s.write(b'bootloader\n'); time.sleep(0.3); s.close()
except: pass
" 2>/dev/null
    elif [ "$USE_GPIO_RESET" = "1" ]; then
        # Hard reset via GPIO18 (XIAO reset pin). Toggle to enter dbl-reset.
        ssh -o ConnectTimeout=3 $PI_HOST "sudo gpioset -c gpiochip0 -t 200ms,0 18=0" 2>/dev/null
    fi

    # Wait for BL DFU mode
    if w=$(wait_for_pid 0045 12); then
        return 0
    fi
    return 1
}

# Self-heal USB bus if kernel damaged it
self_heal() {
    if ! "$SCRIPT_DIR/usb_recovery.sh" diagnose >/dev/null 2>&1; then
        echo "  [heal] USB bus damaged — attempting recovery..."
        "$SCRIPT_DIR/usb_recovery.sh" heal 2>&1 | sed 's/^/    /'
        sleep 3
    fi
}

echo "===================================================================="
echo "  bl_iter_test  $(date '+%Y-%m-%d %H:%M:%S')"
echo "===================================================================="
echo "  FW UF2 : $FW_UF2 ($(stat -c %s $FW_UF2) bytes)"
echo "  Iters  : $ITERATIONS"
echo "  Timeout: $WAIT_TIMEOUT s per iteration"
echo

# Pre-flight: check USB bus is healthy
"$SCRIPT_DIR/usb_recovery.sh" diagnose | sed 's/^/  /'
echo

start_time=$(date +%s)
times=()

for i in $(seq 1 $ITERATIONS); do
    iter_start=$(date +%s)
    echo "--- iter $i  $(date +%H:%M:%S) ---"

    # Self-heal check
    self_heal

    # Get into BL DFU mode
    current_pid=$(xiao_pid)
    if [ "$current_pid" != "0045" ]; then
        if ! enter_dfu; then
            echo "  FAIL: couldn't enter DFU mode (current pid=$(xiao_pid))"
            FAIL=$((FAIL+1))
            continue
        fi
    fi

    # Wait for drive to mount
    for w in $(seq 1 10); do
        sleep 1
        DRV=$(dfu_drive 2>/dev/null) && break
    done
    if [ -z "${DRV:-}" ]; then
        echo "  FAIL: BL up but UF2 drive didn't mount"
        FAIL=$((FAIL+1))
        continue
    fi
    blver=$(head -1 "$DRV/INFO_UF2.TXT" 2>/dev/null | awk '{print $3}')

    # Drop firmware
    drop_t=$(date +%s.%N)
    cp "$FW_UF2" "$DRV/"
    sync
    drop_done=$(date +%s.%N)

    # Wait for APP transition
    if w=$(wait_for_pid 8045 $WAIT_TIMEOUT); then
        done_t=$(date +%s.%N)
        dt=$(echo "$done_t - $drop_t" | bc -l)
        times+=("$dt")
        printf "  PASS %.1fs — bl=%s\n" "$dt" "$blver"
        PASS=$((PASS+1))
        # Stabilize app before next iteration
        sleep 2
    else
        echo "  FAIL after ${WAIT_TIMEOUT}s — pid=$(xiao_pid) (HICCUP — stuck in BL)"
        FAIL=$((FAIL+1))
        # Try to recover via re-drop (the classic operator workaround)
        echo "  [recover] re-dropping UF2..."
        DRV2=$(dfu_drive 2>/dev/null)
        if [ -n "$DRV2" ]; then
            cp "$FW_UF2" "$DRV2/"
            sync
            wait_for_pid 8045 20 >/dev/null && echo "  [recover] re-drop succeeded" || \
                echo "  [recover] FAILED — needs manual intervention"
        fi
    fi
done

total_time=$(($(date +%s) - start_time))

echo
echo "===================================================================="
echo "  Summary: $PASS pass / $FAIL fail of $ITERATIONS in ${total_time}s"
echo "===================================================================="
if [ "${#times[@]}" -gt 0 ]; then
    avg=$(echo "${times[@]}" | tr ' ' '\n' | awk '{s+=$1} END {printf "%.2f", s/NR}')
    min=$(echo "${times[@]}" | tr ' ' '\n' | sort -n | head -1)
    max=$(echo "${times[@]}" | tr ' ' '\n' | sort -n | tail -1)
    printf "  PASS times: avg=%ss  min=%ss  max=%ss\n" "$avg" "$min" "$max"
fi
echo

exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
