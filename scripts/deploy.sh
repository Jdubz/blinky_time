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
#   4 = post-deploy assertion failed (specific device(s) in unexpected state —
#       per-row FAIL output identifies which devices to investigate)
#   5 = all devices unreachable simultaneously (host USB stack heuristic —
#       almost always a host-side problem, not firmware brick. The script
#       prints recovery steps before exiting; see also
#       memory: feedback_brick_diagnosis_first_rule.md)

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

# Identify this deploy.sh invocation to the server. The server enforces
# `X-Deploy-Tool: deploy.sh-<...>` on flash endpoints to make accidental
# direct curl bypasses fail with 403 — see CLAUDE.md "CRITICAL: Upload Safety".
GIT_SHA=$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
DEPLOY_TOOL_HEADER="X-Deploy-Tool: deploy.sh-${GIT_SHA}"

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
    -H "${DEPLOY_TOOL_HEADER}" \
    -F "firmware=@${HEX};filename=blinky-things.ino.hex" \
    -F "version=b${BUILD}" \
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
    -H "${DEPLOY_TOOL_HEADER}" \
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
#
# Timeout budget: a single device can legitimately take 90-180s on a
# flaky USB hub (uhubctl recoveries, multiple bootloader-entry retries),
# and the per-device timeout in uf2_upload.py is itself ~180s. With 4
# devices that's a worst-case 12 min plus the 10s USB-stabilization
# wait and ~30s verify. We poll for 15 min and warn if any single
# device sits in the same progressMessage for more than 4 min — that's
# a strong signal the device-specific failure path is running.

echo ""
echo "=== Waiting for flash to complete ==="

POLL_INTERVAL_S=5
MAX_POLLS=180                     # 180 × 5s = 15 min total budget
PER_PHASE_WARN_S=240              # warn if same progressMessage for >4 min
last_progress=""
job_start_t=$(date +%s)           # never reset — for total elapsed labels
phase_t0=$job_start_t             # reset on each phase change
last_print_t=0

for i in $(seq 1 ${MAX_POLLS}); do
    sleep ${POLL_INTERVAL_S}
    JOB_STATUS=$(curl -sf "${BLINKY_SERVER}/api/fleet/jobs/${JOB_ID}" 2>/dev/null)
    STATUS=$(echo "$JOB_STATUS" | python3 -c "import json,sys; print(json.load(sys.stdin).get('status','unknown'))" 2>/dev/null)
    PROGRESS=$(echo "$JOB_STATUS" | python3 -c "import json,sys; print(json.load(sys.stdin).get('progressMessage',''))" 2>/dev/null)

    now=$(date +%s)
    elapsed_total=$((now - job_start_t))
    if [[ "$PROGRESS" != "$last_progress" ]]; then
        # New phase. Reset the in-phase clock; the t+Ns label always
        # measures from job_start_t (total elapsed since polling began),
        # not since the previous phase, so operators can see the run
        # duration at a glance.
        last_progress="$PROGRESS"
        phase_t0=$now
        last_print_t=$now
        echo "  [t+${elapsed_total}s] ${STATUS}: ${PROGRESS}"
    elif (( now - last_print_t >= 30 )); then
        # Same phase for 30+ seconds — print a heartbeat with both
        # in-phase elapsed (for stall detection) and total elapsed
        # (for run-duration awareness).
        in_phase=$((now - phase_t0))
        last_print_t=$now
        warn_marker=""
        if (( in_phase >= PER_PHASE_WARN_S )); then
            warn_marker=" [WARN: stuck >${PER_PHASE_WARN_S}s on this phase]"
        fi
        echo "  [t+${elapsed_total}s in-phase ${in_phase}s]${warn_marker}"
    fi

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
    extra = ''
    if flash != 'ok' and info.get('message'):
        extra = f'  [{info[\"message\"][:80]}]'
    print(f'    {dev_id}  {name:20s}  flash={flash} {ok}  version={ver}{extra}')
"
        FLASH_STATUS=$(echo "$JOB_STATUS" | python3 -c "import json,sys; print(json.load(sys.stdin).get('result',{}).get('status','error'))" 2>/dev/null)
        if [[ "$FLASH_STATUS" != "ok" ]]; then
            # Compute partial-success message
            PARTIAL_MSG=$(echo "$JOB_STATUS" | python3 -c "import json,sys; print(json.load(sys.stdin).get('result',{}).get('message','?'))" 2>/dev/null)
            fail "Flash completed with errors: ${PARTIAL_MSG}. Re-run './scripts/deploy.sh --skip-compile' to retry the failed devices." 2
        fi
        break
    elif [[ "$STATUS" == "error" ]]; then
        ERROR=$(echo "$JOB_STATUS" | python3 -c "import json,sys; print(json.load(sys.stdin).get('error','unknown'))" 2>/dev/null)
        fail "Flash failed: ${ERROR}" 2
    fi
