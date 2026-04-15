#!/bin/bash
# Deploy firmware to blinkyhost devices.
#
# Single command: compile → upload → flash → verify.
# The server handles everything after receiving the binary.
# Fails loudly on ANY problem.
#
# Usage:
#   ./scripts/deploy.sh                    # compile + deploy to all devices
#   ./scripts/deploy.sh --no-bump          # recompile without incrementing build number
#   ./scripts/deploy.sh --skip-compile     # deploy already-compiled hex
#
# Requires: blinky-server running on blinkyhost.local:8420
# API key: reads from ~/.blinky-api-key or BLINKY_API_KEY env var
#
# Exit codes:
#   0 = all devices flashed and verified
#   1 = compile failed
#   2 = upload/flash failed
#   3 = missing API key

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

BLINKY_SERVER="http://blinkyhost.local:8420"
HEX="/tmp/blinky-build/blinky-things.ino.hex"
SKIP_COMPILE=false
NO_BUMP=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --no-bump)       NO_BUMP=true; shift ;;
        --skip-compile)  SKIP_COMPILE=true; shift ;;
        *)               echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

fail() { echo "DEPLOY FAILED: $1" >&2; exit "$2"; }

# ─── Load API key ────────────────────────────────────────────────────

API_KEY="${BLINKY_API_KEY:-}"
if [[ -z "$API_KEY" && -f ~/.blinky-api-key ]]; then
    API_KEY=$(cat ~/.blinky-api-key | tr -d '[:space:]')
fi
if [[ -z "$API_KEY" ]]; then
    fail "No API key. Set BLINKY_API_KEY or create ~/.blinky-api-key" 3
fi

# ─── Step 1: Compile ─────────────────────────────────────────────────

if $SKIP_COMPILE; then
    [[ -f "$HEX" ]] || fail "No hex at $HEX (run without --skip-compile)" 1
    BUILD=$(cat blinky-things/BUILD_NUMBER | tr -d '[:space:]')
    echo "=== Skipping compile, using b${BUILD} ==="
else
    BUILD_ARGS=()
    if $NO_BUMP; then BUILD_ARGS+=(--no-bump); fi
    if ! ./scripts/build.sh "${BUILD_ARGS[@]}"; then
        fail "Compilation failed" 1
    fi
    BUILD=$(cat blinky-things/BUILD_NUMBER | tr -d '[:space:]')
fi

HEX_SIZE=$(stat -c%s "$HEX" 2>/dev/null || stat -f%z "$HEX")
echo "  Firmware: b${BUILD} (${HEX_SIZE} bytes)"

# ─── Step 2: Upload and flash ────────────────────────────────────────

echo ""
echo "=== Uploading to blinkyhost and flashing all devices ==="

RESULT=$(curl -sf -X POST "${BLINKY_SERVER}/api/fleet/upload" \
    -H "X-API-Key: ${API_KEY}" \
    -F "firmware=@${HEX};filename=blinky-things.ino.hex" \
    --max-time 600 \
    2>/dev/null) || fail "Upload failed (server unreachable or rejected)" 2

# Parse result
STATUS=$(echo "$RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin).get('status','error'))" 2>/dev/null)
MESSAGE=$(echo "$RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin).get('message','?'))" 2>/dev/null)

echo ""
echo "$RESULT" | python3 -c "
import json, sys
d = json.load(sys.stdin)
print(f'  Result: {d.get(\"message\", \"?\")}')
for dev_id, info in d.get('per_device', {}).items():
    flash = info.get('flash', '?')
    ver = info.get('version') or '?'
    name = info.get('device_name') or '?'
    ok = '✓' if flash == 'ok' else '✗'
    print(f'    {dev_id}  {name:20s}  flash={flash} {ok}  version={ver}')
"

if [[ "$STATUS" != "ok" ]]; then
    fail "Flash failed: ${MESSAGE}" 2
fi

# ─── Step 3: Reset all devices to defaults ────────────────────────────
# Settings persist in flash across firmware updates. Stale values from
# experiments silently corrupt test results. Always reset after deploy.

echo ""
echo "=== Resetting all devices to defaults ==="
if ! curl -sf -X POST "${BLINKY_SERVER}/api/fleet/command" \
    -H 'Content-Type: application/json' \
    -d '{"command": "defaults"}' > /dev/null 2>&1; then
    echo "  [WARNING] defaults reset may have failed — devices could have stale settings" >&2
fi
if ! curl -sf -X POST "${BLINKY_SERVER}/api/fleet/command" \
    -H 'Content-Type: application/json' \
    -d '{"command": "save"}' > /dev/null 2>&1; then
    echo "  [WARNING] save may have failed" >&2
fi
echo "  Done"

echo ""
echo "============================================================"
echo "  DEPLOY SUCCESSFUL: b${BUILD}"
echo "============================================================"
