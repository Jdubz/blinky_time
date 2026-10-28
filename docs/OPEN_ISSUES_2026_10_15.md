# Open Issues — 2026-10-15

Every finding from the 2026-10-14/15 reviews that is not fully closed. Sources:
- firmware drift/responsiveness review, plus its spectral/mic and generators/main-loop sub-reviews;
- brick-risk review (hat is the only test device);
- phone record-mode reviews;
- Copilot review of PR #152;
- CI/tooling issues hit along the way;
- issues filed since (release gate, animation timing, auto-recovery image).

Last synced with GitHub: 2026-10-24. Closed since the last sync, and removed below: #160 (`deploy_local.py` reads link and bootloader live), #164, #177 and #185 (verified on hat_v1, 2026-10-21), #280 (SuperFlux + online picker, PR #292).

Every item is filed as a GitHub issue (linked in the `#` column). **The issues are the system of record**: update and close them there, and keep this file as the index. Remove an item here once its issue is closed.

**Status key:**

| Status | Meaning |
|---|---|
| OPEN | not addressed |
| PARTIAL | mitigated, root cause remains |
| IN-PR | implemented in an unmerged PR |
| VERIFY | merged and in a stable release, not yet verified on a device |
| DECIDE | needs a decision |

PR #152 and PR #155 are merged and ship in stable **b215**, which was installed on hat_v1 on 2026-10-20 through the web updater. Three of their device checks passed on 2026-10-21 (#164, #177, #185); the rest of §7 hasn't been run. Read the hat's version live before starting them.

PR #292 (SuperFlux onsets, online picker, onset-driven pulse; #280) is on master, not in a stable release. Its device checks are listed in §9.

---

## 0. Blocking / next actions