done

if [[ "$STATUS" != "complete" ]]; then
    # Surface what we last saw so the operator knows where it stalled,
    # not just "timed out".
    fail "Flash polling timed out after $((MAX_POLLS * POLL_INTERVAL_S))s. Last server status: '${PROGRESS}'. Re-run './scripts/deploy.sh --skip-compile' to retry, or check 'sudo journalctl -u blinky-server' for the full per-device log." 2
fi

# ─── Step 5: Restore runtime settings (defaults) ────────────────────────
# Note: `defaults` resets soft runtime tunables (mic, audio tracker params,
# generators) — it does NOT wipe device identity (matrix size, deviceId).
# That preservation is intentional: fleet devices keep their identity across
# every firmware deploy. To wipe identity, use `factory`/`reset` (heavy,
# wipes everything; deploy.sh does not invoke it because it would unconfig
# every device on every flash).
#
# Per-device status is now displayed instead of "Done" — pre-2026-05-01
# this step silently skipped devices not in CONNECTED state. Now devices
# that didn't ACK the command surface explicitly and the deploy fails loud.

run_fleet_command() {
    local cmd_label="$1"
    local cmd="$2"
    local resp_json
    # Build the JSON body via python3 to handle quoting safely. Embedding
    # ${cmd} bare into a "{\"command\": \"${cmd}\"}" template would silently
    # corrupt the body for any cmd containing quotes/braces (per PR 138
    # review). Current callers only pass static strings, but that's not a
    # property we want to depend on if the function is reused later.
    local body
    body=$(CMD="$cmd" python3 -c 'import json, os; print(json.dumps({"command": os.environ["CMD"]}))') || {
        echo "  [ERROR] ${cmd_label}: failed to build JSON body" >&2
        return 1
    }
    resp_json=$(curl -sf -X POST "${BLINKY_SERVER}/api/fleet/command" \
        -H "X-API-Key: ${API_KEY}" \
        -H "${DEPLOY_TOOL_HEADER}" \
        -H 'Content-Type: application/json' \
        -d "$body" 2>&1) || {
        echo "  [ERROR] ${cmd_label}: HTTP request failed" >&2
        return 1
    }

    # Parse the per-device dict, print one row per device, and exit non-zero
    # if any device's response doesn't start with "OK". Per PR 138 review:
    # the previous "fail only on skipped/error prefixes" was too permissive
    # — a firmware error string that didn't start with those prefixes would
    # silently pass. Firmware command handlers all respond with "OK..." on
    # success (see SerialConsole.cpp), so anchor on that.
    if ! echo "$resp_json" | python3 -c "
import json, sys
data = json.loads(sys.stdin.read())
fails = 0
for dev_id, resp in data.items():
    short = dev_id[:12]
    resp_str = str(resp).strip()
    first_line = resp_str.split(chr(10))[0][:60]
    if resp_str.startswith('OK'):
        print(f'  {short} OK ({first_line})')
    else:
        print(f'  {short} FAIL: {first_line}')
        fails += 1
sys.exit(1 if fails else 0)
"; then
        return 1
    fi
    return 0
}

echo ""
echo "=== Restoring runtime settings ==="
if ! run_fleet_command "restore_runtime_settings" "restore_runtime_settings"; then
    fail "restore_runtime_settings did not land on every device. State stale; investigate before next test." 4
fi

echo ""
echo "=== Saving settings to flash ==="
if ! run_fleet_command "save" "save"; then
    fail "save command did not land on every device. Reboot will lose the just-applied defaults." 4
fi

# ─── Step 6: Post-deploy state assertion ────────────────────────────────
# Verify each device's actual state matches expectations. Catches:
#   - flash succeeded but device booted to wrong version (rare, partial flash)
#   - audio loop overrun-bound at boot (fixed by b156 drain loop, but regressions
#     would surface here)
#   - fps unexpectedly low (perf regression, e.g., the v36-fmax merge gate)

echo ""
echo "=== Verifying post-deploy state ==="
sleep 6  # let the LoopMetrics 5s window close so fps reads are non-zero

export EXPECTED_BUILD="b${BUILD}"
export API_KEY BLINKY_SERVER
DEVICES_JSON=$(curl -sf -H "X-API-Key: ${API_KEY}" "${BLINKY_SERVER}/api/devices" 2>&1) || {
    fail "Failed to list devices for post-deploy verification: ${DEVICES_JSON}" 4
}

echo "$DEVICES_JSON" | python3 -c "
import json, sys, urllib.request, os

API_KEY = os.environ['API_KEY']
SERVER = os.environ['BLINKY_SERVER']
EXPECTED = os.environ['EXPECTED_BUILD']

