# Server Consolidation Plan

*Created: April 1, 2026*

## Problem

Three separate systems manage serial connections, compute metrics, and run tests independently:

| System | Language | Manages serial? | Scoring | PLP metrics |
|--------|----------|----------------|---------|-------------|
| **blinky-server** | Python/FastAPI | Yes (fleet manager) | None | None |
| **blinky-serial-mcp** | TypeScript/MCP | Yes (direct serial) | Canonical (scoring.ts) | Yes |
| **ml-training/tools/*.cjs** | Node.js CLI scripts | Yes (direct serial) | Inline per-script (BPM-based, STALE) | No |

This causes:
- **Port contention**: Server and test tools race for serial ports. The server silently consumes streaming data, causing tests to collect 0 samples.
- **Stale metrics**: 8 CJS scripts still compute BPM error (removed from canonical scoring months ago). PLP metrics (plpAtTransient, plpAutoCorr, plpPeakiness) exist only in the MCP server — no CLI tool can compute them.
- **Duplicated scoring**: Greedy F1 matching reimplemented 3 times with different tolerances. Latency estimation has 2 different algorithms.
- **No single source of truth**: Changes to scoring methodology require updating 3 codebases.

## Goal

**blinky-server becomes the single owner of all device connections, test orchestration, scoring, and results.** The MCP server becomes a thin HTTP client. All CJS test scripts are deleted.

## Architecture After Consolidation

```
┌──────────────────────────────────────────────────────────────────────┐
│                          blinky-server                               │
│                    (Python/FastAPI, port 8420)                        │
│                                                                      │
│  ┌──────────────────┐  ┌───────────────────┐  ┌──────────────────┐  │
│  │  Device Manager   │  │  Test Runner       │  │  Scoring Engine  │  │
│  │  (fleet mgmt,     │  │  (orchestration,   │  │  (canonical,     │  │
│  │   discovery,      │  │   audio playback,  │  │   PLP metrics,   │  │
│  │   reconnect,      │  │   data collection, │  │   F1 matching,   │  │
│  │   dedup)          │  │   job management)  │  │   latency est.)  │  │
│  └──────┬───────────┘  └──────┬────────────┘  └──────────────────┘  │
│         │                     │                                      │
│  ┌──────┴──────────────────────┴──────────────────────────────────┐  │
│  │                    Transport Layer                              │  │
│  │  Serial (pyserial-asyncio) │ BLE (bleak) │ WiFi (TCP socket)  │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  REST API: /api/devices, /api/test, /api/fleet, /api/ota            │
│  WebSocket: /ws/{device_id}, /ws/fleet                              │
└──────────────┬───────────────────────────────────────────────────────┘
               │ HTTP/WS
┌──────────────┴───────────────┐     ┌──────────────────────────────┐
│  blinky-serial-mcp           │     │  Any HTTP client              │
│  (thin MCP wrapper,          │     │  (curl, web UI, scripts)      │
│   translates MCP tools       │     │                               │
│   to REST API calls)         │     │                               │
└──────────────────────────────┘     └──────────────────────────────┘
```

## REST API Surface (After Consolidation)

### Existing Endpoints (already in blinky-server)

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/devices` | List all devices with state/platform/transport |
| GET | `/api/devices/{id}` | Single device detail |
| GET | `/api/devices/{id}/settings` | Get device settings |
| PUT | `/api/devices/{id}/settings/{name}` | Set a setting |
| POST | `/api/devices/{id}/settings/save` | Persist to flash |
| POST | `/api/devices/{id}/settings/defaults` | Factory reset |
| POST | `/api/devices/{id}/command` | Send raw command |
| POST | `/api/devices/{id}/stream/{mode}` | Control streaming (on/off/fast/debug) |
| POST | `/api/devices/{id}/ota` | Upload firmware (UF2 > BLE DFU) |
| POST | `/api/fleet/command` | Broadcast to all devices |
| PUT | `/api/fleet/settings/{name}` | Set on all devices |
| POST | `/api/fleet/ota` | Flash all devices |
| POST | `/api/fleet/deploy` | Compile + flash all |
| WS | `/ws/{id}` | Device data stream |
| WS | `/ws/fleet` | Multiplexed fleet stream |

### New Testing Endpoints (Phases 2-4)

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/test/run-track` | Run single track test (N devices, N runs) |
| POST | `/api/test/validate` | Full validation suite (all tracks, all devices) |
| POST | `/api/test/param-sweep` | Multi-device parameter sweep |
| POST | `/api/test/ab-test` | A/B comparison between two settings |
| POST | `/api/test/pattern-memory` | Pattern slot cache testing |
| POST | `/api/test/run-pattern` | Synthetic pattern test (calibrated drums) |
| POST | `/api/test/start-session` | Start raw recording on device |
| POST | `/api/test/stop-session` | Stop recording, return raw buffers |
| POST | `/api/test/score` | Ad-hoc scoring of externally collected data |
| POST | `/api/test/tune-ensemble` | Binary search threshold optimization |
| GET | `/api/test/jobs` | List recent test jobs |
| GET | `/api/test/jobs/{id}` | Job status/progress/result |
| GET | `/api/test/tracks` | Discover available test tracks |
| GET | `/api/test/patterns` | List synthetic test patterns |
| GET | `/api/test/audio-lock` | Check audio lock status |
| DELETE | `/api/test/audio-lock` | Force-release stuck audio lock |

All test endpoints return `{job_id}` immediately. Long-running tests execute as async background tasks. Poll `GET /api/test/jobs/{id}` for progress and results.

## Implementation Phases

### Phase 1: Scoring Engine + Audio Foundations — COMPLETE ✅

Stateless library code ported to Python. No server API changes.

**Files created:**
- `blinky_server/testing/scoring.py` (583 lines) — Full scoring engine ported from scoring.ts with JS-compatible rounding
- `blinky_server/testing/types.py` (147 lines) — Dataclasses: GroundTruth, DeviceRunScore, TestData, etc.
- `blinky_server/testing/audio_lock.py` (119 lines) — Atomic file lock at `/tmp/blinky-audio.lock`
- `blinky_server/testing/audio_player.py` (99 lines) — Async ffplay subprocess wrapper
- `blinky_server/testing/track_discovery.py` (105 lines) — Scan for audio + ground truth pairs

**Scoring functions ported:**
- `match_events_f1()` — Greedy nearest-neighbor F1 matching
- `estimate_audio_latency()` — Histogram-peak latency estimation (10ms buckets, -50..300ms bounds)
- `score_device_run()` — Full scoring: beat F1/CMLt/CMLc/AMLt, transient multi-tolerance F1 (50/70/100/150ms), PLP metrics (atTransient/autoCorr/peakiness), music mode (confidence, phase stability), diagnostics
- `format_score_summary()` — Compact JSON for API responses
- `analyze_phase_alignment()` — % pulses on 8th-note grid subdivisions (from param_sweep_multidev.cjs)

**Review completed:** 6 bugs found and fixed (JS Math.round vs Python round divergence, integer vs float division, errno.ESRCH specificity, missing oss field).

### Phase 2: Test Session Infrastructure

Add per-device recording buffers so streaming data flows into the scoring engine.

**New file:** `blinky_server/testing/test_session.py`

`TestSession` class attaches to a Device and accumulates:
- `transient_buffer: list[TransientEvent]` — onset detections with timestamp, type, strength
- `music_state_buffer: list[MusicState]` — bpm, phase, confidence, plpPulse at each stream frame
- `beat_event_buffer: list[BeatEvent]` — beat events (music.q == 1)

Interface:
```python
class TestSession:
    def start_recording(self) -> None
    def stop_recording(self) -> TestData   # Returns frozen snapshot
    def ingest_stream_line(self, data: dict) -> None  # Called by Device._route_stream_line
```

**Modified files:**
- `device/device.py` — Add `_test_session` field. Route streaming JSON to test session when active. Must handle music state format (`{"m": {...}}`) which current code doesn't classify separately from audio.
- `device/manager.py` — Add `start_test_session(device_id)` / `stop_test_session(device_id)` methods.

**Verification:** Connect to real device, start streaming + recording, play audio, stop recording, score the TestData against ground truth. Compare output to TypeScript pipeline.

### Phase 3: Test Runner + REST Endpoints

Core test orchestration — play audio, collect data from N devices, score against ground truth.

**New files:**

`blinky_server/testing/test_runner.py` — Orchestration engine:
```python
async def run_track(
    fleet, device_ids, audio_file, ground_truth_file,
    *, duration_ms, seek_sec, gain, commands, per_device_commands, num_runs
) -> dict

async def run_validation_suite(
    fleet, device_ids,
    *, track_dir, track_names, duration_ms, num_runs, gain, commands, per_device_commands
) -> dict
```

Orchestration steps (matching test-runner.ts exactly):
1. Acquire audio lock
2. Kill orphan ffplay
3. Configure devices (gain lock, pre-commands, per-device commands)
4. Start streaming on all devices (`stream fast`)
5. For each run: start recording → play audio → stop recording → score
6. 5-second inter-run gap
7. Aggregate results (per-device, per-run, suite-level)
8. Unlock gain, release audio lock

`blinky_server/testing/job_manager.py` — In-memory job tracker:
- Jobs have: id, status (pending/running/complete/error), progress, result, created_at
- Background task execution via asyncio
- Auto-prune completed jobs after 1 hour

`blinky_server/api/routes_testing.py` — REST endpoints (see table above)

**Verification:** POST /api/test/run-track with single device + single track, verify PLP scores match TypeScript pipeline output.

### Phase 4: Advanced Test Tools

Port the specialized test methodologies from CJS scripts.

**Parameter sweep** (`testing/param_sweep.py`, port of `param_sweep_multidev.cjs`):
- Multi-device batching: assign different parameter values to different devices, play audio once. With 3 devices and 9 values, only 3 audio passes needed.
- Track manifest seek-to-middle strategy (skip intros, jump to dense beat regions)
- 12-second settle time (OSS buffer fill + ACF convergence + Bayesian stabilization)
- Phase alignment scoring (% pulses on 8th-note grid)
- Per-value aggregate statistics, monotonicity detection, optimal value recommendation

**A/B test** (`testing/ab_test.py`, port of `ab_test_multidev.cjs`):
- Baseline → test → reset alternation for each track
- Per-device + cross-device aggregated statistics
- Statistical winner determination with octave error tracking

**Pattern memory test** (`testing/pattern_memory.py`, port of `pattern_memory_test.cjs`):
- Cold-start detection: bars until active slot confidence > 0.3
- Fill tolerance: minimum confidence during drum fill windows
- Cache save/restore: slot valid count tracking
- Slot stability: % snapshots with same active slot in steady sections
- Dual telemetry: streaming data + periodic `json slots` command polling

### Phase 5: MCP Server → Thin HTTP Client

Rewrite `blinky-serial-mcp/src/index.ts` from direct serial management to HTTP calls against `http://localhost:8420/api/...`. Shrinks from 2122 to ~500 lines.

**Key changes:**
- Remove: DeviceManager, DeviceSession, BlinkySerial, serial.ts, device-manager.ts
- Remove: scoring.ts, audio-lock.ts, track-discovery.ts, test-runner.ts
- Keep: MCP tool definitions (same interface contract for Claude)
- Add: HTTP client helpers, WebSocket client for monitor_* tools

All 30 MCP tools continue to work identically from Claude's perspective. The implementation changes from "open serial port and send bytes" to "HTTP POST to server API."

### Phase 6: Synthetic Patterns + Ensemble Tuning

Port 20 calibrated drum patterns from `blinky-test-player/src/patterns.ts` (2487 lines) and binary-search threshold optimizer from `fast-tune.ts` (316 lines).

Pre-generate WAV files from pattern definitions to play via ffplay (eliminates Node.js dependency for audio generation).

### Phase 7: Delete External Scripts

After all phases verified end-to-end, delete:

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

**Delete (param-tuner):**
- `blinky-test-player/src/param-tuner/` (entire directory)

**Keep (still needed):**
- `blinky-serial-mcp/src/index.ts` — thin MCP wrapper
- `blinky-test-player/music/` — audio files + ground truth labels
- `blinky-test-player/samples/` — audio samples for synthetic patterns
- `tools/uf2_upload.py` — standalone UF2 tool (called by server as subprocess)
- `tools/fleet_flash.sh` — convenience wrapper
- `ml-training/tools/generate_track_manifest.cjs` — utility
- `ml-training/tools/generate_screening_design.cjs` — utility

**Update docs:** CLAUDE.md, TESTING.md, AUDIO-TUNING-GUIDE.md — reference server API endpoints.

## Scoring Metrics (Canonical, After Consolidation)

All scoring uses `blinky_server/testing/scoring.py`. No BPM accuracy metrics anywhere.

### Primary Metrics (Pattern Quality)
| Metric | Description | Range | Good |
|--------|-------------|-------|------|
| **plpAtTransient** | Avg PLP pulse at ground truth onset times | 0-1 | > 0.5 |
| **plpAutoCorr** | Autocorrelation at detected period lag | 0-1 | > 0.5 |
| **plpPeakiness** | Peak/mean ratio of PLP pattern | 1+ | > 2.0 |

### Secondary Metrics (Detection Quality)
| Metric | Description | Range |
|--------|-------------|-------|
| transientF1 | Onset detection F1 (100ms tolerance) | 0-1 |
| transientF1_at_50ms/70ms | Tighter tolerance variants | 0-1 |
| phaseStability | Phase consistency (1 - circular std × 10) | 0-1 |
| avgConfidence | Mean music mode confidence | 0-1 |
| phaseAlignment | % pulses on 8th-note grid | 0-100% |

### Informational (Not Scored)
| Metric | Description |
|--------|-------------|
| detectedBpm | Average detected BPM (informational only) |
| audioLatencyMs | Estimated pipeline latency |
| beatF1/CMLt/CMLc/AMLt | Beat tracking continuity (for debugging) |

## Implementation Order

```
Phase 1 ✅ ──→ Phase 2 ──→ Phase 3 ──→ Phase 4 ──→ Phase 7
                                    ──→ Phase 5 ──→ Phase 7
                                    ──→ Phase 6 ──→ Phase 7
```

Critical path: Phase 1 → 2 → 3. Phases 4, 5, 6 can be parallelized after Phase 3. Phase 7 is the final cleanup after everything is verified end-to-end.

Total: ~3500 lines new Python, ~1000 lines tests. MCP server shrinks by ~1600 lines. 14 CJS scripts deleted.
