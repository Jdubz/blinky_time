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
# How long to wait after flashing before reading back fps via json info.
# LoopMetrics publishes a fresh fps reading every WINDOW_MS (5 s) — we add
# a 1 s margin so the first window has unambiguously closed before the
# post-deploy assertion runs. Per PR 138 round-14 review (LOW #3, name
# the magic number rather than burying it inline). When v36-fmax ships
# with NN re-enabled, this also gates the fps≥30 hard check.
LOOP_METRICS_SETTLE_S=6

DEVICES_ARG=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --no-bump)       NO_BUMP=true; shift ;;
        --skip-compile)  SKIP_COMPILE=true; shift ;;
        --devices)       DEVICES_ARG="$2"; shift 2 ;;
        --devices=*)     DEVICES_ARG="${1#--devices=}"; shift ;;
        -h|--help)
            cat <<EOF
Usage: ./scripts/deploy.sh --devices <list> [--no-bump] [--skip-compile]

  --devices <list>   Comma-separated device IDs OR the literal 'all' to
                     deploy to every flashable device in range. Required.
                     Use --devices=list to print the candidate IDs without
                     starting a deploy.

Devices in dfu_recovery are intentionally excluded from 'all' — flashing
a stuck device is a deliberate operator action via --devices <id>.
EOF
            exit 0
            ;;
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

# Show the candidate flashable devices and exit. Useful for figuring out
# which IDs to pass to --devices without committing to a deploy.
if [[ "$DEVICES_ARG" == "list" ]]; then
    echo "Flashable devices currently visible to blinky-server:"
    curl -sf "${BLINKY_SERVER}/api/devices" | python3 -c "
import json, sys
flashable = {'connected', 'present'}
ds = json.load(sys.stdin)
candidates = [d for d in ds if d.get('platform') == 'nrf52840' and d.get('state') in flashable]
if not candidates:
    print('  (none)')
    sys.exit(0)