devices = json.loads(sys.stdin.read())
fails = []
for d in devices:
    dev_id = d.get('id', '?')
    short = dev_id[:12]
    state = d.get('state', '?')
    if state != 'connected':
        fails.append(f'{short} state={state}')
        print(f'  {short} FAIL: state={state}')
        continue
    req = urllib.request.Request(
        f'{SERVER}/api/devices/{dev_id}/command',
        data=b'{\"command\": \"json info\"}',
        headers={'X-API-Key': API_KEY, 'Content-Type': 'application/json'},
        method='POST',
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            wrap = json.loads(r.read())
        info = json.loads(wrap['response'])
    except Exception as e:
        fails.append(f'{short} json info failed: {e}')
        print(f'  {short} FAIL: json info: {e}')
        continue

    version = info.get('version', '?')
    fps = info.get('fps', 0.0)
    overruns = info.get('audioOverruns', -1)
    samples_lost = info.get('audioSamplesLost', -1)

    # Hard checks: version, basic counters present.
    if version != EXPECTED:
        fails.append(f'{short} version={version} expected={EXPECTED}')
        print(f'  {short} FAIL: version={version} expected={EXPECTED} fps={fps} overruns={overruns}')
        continue
    if overruns < 0 or samples_lost < 0:
        # New firmware should always expose these; if missing, instrumentation regressed.
        fails.append(f'{short} missing audio overrun counters')
        print(f'  {short} FAIL: missing overrun counters in json info')
        continue

    # Soft warnings: fps low / overruns high. Don't fail deploy on these
    # (boot-time stalls produce a few overruns expectedly), but surface them.
    warn = []
    if fps > 0 and fps < 30:
        warn.append(f'fps={fps:.1f}<30')
    if overruns > 5:
        warn.append(f'overruns={overruns}')
    warn_str = ' WARN:' + ','.join(warn) if warn else ''
    print(f'  {short} OK version={version} fps={fps:.1f} overruns={overruns}{warn_str}')

# Failure-class taxonomy (#142): if EVERY device is unreachable, the cause is
# almost certainly upstream (host USB stack, server, network) rather than
# coincident firmware bricks on N devices. Surface that distinction so the
# operator knows whether to investigate firmware or restart blinkyhost.
# 2026-05-01 incident: I diagnosed '4 bricked devices' from 'all 4 unreachable'
# when actually blinkyhost's USB stack was stale. Reboot fixed it; firmware
# was fine. Saved as memory: feedback_brick_diagnosis_first_rule.md.
total = len(devices)
unreachable = sum(1 for d in devices if d.get('state') != 'connected')
all_unreachable = total > 0 and unreachable == total
print(f'pass={total-len(fails)}/{total}', file=sys.stderr)
if all_unreachable:
    print(f'all_unreachable=1', file=sys.stderr)
sys.exit(1 if fails else 0)
" || {
    # Re-evaluate whether the failure is host-side (all unreachable) or
    # device-specific. If host-side, advise reboot rather than implying
    # bricks; if device-specific, the per-row FAIL output above tells
    # the operator which devices to investigate.
    UNREACHABLE_COUNT=$(curl -sf -H "X-API-Key: ${API_KEY}" "${BLINKY_SERVER}/api/devices" 2>/dev/null | \
        python3 -c "import json,sys; ds=json.load(sys.stdin); print(sum(1 for d in ds if d.get('state')!='connected'), len(ds))")
    UNREACH=$(echo "$UNREACHABLE_COUNT" | awk '{print $1}')
    TOTAL=$(echo "$UNREACHABLE_COUNT" | awk '{print $2}')
    if [[ -n "$TOTAL" && "$TOTAL" -gt 0 && "$UNREACH" == "$TOTAL" ]]; then
        echo ""
        echo "  All $TOTAL device(s) unreachable simultaneously — this pattern almost"
        echo "  always means the HOST USB stack is stuck (kernel/udev), not firmware"
        echo "  bricks. Try:"
        echo "    1. ssh blinkyhost.local sudo systemctl restart systemd-udevd"
        echo "    2. If that doesn't help, ssh blinkyhost.local sudo reboot"
        echo "    3. Then re-run this deploy with --skip-compile"
        echo "  See memory: feedback_brick_diagnosis_first_rule.md"
        fail "All devices unreachable — likely host USB stack, not firmware. Recovery steps printed above." 5
    else
        fail "Post-deploy state assertion failed (see rows above). $UNREACH/$TOTAL device(s) in unexpected state — investigate per-device causes." 4
    fi
}

echo ""
echo "============================================================"
echo "  DEPLOY SUCCESSFUL: b${BUILD}"
echo "============================================================"
