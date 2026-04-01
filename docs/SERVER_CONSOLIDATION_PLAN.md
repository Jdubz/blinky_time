# Server Consolidation Plan

*Created: April 1, 2026*

## Problem

Multiple independent systems manage serial connections, compute metrics, and run tests:

| System | Language | Manages serial? | Scoring |
|--------|----------|----------------|---------|
| **blinky-server** | Python/FastAPI | Yes (fleet manager) | Phase 1 (beat/BPM metrics still present) |
| **blinky-serial-mcp** | TypeScript/MCP | Yes (direct serial) | Own scoring.ts (stale) |
| **ml-training/tools/*.cjs** | Node.js CLI | Yes (direct serial) | Inline per-script (stale, BPM-based) |
| **tools/*.py** | Python CLI | Yes (direct serial) | None |
| **blinky-test-player** | TypeScript CLI | Yes (direct serial) | None |

This causes:
- **Port contention**: Server and tools race for serial ports. Server silently consumes streaming data, causing tools to collect 0 samples.
- **Stale metrics**: Multiple implementations compute BPM error, beat F1, downbeat alignment — metrics we don't use and never should have scored.
- **Confusing terminology**: "OTA" endpoints that do serial UF2 upload. Beat/BPM fields polluting scoring output.
- **Maintenance burden**: 14 CJS scripts, 6 Python tools, an MCP server, and a test player all open serial ports independently.

## Principles

1. **Zero callable scripts outside blinky-server.** Every device interaction goes through the server API. No standalone serial tools.
2. **Battle-tested code moves, doesn't change.** `uf2_upload.py` is proven over hundreds of flashes. When it moves into the server package, its logic stays byte-for-byte identical. The server continues to invoke it as a subprocess (proven pattern, zero regression risk).
3. **Only two metrics matter: pattern strength and onset accuracy.** All BPM, beat F1, downbeat, CMLt/CMLc/AMLt scoring is deleted. These metrics are meaningless for a visualizer — where an onset falls in the meter is irrelevant. The only questions: does the PLP pattern lock on? Do onsets fire on kicks and snares?
4. **Clean naming.** "OTA" → "firmware". "beat" → "onset". No legacy terminology.
5. **Only build what we actually use.** Parameter sweeps and validation runs. That's it. No A/B test framework, no pattern memory test, no model comparison, no config test, no phase/downbeat eval. These were either never used or are subsumed by parameter sweeps.
6. **Server owns all serial.** No file-based inter-process locking. The server's asyncio event loop serializes all port access. For firmware upload, the server disconnects the transport before spawning the subprocess.

## Architecture After Consolidation

```
┌───────────────────────────────────────────────────────────────────────┐
│                          blinky-server                                │
│                    (Python/FastAPI, port 8420)                         │
│                                                                       │
│  ┌──────────────────┐  ┌───────────────────┐  ┌───────────────────┐  │
│  │  Fleet Manager    │  │  Test Runner       │  │  Scoring Engine   │  │
│  │  (discovery,      │  │  (param sweep,     │  │  (onset F1,       │  │
│  │   reconnect,      │  │   validation,      │  │   PLP metrics,    │  │
│  │   streaming)      │  │   audio playback,  │  │   latency est.)   │  │
│  │                   │  │   job management)  │  │                   │  │
│  └──────┬───────────┘  └──────┬────────────┘  └───────────────────┘  │
│         │                     │                                       │
│  ┌──────┴──────────────────────┴───────────────────────────────────┐  │
│  │                    Transport Layer                               │  │
│  │  Serial (pyserial-asyncio) │ BLE (bleak) │ WiFi (TCP socket)   │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                                                                       │
│  ┌──────────────────────┐  ┌──────────────────────────────────────┐  │
│  │  Firmware Upload      │  │  Internal Modules                    │  │
│  │  (uf2_upload.py,      │  │  (compile, audio_player,             │  │
│  │   ble_dfu.py —        │  │   track_discovery)                   │  │
│  │   battle-tested,      │  │                                      │  │
│  │   subprocess call)    │  │                                      │  │
│  └──────────────────────┘  └──────────────────────────────────────┘  │
│                                                                       │
│  REST: /api/devices, /api/test, /api/fleet, /api/firmware             │
│  WebSocket: /ws/{device_id}, /ws/fleet                                │
└──────────────┬────────────────────────────────────────────────────────┘
               │ HTTP/WS
┌──────────────┴───────────────┐     ┌──────────────────────────────┐
│  blinky-serial-mcp           │     │  Any HTTP client              │
│  (thin MCP wrapper,          │     │  (curl, blinky-console,       │
│   translates MCP tools       │     │   scripts)                    │
│   to REST API calls)         │     │                               │
└──────────────────────────────┘     └──────────────────────────────┘
```

## Scoring Metrics (Canonical — After Cleanup)

All scoring uses `blinky_server/testing/scoring.py`. **No BPM. No beat F1. No downbeats. No CMLt/CMLc/AMLt.**

### Primary Metrics (Pattern Quality)
| Metric | Description | Range | Good |
|--------|-------------|-------|------|
| **plpAtTransient** | Avg PLP pulse at ground truth onset times | 0-1 | > 0.5 |
| **plpAutoCorr** | PLP autocorrelation at detected period lag | 0-1 | > 0.5 |
| **plpPeakiness** | PLP peak/mean ratio (pattern structure) | 1+ | > 2.0 |

### Secondary Metrics (Onset Accuracy)
| Metric | Description | Range |
|--------|-------------|-------|
| **onsetF1** | Onset detection F1 (100ms tolerance) | 0-1 |
| **onsetF1_50ms** | Onset F1 at tighter 50ms tolerance | 0-1 |
| **onsetF1_70ms** | Onset F1 at 70ms tolerance | 0-1 |
| **onsetF1_150ms** | Onset F1 at relaxed 150ms tolerance | 0-1 |
| **onsetPrecision** | Onset precision | 0-1 |
| **onsetRecall** | Onset recall | 0-1 |

### Diagnostics (Not Scored — For Debugging Only)
| Metric | Description |
|--------|-------------|
| audioLatencyMs | Estimated pipeline latency |
| onsetRate | Detections per second |
| onsetOffsetMs | Median/stddev of onset-to-GT offset |
| plpMean | Average PLP value (0.5 = cosine fallback) |
| avgConfidence | Mean rhythm tracking confidence |

### Removed (Do Not Reintroduce)
| ~~Metric~~ | Why |
|-------------|-----|
| ~~beatF1~~ | Confounds music structure with model quality |
| ~~CMLt/CMLc/AMLt~~ | Beat continuity is meaningless for a visualizer |
| ~~detectedBpm~~ | Just a number. Half/double time is equally valid. |
| ~~phaseStability~~ | Coupled to BPM estimate, misleading |
| ~~downbeatF1~~ | Meter position is irrelevant |
| ~~bpmError~~ | Never existed in server, present in CJS scripts |

## REST API Surface

### Existing Endpoints (rename OTA → firmware)

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/devices` | List all devices with state/platform/transport |
| GET | `/api/devices/{id}` | Single device detail |
| GET | `/api/devices/{id}/settings` | Get device settings |
| GET | `/api/devices/{id}/settings/{category}` | Filter by category |
| PUT | `/api/devices/{id}/settings/{name}` | Set a setting |
| POST | `/api/devices/{id}/settings/save` | Persist to flash |
| POST | `/api/devices/{id}/settings/defaults` | Factory reset |
| POST | `/api/devices/{id}/command` | Send raw command |
| POST | `/api/devices/{id}/stream/{mode}` | Control streaming (on/off/fast/debug) |
| POST | `/api/devices/{id}/flash` | Upload firmware to one device (was `/ota`) |
| POST | `/api/fleet/command` | Broadcast to all devices |
| PUT | `/api/fleet/settings/{name}` | Set on all devices |
| POST | `/api/fleet/flash` | Flash all devices (was `/fleet/ota`) |
| POST | `/api/fleet/deploy` | Compile + flash all |
| POST | `/api/firmware/compile` | Compile firmware (was `/ota/compile`) |
| POST | `/api/firmware/compile-dfu` | Compile + generate DFU package (was `/ota/compile-dfu`) |
| WS | `/ws/{id}` | Device data stream |
| WS | `/ws/fleet` | Multiplexed fleet stream |

### New Testing Endpoints (Phases 3-4)

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/test/validate` | Run validation suite (all tracks, N devices, N runs) |
| POST | `/api/test/param-sweep` | Multi-device parameter sweep |
| GET | `/api/test/jobs` | List recent test jobs |
| GET | `/api/test/jobs/{id}` | Job status/progress/result |
| GET | `/api/test/tracks` | Discover available test tracks |
| GET | `/api/test/audio-lock` | Check audio lock status |
| DELETE | `/api/test/audio-lock` | Force-release stuck audio lock |

All test endpoints return `{job_id}` immediately. Long-running tests execute as async background tasks. Poll `GET /api/test/jobs/{id}` for progress and results.

## Implementation Phases

### Phase 1: Scoring Engine + Audio Foundations — COMPLETE ✅

Stateless library code ported to Python. No server API changes yet.

**Files created:**
- `blinky_server/testing/scoring.py` (618 lines) — Full scoring engine ported from scoring.ts
- `blinky_server/testing/types.py` (149 lines) — Dataclasses
- `blinky_server/testing/audio_lock.py` (121 lines) — Atomic file lock
- `blinky_server/testing/audio_player.py` (101 lines) — Async ffplay wrapper
- `blinky_server/testing/track_discovery.py` (107 lines) — Track + ground truth scanner

### Phase 2: Metric Cleanup + Rename

Strip all beat/BPM metrics from the scoring engine. Rename OTA → firmware. Remove inter-process serial lock.

**scoring.py changes:**
- Delete `BEAT_TOLERANCE_SEC` constant
- Delete beat F1 computation (lines 217-227: ref_beats, est_beats, beat_result)
- Delete CMLt/CMLc computation (lines 252-267)
- Delete AMLt computation (lines 269-297)
- Delete `MusicMode` construction (avg_bpm, detected_bpm — lines 299-303, 438-443)
- Delete beat event offset diagnostics (lines 392-411: beat_event_offsets, beat_offset_stats, beat_offset_histogram, prediction_ratio, beat_vs_reference)
- Delete `adjusted_beat_events` from output
- Rename: `transient_tracking` → `onset_tracking`, field names `f1_at_Xms` → `f1_Xms`
- PLP autocorrelation: replace `avg_bpm` with period-based lag from `music_states` phase data (the device streams period, we don't need to derive it from BPM)
- Keep: `match_events_f1()` (used for onset F1), `estimate_audio_latency()`, `analyze_phase_alignment()`, `_compute_offset_stats()`, PLP metrics, onset-to-GT offset diagnostics

**types.py changes:**
- Delete `BeatEvent` dataclass
- Delete `BeatTracking` dataclass
- Delete `MusicMode` dataclass
- Remove `bpm` field from `GroundTruth`
- Remove `bpm` field from `MusicState`
- Remove `beat_events` from `TestData`
- Remove from `Diagnostics`: `beat_event_rate`, `beat_offset_stats`, `beat_offset_histogram`, `beat_vs_reference`, `prediction_ratio`, `beat_event_offsets`
- Rename `TransientTracking` → `OnsetTracking`
- Remove from `DeviceRunScore`: `beat_tracking`, `music_mode`, `adjusted_beat_events`
- Add to `DeviceRunScore`: `avg_confidence: float`, `activation_ms: float | None` (kept from MusicMode, useful diagnostics)

**API rename:**
- `routes_ota.py` → `routes_firmware.py`
- Router tag: `ota` → `firmware`
- Endpoints: `/ota/compile` → `/firmware/compile`, `/ota/compile-dfu` → `/firmware/compile-dfu`
- `/fleet/ota` → `/fleet/flash`, `/devices/{id}/ota` → `/devices/{id}/flash`
- `OtaRequest` model → `FlashRequest`
- Module: `blinky_server/ota/` → `blinky_server/firmware/`

**Serial lock simplification:**
- Delete `blinky_server/transport/serial_lock.py` (server is sole serial consumer)
- Delete `tools/serial_lock.py` (no external consumers)
- Server disconnects transport before subprocess calls — no lock needed
- Remove `_request_server_release()` HTTP call from uf2_upload.py's server-invoked path

**Verification:** Run existing pytest suite. Confirm scoring output has zero beat/BPM fields.

### Phase 3: Test Session Infrastructure

Add per-device recording buffers so streaming data flows into the scoring engine.

**New file:** `blinky_server/testing/test_session.py`

`TestSession` class attaches to a Device and accumulates:
- `onset_buffer: list[TransientEvent]` — onset detections with timestamp, type, strength
- `music_state_buffer: list[MusicState]` — phase, confidence, plpPulse at each stream frame

Interface:
```python
class TestSession:
    def start_recording(self) -> None
    def stop_recording(self) -> TestData   # Returns frozen snapshot
    def ingest_stream_line(self, data: dict) -> None
```

**Modified files:**
- `device/device.py` — Add `_test_session` field. Route streaming JSON to test session when active.
- `device/manager.py` — Add `start_test_session(device_id)` / `stop_test_session(device_id)`.

**Verification:** Connect to real device, start streaming + recording, play audio, stop recording, score the TestData against ground truth. Confirm onset F1 and PLP metrics are populated.

### Phase 4: Test Runner + REST Endpoints

Core test orchestration — play audio, collect data from N devices, score against ground truth.

**New files:**

`blinky_server/testing/test_runner.py` — Orchestration engine:
```python
async def run_validation(
    fleet, device_ids, *,
    track_dir, track_names, duration_ms, seek_sec, gain, num_runs,
    commands, per_device_commands,
) -> dict

async def run_param_sweep(
    fleet, device_ids, *,
    param_name, values, track_dir, track_names,
    duration_ms, settle_ms, num_runs, gain,
) -> dict
```

Validation orchestration (matching current test-runner.ts):
1. Acquire audio lock
2. Kill orphan ffplay
3. Configure devices (gain lock, pre-commands)
4. Start streaming on all devices (`stream fast`)
5. For each run: start recording → play audio → stop recording → score
6. 5-second inter-run gap
7. Aggregate results (per-device, per-run, suite-level)
8. Release audio lock

Parameter sweep orchestration (matching param_sweep_multidev.cjs):
1. Multi-device batching: assign different parameter values to different devices, play audio once. With 3 devices and 9 values, only 3 audio passes needed.
2. Track manifest seek-to-middle (skip intros, jump to dense regions)
3. 12-second settle time (ACF convergence)
4. Per-value aggregate statistics with optimal value recommendation
5. Phase alignment scoring (% pulses on 8th-note grid)

`blinky_server/testing/job_manager.py` — In-memory async job tracker:
- Jobs: id, status (pending/running/complete/error), progress, result, created_at
- Background task execution via asyncio
- Auto-prune completed jobs after 1 hour

`blinky_server/api/routes_testing.py` — REST endpoints (see table above)

**Verification:** POST /api/test/validate with single device + single track. Confirm PLP scores and onset F1 are populated and reasonable.

### Phase 5: MCP Server → Thin HTTP Client

Rewrite `blinky-serial-mcp/src/index.ts` from direct serial management to HTTP calls against `http://localhost:8420/api/...`.

**Remove (direct serial code):**
- DeviceManager, DeviceSession, BlinkySerial, serial.ts, device-manager.ts
- scoring.ts, audio-lock.ts, track-discovery.ts, test-runner.ts

**Remove (beat/BPM tools):**
- `monitor_music` — was BPM tracking metrics, no replacement needed
- `get_beat_state` — was BPM/phase snapshot, no replacement needed

**Keep (as HTTP wrappers):**
- `list_ports` → GET /api/devices
- `connect` / `disconnect` → no-op or advisory (server manages connections)
- `status` → GET /api/devices/{id}
- `send_command` → POST /api/devices/{id}/command
- `get_settings` / `set_setting` / `save_settings` / `reset_defaults` → existing settings endpoints
- `get_audio` / `stream_start` / `stream_stop` → POST /api/devices/{id}/stream + WS /ws/{id}
- `monitor_audio` → WS /ws/{id} with duration + aggregation
- `run_test` / `run_validation_suite` → POST /api/test/validate
- `run_music_test` / `run_music_test_multi` → POST /api/test/validate
- `check_test_result` → GET /api/test/jobs/{id}
- `list_patterns` → GET /api/test/tracks
- `render_preview` → keep local (simulator is a local C++ binary)
- `get_music_status` → GET /api/devices/{id} (confidence/phase from device state)

All MCP tools continue to work identically from Claude's perspective.

### Phase 6: Delete Everything External

After all phases verified end-to-end:

**Delete (14 CJS test scripts):**
- `ml-training/tools/ab_test_*.cjs` (7 files)
- `ml-training/tools/param_sweep_multidev.cjs`
- `ml-training/tools/config_test_multidev.cjs`
- `ml-training/tools/model_compare_multidev.cjs`
- `ml-training/tools/pattern_memory_test.cjs`
- `ml-training/tools/phase_downbeat_eval.cjs`
- `ml-training/tools/analyze_sweep.cjs`
- `ml-training/tools/analyze_all_sweeps.cjs`

**Delete (MCP server internals):**
- `blinky-serial-mcp/src/lib/scoring.ts`
- `blinky-serial-mcp/src/lib/audio-lock.ts`
- `blinky-serial-mcp/src/lib/track-discovery.ts`
- `blinky-serial-mcp/src/test-runner.ts`
- `blinky-serial-mcp/src/device-session.ts`
- `blinky-serial-mcp/src/serial.ts`
- `blinky-serial-mcp/src/device-manager.ts`

**Delete (standalone Python tools with direct serial):**
- `tools/audio_tuner.py` — unused since Dec 2025, server streams replace it
- `tools/serial_logger.py` — unused since Dec 2025, WS /ws/{id} replaces it
- `tools/run_ble_dfu.py` — server handles BLE DFU via /api/devices/{id}/flash
- `tools/test_ble_dfu.py` — test script, no longer needed
- `tools/test_ble_dfu_notify.py` — test script, no longer needed
- `tools/serial_lock.py` — no external consumers remain

**Delete (test player CLI):**
- `blinky-test-player/src/param-tuner/` (entire directory)
- `blinky-test-player/src/index.ts` (CLI entry point — server handles test execution)
- `blinky-test-player/dist/` (compiled output)
- `blinky-test-player/package.json` scripts that run the CLI

**Move into blinky-server (exact copy, no logic changes):**
- `tools/uf2_upload.py` → `blinky-server/blinky_server/firmware/uf2_upload_cli.py`
  - Battle-tested. Zero modifications to the script logic.
  - Server continues to invoke it as a subprocess (proven pattern).
  - Only change: subprocess path points to new location.

**Delete (replaced by server invocation):**
- `tools/fleet_flash.sh` — replaced by POST /api/fleet/flash

**Move ML capture tools into server:**
- `ml-training/tools/capture_nn_stream.py` → POST /api/devices/{id}/stream/nn + WS /ws/{id}
  - Server already supports streaming modes. Just needs `nn` mode added.
- `ml-training/tools/gain_volume_sweep.py` → absorbed into param sweep infrastructure
  - Multi-device gain sweep is just a parameter sweep over `gain` values.

**Keep (still needed, no serial access):**
- `blinky-test-player/music/` — audio files + ground truth labels
- `blinky-test-player/samples/` — audio samples for synthetic patterns
- `blinky-test-player/src/patterns.ts` — pattern definitions (reference, pre-generate WAVs)
- `ml-training/tools/generate_track_manifest.cjs` — offline utility, no serial
- `ml-training/tools/generate_screening_design.cjs` — offline utility, no serial
- `ml-training/tools/derive_pattern_ground_truth.py` — offline analysis, no serial
- `tools/pre_upload_check.py` — compile-time safety, no serial
- `tools/validate_syntax.py` — compile-time validation, no serial
- `tools/deploy_model.sh` — model export pipeline (calls server API for flash)

**Update docs:** CLAUDE.md, TESTING.md, AUDIO-TUNING-GUIDE.md — reference server API endpoints, remove all beat/BPM terminology.

## uf2_upload.py Migration (Zero Regression)

This script has flashed hundreds of devices without failure. The migration strategy is designed for zero risk:

1. **Copy** `tools/uf2_upload.py` → `blinky-server/blinky_server/firmware/uf2_upload_cli.py` (byte-for-byte identical logic)
2. **Update** the server's subprocess call path (one line change in `firmware/uf2_upload.py` wrapper)
3. **Remove** the `_request_server_release()` HTTP call path — when invoked by the server, the transport is already disconnected. The script's standalone lock acquisition code becomes a no-op (no lock file to contend with).
4. **Test** by flashing a bare test chip via POST /api/devices/{id}/flash. Verify identical behavior.
5. **Test** fleet flash via POST /api/fleet/flash on all blinkyhost devices.
6. **Only then** delete `tools/uf2_upload.py` and `tools/fleet_flash.sh`.

The script retains its `if __name__ == "__main__"` entry point. The server calls it as:
```python
subprocess.run([sys.executable, uf2_cli_path, "--hex", firmware_path, port, "-v"])
```

Same invocation pattern as today. Same stdout parsing. Same timeout handling.

## Serial Lock Elimination

**Current state:** Three lock mechanisms compete:
- `tools/serial_lock.py` — file-based (`/tmp/blinky-serial/*.lock`)
- `blinky-server/transport/serial_lock.py` — re-exports tools version
- `blinky-serial-mcp` — JS equivalent of the same lock

**After consolidation:** The server is the sole serial consumer. All port access is serialized by the asyncio event loop. No inter-process coordination needed.

- Server connects/disconnects transports directly (already does this)
- Before firmware upload: server disconnects transport, spawns subprocess, subprocess has exclusive access, server reconnects after completion
- No file locks, no HTTP release requests, no stale PID cleanup

The `/tmp/blinky-serial/` directory and all lock files are eliminated entirely.

## Implementation Order

```
Phase 1 ✅ ──→ Phase 2 ──→ Phase 3 ──→ Phase 4 ──→ Phase 5 ──→ Phase 6
```

Linear critical path. Each phase builds on the previous. Phase 6 (deletion) only happens after end-to-end verification of all preceding phases.

Estimated scope: ~2000 lines new Python, ~500 lines tests. Scoring shrinks by ~200 lines. MCP server shrinks by ~1600 lines. 14 CJS scripts + 6 Python tools + fleet_flash.sh deleted.