for d in candidates:
    print(f\"  {d['id']:25} state={d['state']:9} name={d.get('device_name') or '-'}\")"
    exit 0
fi

# Explicit device whitelist is required: no auto-derive of 'all flashable',
# because that scope picks up sibling devices an operator might not want
# to touch (development units in the same room, etc.). Pass --devices=all
# to opt into 'every flashable device in range', or --devices=ID1,ID2 for
# an explicit subset.
if [[ -z "$DEVICES_ARG" ]]; then
    fail "missing --devices. Use './scripts/deploy.sh --devices list' to see candidates, then --devices=ID1,ID2 or --devices=all." 6
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

# ─── Step 3: Resolve the target device whitelist ─────────────────────
#
# The whitelist is required (see arg parsing above). Two forms accepted:
#   --devices=all       -> every flashable nRF52840 in range
#   --devices=ID1,ID2   -> exactly these device IDs
#
# In both cases we resolve to a JSON array and pass it to /api/fleet/flash.
# The server validates each ID exists + is flashable; unknown or wrong-
# state IDs fail loudly rather than being silently dropped.

DEVICES_JSON=$(curl -sf "${BLINKY_SERVER}/api/devices" --max-time 5 2>/dev/null) \
    || fail "Could not query /api/devices for whitelist" 2

DEVICE_IDS_JSON=$(echo "$DEVICES_JSON" | DEVICES_ARG="$DEVICES_ARG" python3 -c "
import json, os, sys
ds = json.load(sys.stdin)
flashable = {'connected', 'present'}
arg = os.environ['DEVICES_ARG']
if arg == 'all':
    candidates = [d for d in ds if d.get('platform') == 'nrf52840' and d.get('state') in flashable]
    ids = [d['id'] for d in candidates]
    print(json.dumps(ids))
else:
    # Explicit comma-separated list. Trust the operator on naming; the
    # server will 400 on any unknown / not-flashable ID.
    ids = [s.strip() for s in arg.split(',') if s.strip()]
    print(json.dumps(ids))
")

DEVICE_COUNT_PRE=$(echo "$DEVICE_IDS_JSON" | python3 -c "import json,sys; print(len(json.load(sys.stdin)))")
if [[ "$DEVICE_COUNT_PRE" -eq 0 ]]; then
    fail "No devices in the --devices whitelist (got '${DEVICES_ARG}')" 2
fi
echo "  Targets (${DEVICES_ARG}): ${DEVICE_COUNT_PRE} device(s)"
echo "$DEVICE_IDS_JSON" | python3 -c "import json,sys; [print(f'    {i}') for i in json.load(sys.stdin)]"

# ─── Step 4: Flash whitelisted devices ───────────────────────────────

echo ""
echo "=== Flashing whitelisted devices ==="

FLASH_RESULT=$(curl -sf -X POST "${BLINKY_SERVER}/api/fleet/flash" \
    -H "X-API-Key: ${API_KEY}" \
    -H "${DEPLOY_TOOL_HEADER}" \
    -H 'Content-Type: application/json' \
    -d "{\"firmware_path\": \"${FW_PATH}\", \"device_ids\": ${DEVICE_IDS_JSON}}" \
    --max-time 10 \
    2>/dev/null) || fail "Flash request failed" 2

JOB_ID=$(echo "$FLASH_RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin).get('job_id',''))" 2>/dev/null)
DEVICE_COUNT=$(echo "$FLASH_RESULT" | python3 -c "import json,sys; print(json.load(sys.stdin).get('devices',0))" 2>/dev/null)
echo "  Job: ${JOB_ID} (${DEVICE_COUNT} devices)"

if [[ -z "$JOB_ID" ]]; then
    fail "Flash returned no job_id" 2
fi

# ─── Step 5: Poll for flash completion ───────────────────────────────
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

# ─── Step 6: Restore runtime settings (defaults) ────────────────────────
# Note: `defaults` resets soft runtime tunables (mic, audio tracker params,
# generators) — it does NOT wipe device identity (matrix size, deviceId).
# That preservation is intentional: fleet devices keep their identity across
# every firmware deploy. To wipe identity, use `wipe_device_identity`
# (formerly `factory`/`reset` — old aliases still work, see #141). Heavy:
# wipes everything; deploy.sh does not invoke it because it would unconfig
# every device on every flash.
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
    resp_json=$(curl -sf --max-time 15 -X POST "${BLINKY_SERVER}/api/fleet/command" \
        -H "X-API-Key: ${API_KEY}" \
        -H "${DEPLOY_TOOL_HEADER}" \
        -H 'Content-Type: application/json' \
        -d "$body" 2>&1) || {
        # Surface the captured stderr so the operator sees timeout vs 403 vs
        # 5xx vs network error, not just a generic "HTTP request failed".
        # Per PR 138 round-12 review.
        echo "  [ERROR] ${cmd_label}: HTTP request failed: ${resp_json}" >&2
        return 1
    }

    # Parse the per-device dict, print one row per device, and exit non-zero
    # if any device's response is a real failure. Accept two success shapes:
    #   - "OK..."      — a connected device executed the command.
    #   - "skipped: ..." — the server did NOT directly command this device
    #     because it isn't connected (BLE devices sit at state=present, no
    #     persistent GATT link). For the FLEET-BROADCAST commands this helper
    #     runs (restore_runtime_settings, save), the command still reaches
    #     those devices over the air via the broadcaster — the "broadcast"
    #     key (itself an "OK"/non-OK entry) is the real delivery signal. The
    #     server's own API docs call skipped: informational, not an error
    #     (routes_commands.fleet_restore_defaults). Anything ELSE (a firmware
    #     error string) is still a hard failure — we only widen the
    #     allowlist to the server-generated "skipped:" prefix, not arbitrary
    #     non-OK strings (the PR 138 anchor-on-OK concern stands).
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
    elif resp_str.startswith('skipped:'):
        # Not connected — broadcast still reached it. Informational.
        print(f'  {short} skipped (not connected; broadcast still sent): {first_line}')
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

# ─── Step 7: Post-deploy state assertion ────────────────────────────────
# Verify each device's actual state matches expectations. Catches:
#   - flash succeeded but device booted to wrong version (rare, partial flash)
#   - audio loop overrun-bound at boot (fixed by b156 drain loop, but regressions
#     would surface here)
#   - fps unexpectedly low (perf regression, e.g., the v36-fmax merge gate)

echo ""
echo "=== Verifying post-deploy state ==="
sleep "$LOOP_METRICS_SETTLE_S"  # let the LoopMetrics 5s window close so fps reads are non-zero

export EXPECTED_BUILD="b${BUILD}"
export API_KEY BLINKY_SERVER
DEVICES_JSON=$(curl -sf --max-time 15 -H "X-API-Key: ${API_KEY}" "${BLINKY_SERVER}/api/devices" 2>&1) || {
    fail "Failed to list devices for post-deploy verification: ${DEVICES_JSON}" 4
}

# Verify ONLY the devices we targeted (DEVICE_IDS_JSON), not the whole
# fleet — a subset deploy must not fail on devices it never touched.
# Uses a quoted-delimiter heredoc so bash does NO expansion of the body
# (the previous `python3 -c "..."` form let bash try to execute f-string
# fragments — `fps: command not found` — on the error path). Inputs come
# in via the environment.
set +e
# Pass the (potentially large) /api/devices payload via a temp FILE, not an
# env var: a big fleet's JSON can exceed the OS environment-size limit
# (ARG_MAX / E2BIG), which would make the verifier fail on valid JSON. Small,
# bounded values (TARGET_IDS, EXPECTED, key, server) stay in the environment.
DEVICES_JSON_FILE="$(mktemp)"
printf '%s' "$DEVICES_JSON" > "$DEVICES_JSON_FILE"
DEVICES_JSON_FILE="$DEVICES_JSON_FILE" \
TARGET_IDS="$DEVICE_IDS_JSON" \
EXPECTED_BUILD="b${BUILD}" \
API_KEY="$API_KEY" \
BLINKY_SERVER="$BLINKY_SERVER" \
python3 - <<'PYEOF'
import json, os, sys, urllib.request

API_KEY = os.environ['API_KEY']
SERVER = os.environ['BLINKY_SERVER']
EXPECTED = os.environ['EXPECTED_BUILD']
targets = sorted(set(json.loads(os.environ['TARGET_IDS'])))
with open(os.environ['DEVICES_JSON_FILE']) as _f:
    by_id = {d.get('id'): d for d in json.load(_f)}


def version_matches(v):
    # Firmware version is "b<num>-<sha>[-dirty]"; EXPECTED is "b<num>".
    # Prefix-match on the build token + dash so the SHA suffix doesn't
    # spuriously fail, while "b1900" still won't match "b190".
    return v == EXPECTED or v.startswith(EXPECTED + '-')


def get_json_info(dev_id):
    # 'json info' over the device API. Only valid for CONNECTED devices —
    # the server 409s a non-connected (BLE 'present') device. One retry
    # absorbs transient flakiness on a connected device.
    # Returns (info_dict | None, err | None).
    req = urllib.request.Request(
        f'{SERVER}/api/devices/{dev_id}/command',
        data=json.dumps({'command': 'json info'}).encode(),
        headers={'X-API-Key': API_KEY, 'Content-Type': 'application/json'},
        method='POST',
    )
    last_err = None
    for _ in range(2):
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                wrap = json.loads(r.read())
            return json.loads(wrap['response']), None
        except Exception as e:  # noqa: BLE001 — report, then retry/fail
            last_err = e
    return None, last_err


fails = []
unreachable = 0
for dev_id in targets:
    short = dev_id[:12]
    d = by_id.get(dev_id)
    if d is None:
        fails.append(f'{short} missing')
        print(f'  {short} FAIL: not in /api/devices (disappeared post-flash)')
        continue
    state = d.get('state', '?')
    # 'error' / 'dfu_recovery' are bad terminal post-flash states.
    if state in ('error', 'dfu_recovery'):
        fails.append(f'{short} state={state}')
        print(f'  {short} FAIL: state={state}')
        continue

    # Sealed BLE devices sit at 'present' (advertising in APP mode, no
    # persistent GATT link) and 409 on per-device commands — so we can't
    # json-info them. But 'present' in app mode IS the health signal: the
    # app booted and is advertising NUS (a bricked / bootlooping device
    # would be dfu_recovery or absent, not present). Their FIRMWARE VERSION
    # was already verified by the flash job, which deploy.sh gated on at the
    # 'Waiting for flash to complete' step (a flash=error there aborts before
    # this assertion ever runs). So accept a non-connected present device
    # without json-info. Only 'connected' devices (serial / active GATT) get
    # the full version/fps/overruns re-check below.
    if state != 'connected':
        print(f'  {short} OK (state={state}; app advertising — version verified by flash job; no GATT for json info)')
        continue

    info, err = get_json_info(dev_id)
    if info is None:
        unreachable += 1
        fails.append(f'{short} unreachable')
        print(f'  {short} FAIL: json info unreachable after retry: {err}')
        continue

    version = info.get('version', '?')
    fps = info.get('fps', 0.0)
    overruns = info.get('audioOverruns', -1)
    samples_lost = info.get('audioSamplesLost', -1)

    if not version_matches(version):
        fails.append(f'{short} version={version}')
        print(f'  {short} FAIL: version={version} expected={EXPECTED}* fps={fps} overruns={overruns}')
        continue
    if overruns < 0 or samples_lost < 0:
        # New firmware should always expose these; if missing, instrumentation regressed.
        fails.append(f'{short} missing audio overrun counters')
        print(f'  {short} FAIL: missing overrun counters in json info')
        continue

    # Soft warnings: fps low / overruns high. Don't fail deploy on these
    # (boot-time stalls produce a few overruns expectedly), but surface them.
    warn = []
    if fps == 0.0:
        warn.append('fps=0.0(window-unclosed)')
    elif fps < 30:
        warn.append(f'fps={fps:.1f}<30')
    if overruns > 5:
        warn.append(f'overruns={overruns}')
    warn_str = '  WARN: ' + ', '.join(warn) if warn else ''
    print(f'  {short} OK version={version} fps={fps:.1f} overruns={overruns}{warn_str}')

# Failure-class taxonomy (#142): if EVERY targeted device is unreachable,
# the cause is almost certainly upstream (host USB stack, server, network)
# rather than coincident firmware bricks. Surface that distinction.
# (feedback_brick_diagnosis_first_rule.md.)
total = len(targets)
print(f'pass={total - len(fails)}/{total}', file=sys.stderr)
if total > 0 and unreachable == total:
    print('all_unreachable=1', file=sys.stderr)
sys.exit(1 if fails else 0)
PYEOF
ASSERT_RC=$?
rm -f "$DEVICES_JSON_FILE"
set -e
if [ "$ASSERT_RC" -ne 0 ]; then
    # Re-evaluate whether the failure is host-side (all targets off the bus)
    # or device-specific. Scope to the TARGET devices. A device counts as "on
    # the bus" if the server can SEE it in ANY state — 'present'/'connected'
    # (healthy), but ALSO 'connecting' and 'dfu_recovery': a device in DFU is
    # very much present (advertising AdaDFU), and 'connecting' is transient.
    # Only a device the server can't see at all (missing / disconnected) is
    # genuinely "off the bus" and points at a stuck host USB stack. Counting
    # dfu_recovery/connecting as off-bus would mis-route a real device-specific
    # failure (e.g. one chip stuck in DFU) into the "restart udevd / reboot
    # host" branch. Double-quoted -c with no '$' inside is bash-expansion-safe;
    # TARGET_IDS is passed via the environment.
    DIAG=$(curl -sf --max-time 10 -H "X-API-Key: ${API_KEY}" "${BLINKY_SERVER}/api/devices" 2>/dev/null | \
        TARGET_IDS="$DEVICE_IDS_JSON" python3 -c "import json,os,sys; ds={d.get('id'):d for d in json.load(sys.stdin)}; t=json.loads(os.environ['TARGET_IDS']); off=sum(1 for x in t if ds.get(x,{}).get('state') not in ('present','connected','connecting','dfu_recovery')); print(off, len(t))" 2>/dev/null)
    OFF=$(echo "$DIAG" | awk '{print $1}')
    TOTAL=$(echo "$DIAG" | awk '{print $2}')
    # Three branches:
    #   (a) TOTAL empty → server itself unreachable (curl failed)
    #   (b) TOTAL > 0 AND OFF == TOTAL → all targets off the bus, likely host USB
    #   (c) Otherwise → specific device(s) failed, deploy state diverged
    if [[ -z "$TOTAL" ]]; then
        echo ""
        echo "  Could not reach blinky-server to diagnose post-deploy state."
        echo "  Either the server crashed mid-deploy, the network is down, or the"
        echo "  X-API-Key was rejected. Check:"
        echo "    1. ssh blinkyhost.local sudo systemctl status blinky-server"
        echo "    2. ssh blinkyhost.local sudo journalctl -u blinky-server -n 50"
        fail "Could not reach server to diagnose post-deploy state — server may be down." 5
    elif [[ "$TOTAL" -gt 0 && "$OFF" == "$TOTAL" ]]; then
        echo ""
        echo "  All $TOTAL targeted device(s) off the bus simultaneously — this pattern"
        echo "  almost always means the HOST USB stack is stuck (kernel/udev), not"
        echo "  firmware bricks. Try:"
        echo "    1. ssh blinkyhost.local sudo systemctl restart systemd-udevd"
        echo "    2. If that doesn't help, ssh blinkyhost.local sudo reboot"
        echo "    3. Then re-run this deploy with --skip-compile"
        echo "  See memory: feedback_brick_diagnosis_first_rule.md"
        fail "All targeted devices off the bus — likely host USB stack, not firmware. Recovery steps printed above." 5
    else
        fail "Post-deploy state assertion failed (see rows above). $OFF/$TOTAL targeted device(s) in unexpected state — investigate per-device causes." 4
    fi
fi

echo ""
echo "============================================================"
echo "  DEPLOY SUCCESSFUL: b${BUILD}"
echo "============================================================"