| # | Item | Status | Action |
|---|---|---|---|
| 0.4 ([#161](https://github.com/Jdubz/blinky_time/issues/161)) | **PR #152 needs device verification.** See the checklist in §7. | VERIFY | b215 carries it. Run §7 on the hat, observing the 60 s rule. |

## 1. Rhythm tracking / prediction (AudioTracker)

| # | Issue | Location | Status | Notes / proposed fix |
|---|---|---|---|---|
| 1.2 ([#165](https://github.com/Jdubz/blinky_time/issues/165)) | NN inference only on the newest frame of a catch-up burst (CPU guard). Below 62.5 fps, onsets only in skipped frames can be missed. | `AudioTracker::update` | PARTIAL | The window gets every frame. Full fix: a cheaper model (1.11) or the audio path in its own task. |
| 1.3 ([#166](https://github.com/Jdubz/blinky_time/issues/166)) | Wrong tempo multiple on syncopated content (DnB ~170 BPM, UK garage); 6–8-beat cycles from 1.5×/2× beat-lag peaks | `runAcf` candidate selection | PARTIAL | Tempo prior (0.8 oct) lifted valid-tempo time 0.60 → 0.67; syncopation-derived bar candidates (beats from 1.5/2.5-beat ACF peaks, scored at the bar lag) lift it to 0.73: dnb-liquid 0.00 → 0.83, garage 0.06 → 0.47. Remaining: reggaeton regressed 0.81 → 0.64 (3+3+2 dembow), dnb-energetic 0.30, breakbeat-background 0.30. Duple-support scoring (ACF at 2b/4b) was tried and did not help. |
| 1.4 ([#167](https://github.com/Jdubz/blinky_time/issues/167)) | Phase lock near the noise floor on dubstep / garage / breakbeat / trap | tracker | OPEN | Tied to 1.3. Measure with `tests/tracker` `excess`. |
| 1.5 ([#168](https://github.com/Jdubz/blinky_time/issues/168)) | 174 BPM tracked as a 2-beat cycle (octave ambiguity) | `runAcf` | OPEN | Acceptable for visuals but limits `beatCount` meaning; revisit with 1.3. |
| 1.8 ([#171](https://github.com/Jdubz/blinky_time/issues/171)) | JSON stream formatting costs 10–50 ms per emit (loop stall while streaming) | `SerialConsole::streamTick` | OPEN | Rate-limit to a CPU budget or move off the loop task. |
| 1.9 ([#172](https://github.com/Jdubz/blinky_time/issues/172)) | Silence reset / confidence gating used relative level | `AudioTracker` | VERIFY | Silence detection removed with the noise gate. At device-like levels on edm/, an adaptive noise floor falsely reset tempo 487-2159 times in music; never resetting gave 0 false resets and re-locked on a new song as fast or faster. Breakdowns can no longer reset tempo. |
| 1.11 ([#174](https://github.com/Jdubz/blinky_time/issues/174)) | NN computes 16 output frames to use 1 (receptive field 9) | model export | OPEN (ML) | Re-export with W=9 or streaming conv state → ~½ inference cost. Prerequisite for turning the NN back on: it must beat SuperFlux (drum F 0.657 at device level, `docs/AUDIO_ARCHITECTURE.md`) at a frame rate the renderer accepts, in the harness cost model. |

## 2. Spectral / mic / features

| # | Issue | Location | Status | Notes / proposed fix |
|---|---|---|---|---|
| 2.1 ([#175](https://github.com/Jdubz/blinky_time/issues/175)) | Raw mel saturates (band pinned at 1.0) above ~−37 dBFS bass; NN loses low-band contrast at high SPL | `SharedSpectralAnalysis::computeMelBandsFrom` | PARTIAL | Telemetry added (`melSaturated`/`melTotal` in `json info`). Fix needs digital pre-gain or PCEN **mirrored in training**; use field recordings. |
| 2.4 ([#178](https://github.com/Jdubz/blinky_time/issues/178)) | Non-overlapping Hamming frames → onset-strength wobble with hit position | FFT framing | OPEN (ML) | 50 % overlap requires a retrain. |
| 2.6 ([#180](https://github.com/Jdubz/blinky_time/issues/180)) | Mel whitening gives little contrast (log-domain ÷ running max) | `whitenMelBands` | OPEN | Low priority; comment corrected in #152. |
| 2.7 ([#181](https://github.com/Jdubz/blinky_time/issues/181)) | Noise estimator creeps in steady music (leaky minimum) | `estimateAndSubtractNoise` | OPEN | Off by default and not persisted. Keep off, or switch to windowed min-statistics. |
| 2.10 ([#184](https://github.com/Jdubz/blinky_time/issues/184)) | Mic ring overrun splices a window → spurious flux spike | `AdaptiveMic`, `SharedSpectralAnalysis` | VERIFY | `markDiscontinuity()`. |

## 3. Device safety / BLE

| # | Issue | Location | Status | Notes / proposed fix |
|---|---|---|---|---|
| 3.5 ([#189](https://github.com/Jdubz/blinky_time/issues/189)) | During record sessions the HVN queue is shared with NUS; NUS text output can block the loop up to 100 ms per chunk | `BleNus::drainTxBuffer` | OPEN | Keep console quiet during sessions; consider a non-blocking notify check. |
| 3.6 ([#190](https://github.com/Jdubz/blinky_time/issues/190)) | Fleet scanner gets less radio time while a central is connected (event length 125 ms) | SoftDevice config | OPEN | Measure broadcast reception with a phone connected. |

## 4. Visual generators

| # | Issue | Location | Status | Notes |
|---|---|---|---|---|
| 4.1 ([#192](https://github.com/Jdubz/blinky_time/issues/192)) | Fire burst re-arm, particle pool exhaustion, fps-dependent aging/decays, Water beat sweep, phase-threshold beats | generators, `ParticleGenerator` | VERIFY | `onset`/`beatCount` events, `killOldest`, stochastic rounding, dt scaling. Verify visually on the hat (linear layout). |
| 4.2 ([#193](https://github.com/Jdubz/blinky_time/issues/193)) | LED `show()` DMA wait caps fps on large strips (~30 µs/LED) | `Nrf52PwmLedStrip` | OPEN | Not a stall; relevant to the ≥30 fps gate on 1000+ LED devices. |

## 5. Phone record mode

| # | Issue | Status | Notes |
|---|---|---|---|
| 5.1 ([#195](https://github.com/Jdubz/blinky_time/issues/195)) | End-to-end test on Android Chrome: 30 min, 1 m and 10 m, 0 audio gaps, fps within 5 %, FFT overruns unchanged | OPEN | Bench gate in `PHONE_RECORD_MODE_PLAN.md`. |
| 5.2 ([#196](https://github.com/Jdubz/blinky_time/issues/196)) | Behaviour when the screen locks / tab is backgrounded | OPEN | Wake Lock is used; verify Chrome keeps notifications flowing. |
| 5.5 ([#243](https://github.com/Jdubz/blinky_time/issues/243)) | Re-scope record mode as live calibration-data capture, not audio recording | OPEN | Must use the visualizer's exact live settings; analysis frames become the primary payload, PCM optional at most. |
| 5.4 ([#198](https://github.com/Jdubz/blinky_time/issues/198)) | Copilot review findings (10) | VERIFY | All addressed in `0640b435`; verify the ambiguous-start / unsaved-tail / navigation paths on a phone. |

## 6. Tooling / CI / repo hygiene

| # | Issue | Status | Notes |
|---|---|---|---|
| 6.6 ([#258](https://github.com/Jdubz/blinky_time/issues/258)) | Release gate needs timing checks, not just an fps floor | PARTIAL | Floor 60 (#262); `frametime` check on `maxFrameMs` added. Still to do: phase keeps pace with the wall clock. |

## 7. Device verification checklist for PR #152 (hat)

Steps 2 and 3 (BLE boot, MTU 247) and the tempo/mic parts of step 5 passed on 2026-10-21 (#164, #177, #185). Still to run: steps 3 (fleet reach, `device upload` round-trip), 4 and the Fire part of 5.

1. Read the hat's version live (`json info`). At b215 or later, no flash is needed. Otherwise flash the latest stable release with `./scripts/deploy.sh --devices=<hat>`. Either way, don't reset it again for at least 60 s.
2. Boot log: no `[FALLBACK] Bluefruit.begin() FAILED`; `[BLE] Record service started` present.
3. Fleet server still reaches the hat over BLE; `ble` reports MTU 247; a `device upload` round-trip works (244-byte chunks, 1024-byte RX FIFO).
4. `json info`:
   - fps ≥ 30;
   - `audioOverruns` not climbing;
   - `spectralAccumOverflow` = 0;
   - `clipSamples` / `melSaturated` plausible;
   - `loopStackFree` and `cmdStackFree` (send `json info` over both USB and BLE) ≥ 512 B, `recStackFree` ≥ 512 B after a record session, `heapPeak` well under `heapTotal`.
5. Visual: tempo lock holds through a breakdown → drop. Fire bursts continue through dense hats. No particle starvation on the hat's linear layout.

## 8. Carried over from 2026-05-20

The still-open items of `docs/archive/OPEN_ISSUES_2026_05_20.md` (fleet rollout, SWD recovery, BLE-DFU tooling), filed by #204. That doc was five months old at filing, so confirm each item is still open before starting on it.

| # | Item | Status |
|---|---|---|
| 8.1 ([#209](https://github.com/Jdubz/blinky_time/issues/209)) | Pi `recover.sh` still needs the manual BCM 18 hold + release | OPEN |
| 8.2 ([#210](https://github.com/Jdubz/blinky_time/issues/210)) | 06ACEB recovery unfinished | OPEN |
| 8.3 ([#211](https://github.com/Jdubz/blinky_time/issues/211)) | false-negative exit 4 after a successful BL flash | OPEN |
| 8.4 ([#212](https://github.com/Jdubz/blinky_time/issues/212)) | server re-grabs the port between BL and app flash | OPEN |
| 8.7 ([#215](https://github.com/Jdubz/blinky_time/issues/215)) | all-BLE deploy.sh happy path never run to exit 0 | OPEN |
| 8.8 ([#216](https://github.com/Jdubz/blinky_time/issues/216)) | BLE-DFU stuck at MTU 23 | OPEN |
| 8.9 ([#217](https://github.com/Jdubz/blinky_time/issues/217)) | lemon-cart install.sh not run | OPEN |
| 8.11 ([#219](https://github.com/Jdubz/blinky_time/issues/219)) | auto-recovery untested on an enrolled device | OPEN |
| 8.12 ([#220](https://github.com/Jdubz/blinky_time/issues/220)) | no BL-over-BLE path (future) | OPEN |
| 8.13 ([#221](https://github.com/Jdubz/blinky_time/issues/221)) | full-fleet gossip-ACK delivery unmeasured | OPEN |

## 9. Onset detection, labels and ML (filed 2026-10-22/23)

The detection task is kick and snare hits (CLAUDE.md). Detectors are scored on drum metrics at the device's level (`docs/AUDIO_ARCHITECTURE.md`).

| # | Issue | Status | Notes |
|---|---|---|---|
| 9.1 | PR #292 device checks (#280 follow-up) | VERIFY | Owner listening check on the hat and a `pulsedecay` choice (default 150 ms); a `fluxdelta` sweep on the device (kick recall 53% on the hat vs 72% on the host, same segments). Flash with `tools/deploy_local.py --dev` (test device only). |
| 9.2 ([#284](https://github.com/Jdubz/blinky_time/issues/284)) | Drum labels ~26 ms late, kick/snare swaps, phantom snares | OPEN | Poisons NN targets and every drum metric. Snare recall numbers are provisional until fixed. |
| 9.3 ([#289](https://github.com/Jdubz/blinky_time/issues/289)) | Hand-curate GT for the 18 edm/ tracks | OPEN (owner) | `tools/label-reviewer/` is ready; no `*.onsets_human.json` exists yet. |
| 9.4 ([#290](https://github.com/Jdubz/blinky_time/issues/290)) | Shift-tolerant loss; retire label widening | OPEN (ML) | In blinky's own trainer: my-spark plan 0239 deletes the platform's audio trainer; training runs as blinky's code via `gpu_task`. |
| 9.5 ([#291](https://github.com/Jdubz/blinky_time/issues/291)) | Publish the mel feature spec as a versioned contract with golden vectors | PARTIAL | Now blinky-side: training is blinky's code again (my-spark plan 0239), so this is training building its front end from the firmware's integer `MEL_BANDS` table (weighted average per band, not an HTK filterbank), plus golden vectors. The spec and golden vectors now exist (`ml-training/spec/`, #295); what's left is any spec change from #175/#178, as a new version. |
| 9.6 ([#288](https://github.com/Jdubz/blinky_time/issues/288)) | Spark `separate` job client | PARTIAL | blinky-server owns the client (`blinky_server/spark/`, `/api/spark/*`): it submits, records outcomes from the Spark's webhook with an hourly poll as backstop, and keeps stems in the Spark's object store (`blinky/stems/<model_type>/`, never reaped). `spark_jobs.py` is its CLI. Follows my-spark #1534's contract: `chunk_seconds` omitted by default (htdemucs's 7.8 s segment is fixed), htdemucs only. The platform side is live (#1530, #1534: htdemucs verified on the GB10). Registered 2026-10-27: `blinky-time` is in the deployed `clients.json` (`max_priority: 0`, `events: []`, so no webhooks; the client polls). A smoke-test `echo` job confirmed the key resolves and priority clamps to 0. Key and API URL are in `~/.config/blinky/spark.env` on the test box (mode 600, not in the repo). Next: the first htdemucs run over the 18 edm/ tracks. `mel-band-roformer` isn't in the engine yet, so Phase 1's two-separator comparison waits. |
| 9.7 ([#283](https://github.com/Jdubz/blinky_time/issues/283)) | Visualizer observability in the simulator | OPEN | Numeric summaries of visual impact, so tuning isn't gated on the owner watching the hat. |
| 9.8 ([#294](https://github.com/Jdubz/blinky_time/issues/294)) | The drum metrics (kick/snare recall, on-drum share, drum F at device level) are computed by ad-hoc study scripts, not by anything in the repo; CI and blinky-server still score all-onset F1 | FIXED | Drum scoring lives in `blinky_server/testing/drum_scoring.py` and feeds both `tests/tracker/run_tracker_eval.py` and blinky-server's device runs and param-sweep summary. CI gates on kick R@50 ≥ 0.77, on-drum ≥ 0.56 and drum F ≥ 0.62 at −50 dB (master baseline 0.789 / 0.583 / 0.638). Snare recall is reported, provisional until #284. |
| 9.9 ([#295](https://github.com/Jdubz/blinky_time/issues/295)) | Training's librosa HTK filterbank isn't the firmware's integer `MEL_BANDS` table (weights differ by up to 0.30; 49/50 rows share bin support); no mel parity test in CI | FIXED | Training builds its mel from the firmware's `MEL_BANDS` table (`ml-training/spec/mel_frontend_v1.json`, `scripts/mel_frontend.py`; `audio.mel_filterbank: firmware_table` in base.yaml, `librosa_htk` only to reproduce old runs). CI's Feature Parity job builds the firmware front end and checks the spec and golden frames (`analysis/test_mel_parity.py`). Measured on edm/ with the deployed model: no change (onset F1 0.609 either way, kick R 0.844 vs 0.846), so the next retrain is the first that can differ. |
