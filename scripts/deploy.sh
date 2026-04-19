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

# ─── Step 2: Upload firmware ─────────────────────────────────────────

echo ""
echo "=== Uploading firmware to blinkyhost ==="

UPLOAD_RESULT=$(curl -sf -X POST "${BLINKY_SERVER}/api/fleet/upload" \
    -H "X-API-Key: ${API_KEY}" \
    -F "firmware=@${HEX};filename=blinky-things.ino.hex" \
    --max-time 30 \
    2>/dev/null) || fail "Upload failed (server unreachable or rejected)" 2

FW_PATH=$(echo "$UPLOAD_RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin).get('firmware_path',''))" 2>/dev/null)
echo "  Uploaded: ${FW_PATH}"

if [[ -z "$FW_PATH" ]]; then
    fail "Upload returned no firmware_path" 2
fi

# ─── Step 3: Flash all devices ───────────────────────────────────────

echo ""
echo "=== Flashing all devices ==="

FLASH_RESULT=$(curl -sf -X POST "${BLINKY_SERVER}/api/fleet/flash" \
    -H "X-API-Key: ${API_KEY}" \
    -H 'Content-Type: application/json' \
    -d "{\"firmware_path\": \"${FW_PATH}\"}" \
    --max-time 10 \
    2>/dev/null) || fail "Flash request failed" 2

JOB_ID=$(echo "$FLASH_RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin).get('job_id',''))" 2>/dev/null)
DEVICE_COUNT=$(echo "$FLASH_RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin).get('devices',0))" 2>/dev/null)
echo "  Job: ${JOB_ID} (${DEVICE_COUNT} devices)"

if [[ -z "$JOB_ID" ]]; then
    fail "Flash returned no job_id" 2
fi

# ─── Step 4: Poll for flash completion ───────────────────────────────

echo ""
echo "=== Waiting for flash to complete ==="

for i in $(seq 1 60); do  # 60 × 5s = 5 min max
    sleep 5
    JOB_STATUS=$(curl -sf "${BLINKY_SERVER}/api/fleet/jobs/${JOB_ID}" 2>/dev/null)
    STATUS=$(echo "$JOB_STATUS" | python3 -c "import json,sys; print(json.load(sys.stdin).get('status','unknown'))" 2>/dev/null)
    PROGRESS=$(echo "$JOB_STATUS" | python3 -c "import json,sys; print(json.load(sys.stdin).get('progressMessage',''))" 2>/dev/null)

    echo "  [${i}] ${STATUS}: ${PROGRESS}"

    if [[ "$STATUS" == "complete" ]]; then
        echo ""
        echo "$JOB_STATUS" | python3 -c "
import json, sys
d = json.load(sys.stdin)
r = d.get('result', {})
print(f'  Result: {r.get(\"message\", \"?\")}')
for dev_id, info in r.get('per_device', {}).items():
    flash = info.get('flash', '?')
    ver = info.get('version') or '?'
    name = info.get('device_name') or '?'
    ok = '✓' if flash == 'ok' else '✗'
    print(f'    {dev_id}  {name:20s}  flash={flash} {ok}  version={ver}')
"
        FLASH_STATUS=$(echo "$JOB_STATUS" | python3 -c "import json,sys; print(json.load(sys.stdin).get('result',{}).get('status','error'))" 2>/dev/null)
        if [[ "$FLASH_STATUS" != "ok" ]]; then
            fail "Flash completed with errors" 2
        fi
        break
    elif [[ "$STATUS" == "error" ]]; then
        ERROR=$(echo "$JOB_STATUS" | python3 -c "import json,sys; print(json.load(sys.stdin).get('error','unknown'))" 2>/dev/null)
        fail "Flash failed: ${ERROR}" 2
    fi
done

if [[ "$STATUS" != "complete" ]]; then
    fail "Flash timed out (5 min). Check server logs." 2
fi

# ─── Step 5: Reset all devices to defaults ────────────────────────────
# Settings persist in flash across firmware updates. Stale values from
# experiments silently corrupt test results. Always reset after deploy.

echo ""
echo "=== Resetting all devices to defaults ==="
if ! curl -sf -X POST "${BLINKY_SERVER}/api/fleet/command" \
    -H "X-API-Key: ${API_KEY}" \
    -H 'Content-Type: application/json' \
    -d '{"command": "defaults"}' > /dev/null 2>&1; then
    echo "  [WARNING] defaults reset may have failed — devices could have stale settings" >&2
fi
if ! curl -sf -X POST "${BLINKY_SERVER}/api/fleet/command" \
    -H "X-API-Key: ${API_KEY}" \
    -H 'Content-Type: application/json' \
    -d '{"command": "save"}' > /dev/null 2>&1; then
    echo "  [WARNING] save may have failed" >&2
fi
echo "  Done"

echo ""
echo "============================================================"
echo "  DEPLOY SUCCESSFUL: b${BUILD}"
echo "============================================================"
