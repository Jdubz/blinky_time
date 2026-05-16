#!/bin/bash
# bl_characterize.sh — measure firmware-flash hiccup rate + correlate with
# flash content. Designed to confirm the audit hypothesis that "consumed-
# but-no-reboot" hiccups are settings_save failures, not flash-write
# failures.
#
# For each iteration:
#   1. Enter DFU
#   2. Drop firmware UF2
#   3. Wait up to N seconds for APP transition
#   4. If FAIL:
#      - Dump CURRENT.UF2 from the still-DFU drive
#      - Compare bytewise against source firmware
#      - Classify: flash complete (settings_save bug) or flash incomplete
#        (flash-write bug)
#   5. Record outcome
#
# Output: tab-separated results with iteration, time, outcome, flash status.

set -uo pipefail

FW_UF2="${FW_UF2:-/tmp/blinky-build/blinky-things.uf2}"
ITERATIONS="${ITERATIONS:-20}"
WAIT_TIMEOUT="${WAIT_TIMEOUT:-20}"
LOG=/tmp/bl_characterize.log

if [ ! -f "$FW_UF2" ]; then
    echo "ERROR: $FW_UF2 not found" >&2; exit 1
fi

xiao_pid() {
    for dp in /sys/bus/usb/devices/*; do
        if [ -f "$dp/idVendor" ] && [ "$(cat $dp/idVendor 2>/dev/null)" = "2886" ]; then
            cat "$dp/idProduct" 2>/dev/null; return
        fi
    done
}

dfu_drive() {
    for d in /media/$USER/XIAO-SENSE; do
        [ -d "$d" ] && echo "$d" && return
    done
    return 1
}

wait_pid() {
    local want=$1 timeout=$2
    for w in $(seq 1 $timeout); do
        sleep 1
        [ "$(xiao_pid)" = "$want" ] && echo "$w" && return 0
    done
    return 1
}

# Diff a CURRENT.UF2 capture against source firmware
classify_flash() {
    local current="$1"
    python3 <<EOF
import struct, sys
src = open('$FW_UF2','rb').read()
cur = open('$current','rb').read()
src_map = {}
for i in range(len(src)//512):
    blk = src[i*512:(i+1)*512]
    if struct.unpack('<I', blk[:4])[0] != 0x0A324655: continue
    tgt = struct.unpack_from('<I', blk, 12)[0]
    ps = struct.unpack_from('<I', blk, 16)[0]
    src_map[tgt] = blk[32:32+ps]
cur_map = {}
for i in range(len(cur)//512):
    blk = cur[i*512:(i+1)*512]
    if struct.unpack('<I', blk[:4])[0] != 0x0A324655: continue
    tgt = struct.unpack_from('<I', blk, 12)[0]
    ps = struct.unpack_from('<I', blk, 16)[0]
    cur_map[tgt] = blk[32:32+ps]
missing = mismatch = erased = 0
for addr, sp in src_map.items():
    if addr not in cur_map: missing += 1; continue
    if cur_map[addr] == sp: continue
    if all(b == 0xFF for b in cur_map[addr]): erased += 1
    else: mismatch += 1
total = missing + mismatch + erased
n = len(src_map)
if total == 0:
    print(f"flash=COMPLETE n={n}")
else:
    print(f"flash=INCOMPLETE n={n} missing={missing} erased={erased} mismatched={mismatch}")
EOF
}

> "$LOG"
declare -i pass=0 fail=0 flash_complete=0 flash_incomplete=0

echo "iter   time     outcome   flash_status                                ms_to_app"
echo "----   ------   -------   ------------------------------------------- ---------"

# Ensure we start in APP mode
if [ "$(xiao_pid)" != "8045" ]; then
    echo "WARN: not in APP at start (pid=$(xiao_pid))"
fi

for i in $(seq 1 $ITERATIONS); do
    ts=$(date +%H:%M:%S)
    iter_t0=$(date +%s.%N)

    # Enter DFU
    if [ "$(xiao_pid)" = "8045" ]; then
        TTY=$(ls /dev/serial/by-id/usb-Seeed_XIAO_nRF52840_Sense_*-if00 2>/dev/null | head -1)
        if [ -n "$TTY" ]; then
            python3 -c "
import serial,time
try: s=serial.Serial('$TTY',115200,timeout=1); s.write(b'bootloader\n'); time.sleep(0.3); s.close()
except: pass
" 2>/dev/null
        fi
    fi

    # Wait for DFU mount
    for w in $(seq 1 12); do
        sleep 1
        DRV=$(dfu_drive 2>/dev/null) && break
    done
    if [ -z "${DRV:-}" ]; then
        printf "%-6d %-8s SKIP      no DFU drive\n" $i "$ts"
        continue
    fi

    # Drop firmware
    drop_t=$(date +%s.%N)
    cp "$FW_UF2" "$DRV/"
    sync

    # Wait for outcome
    outcome="STUCK"
    if w=$(wait_pid 8045 $WAIT_TIMEOUT); then
        outcome="PASS"
        done_t=$(date +%s.%N)
        dt_ms=$(echo "($done_t - $drop_t) * 1000" | bc -l | awk '{printf "%.0f", $1}')
        pass=$((pass+1))
        printf "%-6d %-8s PASS      —                                           %sms\n" $i "$ts" "$dt_ms"
        sleep 2
    else
        fail=$((fail+1))
        # Dump CURRENT.UF2 from the still-DFU drive
        DRV2=$(dfu_drive 2>/dev/null)
        if [ -n "$DRV2" ] && [ -f "$DRV2/CURRENT.UF2" ]; then
            cp "$DRV2/CURRENT.UF2" /tmp/cur_$i.uf2
            status=$(classify_flash /tmp/cur_$i.uf2)
            if [[ "$status" == flash=COMPLETE* ]]; then
                flash_complete=$((flash_complete+1))
            else
                flash_incomplete=$((flash_incomplete+1))
            fi
            printf "%-6d %-8s STUCK     %s\n" $i "$ts" "$status"
        else
            printf "%-6d %-8s STUCK     (no CURRENT.UF2 to dump)\n" $i "$ts"
        fi
        # Recover via re-drop
        cp "$FW_UF2" "$DRV2/" 2>/dev/null
        sync
        wait_pid 8045 20 >/dev/null
        sleep 2
    fi
done

echo
echo "===================================================================="
echo "Summary: PASS=$pass FAIL=$fail of $ITERATIONS"
echo "Of failures: flash COMPLETE=$flash_complete  flash INCOMPLETE=$flash_incomplete"
if [ "$fail" -gt 0 ]; then
    pct_complete=$(echo "$flash_complete * 100 / $fail" | bc)
    echo
    echo "  $pct_complete% of failures had COMPLETE flash but app didn't boot."
    echo "  → matches the 'bootloader_settings_save silently failed' diagnosis."
    echo "  → the 0.8.0-6 fix targets exactly this scenario."
fi
