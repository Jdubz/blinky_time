#!/bin/bash
# fleet_flash.sh — Compile once, flash all connected devices, verify all
#
# Safe, sequential firmware deployment for all XIAO nRF52840 devices.
# Flashes one device at a time (no parallel races), verifies each before
# moving to the next, and provides a clear summary.
#
# Usage:
#   ./tools/fleet_flash.sh                  # Flash all connected devices
#   ./tools/fleet_flash.sh --nn             # Flash with NN beat activation
#   ./tools/fleet_flash.sh --recover        # Recovery mode: power-cycle + flash
#   ./tools/fleet_flash.sh --ports ACM0 ACM1  # Flash specific devices
#   ./tools/fleet_flash.sh --uf2 /path.uf2 # Use pre-built UF2

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="/tmp/blinky-build"
ARDUINO_CLI="${ARDUINO_CLI:-arduino-cli}"
FQBN="Seeeduino:nrf52:xiaonRF52840Sense"
UF2_TOOL="$SCRIPT_DIR/uf2_upload.py"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Parse arguments
NN_FLAG=""
RECOVER_MODE=false
SPECIFIC_PORTS=()
PRE_BUILT_UF2=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --nn) NN_FLAG="-DENABLE_NN_BEAT_ACTIVATION"; shift ;;
        --recover) RECOVER_MODE=true; shift ;;
        --uf2) PRE_BUILT_UF2="$2"; shift 2 ;;
        --ports) shift; while [[ $# -gt 0 && ! "$1" =~ ^-- ]]; do
            SPECIFIC_PORTS+=("/dev/tty$1"); shift; done ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo -e "${CYAN}[fleet]${NC} $*"; }
ok() { echo -e "${GREEN}[OK]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

# ============================================================
# Phase 1: Discover devices
# ============================================================
log "Discovering XIAO nRF52840 devices..."
if [[ ${#SPECIFIC_PORTS[@]} -gt 0 ]]; then
    PORTS=("${SPECIFIC_PORTS[@]}")
else
    PORTS=()
    for p in /dev/ttyACM*; do
        [[ -e "$p" ]] && PORTS+=("$p")
    done
fi

if [[ ${#PORTS[@]} -eq 0 ]]; then
    fail "No devices found"
    exit 1
fi

log "Found ${#PORTS[@]} device(s): ${PORTS[*]}"
echo ""

# ============================================================
# Phase 2: Recovery mode — power-cycle unresponsive devices
# ============================================================
if $RECOVER_MODE; then
    log "Recovery mode: power-cycling all devices..."
    # Find USB hub topology
    HUB_PATH=""
    for p in "${PORTS[@]}"; do
        port_name=$(basename "$p")
        # Try to find the device in uhubctl output
        hub_info=$(sudo uhubctl 2>/dev/null | grep -B5 "$(udevadm info -q property "$p" 2>/dev/null | grep -oP 'ID_SERIAL_SHORT=\K.*')" | head -1 || true)
        if [[ -n "$hub_info" ]]; then
            log "  Power-cycling $p..."
            # Extract hub and port from uhubctl output
            hub=$(echo "$hub_info" | grep -oP 'hub \K[0-9.-]+' || true)
            port_num=$(echo "$hub_info" | grep -oP 'Port \K[0-9]+' || true)
            if [[ -n "$hub" && -n "$port_num" ]]; then
                sudo uhubctl -l "$hub" -p "$port_num" -a cycle -d 3 2>/dev/null || true
            fi
        fi
    done
    log "Waiting 5s for devices to re-enumerate..."
    sleep 5

    # Re-discover
    PORTS=()
    for p in /dev/ttyACM*; do
        [[ -e "$p" ]] && PORTS+=("$p")
    done
    log "After recovery: ${#PORTS[@]} device(s): ${PORTS[*]}"
    echo ""
fi

# ============================================================
# Phase 3: Compile (unless pre-built UF2 provided)
# ============================================================
if [[ -n "$PRE_BUILT_UF2" ]]; then
    log "Using pre-built UF2: $PRE_BUILT_UF2"
    UF2_FILE="$PRE_BUILT_UF2"
else
    EXTRA_FLAGS=""
    if [[ -n "$NN_FLAG" ]]; then
        EXTRA_FLAGS="--build-property compiler.cpp.extra_flags=$NN_FLAG"
        log "Compiling with NN beat activation..."
    else
        log "Compiling firmware..."
    fi

    if ! $ARDUINO_CLI compile --fqbn "$FQBN" $EXTRA_FLAGS \
        --output-dir "$BUILD_DIR" "$PROJECT_DIR/blinky-things" 2>&1 | tail -3; then
        fail "Compilation failed"
        exit 1
    fi

    # Convert to UF2
    log "Converting to UF2..."
    if ! python3 "$UF2_TOOL" --build-dir "$BUILD_DIR" --dry-run 2>&1 | tail -5; then
        fail "UF2 conversion failed"
        exit 1
    fi
    UF2_FILE="$BUILD_DIR/blinky-things.uf2"
fi

UF2_SIZE=$(stat -c%s "$UF2_FILE" 2>/dev/null || stat -f%z "$UF2_FILE" 2>/dev/null)
ok "Firmware ready: $(basename "$UF2_FILE") ($(( UF2_SIZE / 1024 )) KB)"
echo ""

# ============================================================
# Phase 4: Flash each device sequentially
# ============================================================
log "${BOLD}Flashing ${#PORTS[@]} device(s)...${NC}"
echo ""

declare -A RESULTS
FAILED=0
SUCCEEDED=0

for port in "${PORTS[@]}"; do
    port_name=$(basename "$port")
    echo -e "${BOLD}━━━ $port_name ━━━${NC}"

    # Check if device is responsive
    if ! timeout 2 python3 -c "
import serial
s = serial.Serial('$port', 115200, timeout=1)
s.close()
" 2>/dev/null; then
        warn "$port_name: port not accessible, skipping"
        RESULTS[$port_name]="SKIP (port not accessible)"
        ((FAILED++)) || true
        echo ""
        continue
    fi

    # Flash using uf2_upload.py
    if python3 "$UF2_TOOL" "$port" --uf2 "$UF2_FILE" 2>&1 | \
        grep -E "SUCCESSFUL|FAILED|NN status|error" | head -5; then
        ok "$port_name: flashed successfully"
        RESULTS[$port_name]="OK"
        ((SUCCEEDED++)) || true
    else
        fail "$port_name: flash failed"
        RESULTS[$port_name]="FAILED"
        ((FAILED++)) || true
    fi

    # Brief pause between devices for USB bus stability
    sleep 2
    echo ""
done

# ============================================================
# Phase 5: Summary
# ============================================================
echo ""
echo -e "${BOLD}════════════════════════════════════════${NC}"
echo -e "${BOLD}  Fleet Flash Summary${NC}"
echo -e "${BOLD}════════════════════════════════════════${NC}"
echo ""

for port_name in $(echo "${!RESULTS[@]}" | tr ' ' '\n' | sort); do
    status="${RESULTS[$port_name]}"
    if [[ "$status" == "OK" ]]; then
        echo -e "  ${GREEN}✓${NC} $port_name: $status"
    else
        echo -e "  ${RED}✗${NC} $port_name: $status"
    fi
done

echo ""
echo -e "  Succeeded: ${GREEN}$SUCCEEDED${NC}/${#PORTS[@]}"
if [[ $FAILED -gt 0 ]]; then
    echo -e "  Failed:    ${RED}$FAILED${NC}/${#PORTS[@]}"
    echo ""
    echo -e "  ${YELLOW}Tip: Try --recover flag to power-cycle devices first${NC}"
fi
echo ""

exit $FAILED
