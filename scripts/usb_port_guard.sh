#!/bin/bash
# usb_port_guard.sh — detect approaching the Linux kernel's USB-port disable
# threshold and back off before it fires.
#
# Background: after ~5 enumeration failures in a short window (typically
# ETIMEDOUT during device descriptor read), Linux's hub_port_init() retry
# logic gives up and writes `1` to /sys/.../usb1-port<N>/disable. That
# leaves the port unusable for the rest of the boot unless we
# unbind/rebind xhci_hcd (which disconnects every other USB device on the
# bus) or unplug+replug (which may not even clear the soft-disable in
# some kernels).
#
# The 2026-05-15 BL-iteration session hit this — 24 -110 errors during
# rapid BL flash cycles, port 1-12 was disabled, the test device became
# unreachable until physical cable move.
#
# Usage:
#   ./scripts/usb_port_guard.sh status
#       Show current count of -110 errors per port in the last N seconds.
#
#   ./scripts/usb_port_guard.sh wait_safe [port]
#       Block until the -110 rate falls below the danger threshold.
#       Use this between rapid BL flashes.
#
#   ./scripts/usb_port_guard.sh check_port <port>
#       Exit 0 if port is healthy (disable=0), non-zero if disabled.
#
# Integrate into deploy-bootloader.sh / bl_iter_test.sh: call
# `usb_port_guard.sh wait_safe` after each flash before the next.

set -uo pipefail

WINDOW_SECONDS="${WINDOW_SECONDS:-30}"
DANGER_COUNT="${DANGER_COUNT:-3}"   # back off when we're within 2 of the kernel's typical limit of 5

usage() {
    sed -n '2,28p' "$0"
    exit 1
}

count_recent_errors() {
    local port=$1
    journalctl -k --since "${WINDOW_SECONDS} seconds ago" 2>/dev/null | \
        grep -cE "usb ${port}.*error -110" || true
}

list_xiao_ports() {
    for dp in /sys/bus/usb/devices/*; do
        if [ -f "$dp/idVendor" ] && [ "$(cat $dp/idVendor 2>/dev/null)" = "2886" ]; then
            basename "$dp"
        fi
    done
}

cmd_status() {
    echo "=== USB port guard status ==="
    echo "  window: ${WINDOW_SECONDS}s; danger threshold: ${DANGER_COUNT} -110s"
    echo
    # Show any port with -110 errors in the window
    journalctl -k --since "${WINDOW_SECONDS} seconds ago" 2>/dev/null | \
        grep -oE "usb [0-9]+-[0-9]+" | sort -u | while read prefix port; do
            n=$(count_recent_errors "$port")
            if [ "$n" -gt 0 ]; then
                echo "  $port: $n -110 errors in last ${WINDOW_SECONDS}s$([ "$n" -ge "$DANGER_COUNT" ] && echo ' ⚠ DANGER')"
            fi
        done
    # Show port disable states
    for p in /sys/bus/usb/devices/*/usb*-port*/disable; do
        d=$(cat "$p" 2>/dev/null)
        if [ "$d" = "1" ]; then
            port=$(basename $(dirname "$p"))
            echo "  $port: DISABLED by kernel"
        fi
    done
}

cmd_wait_safe() {
    local port="${1:-}"
    if [ -z "$port" ]; then
        # Auto-detect XIAO's current port
        port=$(list_xiao_ports | head -1)
        if [ -z "$port" ]; then
            echo "  no XIAO on USB; nothing to wait for"
            return 0
        fi
    fi

    echo "  guarding port $port (window=${WINDOW_SECONDS}s, threshold=${DANGER_COUNT})"
    local waited=0
    while true; do
        local n=$(count_recent_errors "$port")
        if [ "$n" -lt "$DANGER_COUNT" ]; then
            echo "  port $port has $n -110 errors in window — safe to proceed"
            return 0
        fi
        if [ $waited -ge 60 ]; then
            echo "  WARN: still $n errors in window after 60s; proceeding anyway"
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
        echo "  waiting... ($n -110 errors in last ${WINDOW_SECONDS}s; +${waited}s)"
    done
}

cmd_check_port() {
    local port="$1"
    local disable_file="/sys/bus/usb/devices/1-0:1.0/${port}/disable"
    if [ ! -f "$disable_file" ]; then
        echo "  port $port: not present in sysfs"
        return 1
    fi
    local d=$(cat "$disable_file")
    if [ "$d" = "1" ]; then
        echo "  port $port: DISABLED — needs cable move or xhci rebind"
        return 1
    fi
    echo "  port $port: healthy (disable=0)"
    return 0
}

case "${1:-}" in
    status)     cmd_status ;;
    wait_safe)  cmd_wait_safe "${2:-}" ;;
    check_port) [ -z "${2:-}" ] && usage; cmd_check_port "$2" ;;
    *)          usage ;;
esac
