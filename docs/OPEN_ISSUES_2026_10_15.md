# Open Issues — 2026-10-15

Every finding from the 2026-10-14/15 reviews that is not fully closed. Sources:
- firmware drift/responsiveness review, plus its spectral/mic and generators/main-loop sub-reviews;
- brick-risk review (hat is the only test device);
- phone record-mode reviews;
- Copilot review of PR #152;
- CI/tooling issues hit along the way.

Remove an item once it is verified on hardware or decided. Don't let it become history.

**Status key:**

| Status | Meaning |
|---|---|
| OPEN | not addressed |
| PARTIAL | mitigated, root cause remains |
| IN-PR | implemented in an unmerged PR, not yet verified on a device |
| DECIDE | needs a decision |

Nothing in PR #152 / #155 has run on hardware yet.

---

## 0. Blocking / next actions

| # | Item | Status | Action |
|---|---|---|---|
| 0.1 | **Claude CI review can't authenticate.** `ANTHROPIC_API_KEY` secret returns 401 "Invalid API key"; the older `CLAUDE_CODE_OAUTH_TOKEN` returns 401 "OAuth token revoked". The workflow (#153/#157) itself is correct. | OPEN | Replace the secret: `gh secret set ANTHROPIC_API_KEY -R Jdubz/blinky_time`, then push to #152 to trigger a review. |
| 0.2 | **Temporary `show_full_output: true`** is on `master` in `.github/workflows/claude-code-review.yml` (public logs). | OPEN | Remove after the first successful review. Like any change to that file, the removal only takes effect once merged to `master`. |
| 0.3 | **Hat flash prerequisites unknown.** Is the hat's link USB or BLE-only? Which bootloader (0.8.0-10 or 0.8.0-7)? | DECIDE | Answer before `./scripts/deploy.sh --devices=<hat>`. A BLE-only link makes the BLE-stack change (§3.1) the one change that could cost reachability. |
| 0.4 | **PR #152 needs device verification.** See the checklist in §7. | IN-PR | Flash, observe the 60 s rule, verify. |
| 0.5 | **The onset NN has been disabled on every device since 2026-05-13.** `frame_onset_model_data.h` = v36 (80 mel) vs 50-mel firmware → `FrameOnsetNN` initError 5. | IN-PR #155 | Merge #155 (restores v33) after #152 is flashed and verified, then flash separately and check `show nn` and fps. |
| 0.6 | **`tools/check_model_input.py` not wired into CI** (added in #155). | OPEN | Add a CI step (`pip install tflite`) so a width-mismatched model fails the PR. |

## 1. Rhythm tracking / prediction (AudioTracker)

| # | Issue | Location | Status | Notes / proposed fix |
|---|---|---|---|---|
| 1.1 | Phase advanced per loop iteration; pattern read against a rotating fold origin; frames skipped in catch-up; `OSS_FRAME_RATE` 66; integer period; peak-picker duplicate samples | `AudioTracker.cpp` | IN-PR #152 | Frame clock, fold-measured phase, fractional period (long-lag unbiased ACF), accent from OSS. Harness: alignment −0.014 → +0.096, tempo stable across loop rates. Verify on device. |
| 1.2 | NN inference only on the newest frame of a catch-up burst (CPU guard). Below 62.5 fps, onsets only in skipped frames can be missed. | `AudioTracker::update` | PARTIAL | The window gets every frame. Full fix: a cheaper model (1.11) or the audio path in its own task. |
| 1.3 | Wrong tempo multiple on syncopated content (DnB ~170 BPM, UK garage); 6–8-beat cycles from 1.5×/2× beat-lag peaks | `runAcf` candidate selection | PARTIAL | Tempo prior (0.8 oct) lifted valid-tempo time 0.60 → 0.67. dnb-liquid ~0.0 and garage ~0.05 remain. Consider half-lag ACF checks and metrical-consistency scoring. |
| 1.4 | Phase lock near the noise floor on dubstep / garage / breakbeat / trap | tracker | OPEN | Tied to 1.3. Measure with `tests/tracker` `excess`. |
| 1.5 | 174 BPM tracked as a 2-beat cycle (octave ambiguity) | `runAcf` | OPEN | Acceptable for visuals but limits `beatCount` meaning; revisit with 1.3. |
| 1.6 | "3-tap Hamming" NN smoothing is actually asymmetric 0.77/0.23 | `processFrame` | DECIDE | Kept on purpose (`pulseOnsetFloor` tuned against it). A symmetric filter needs a retune sweep. |
| 1.7 | `stream nn` debug stream emits only the last frame of each update | `SerialConsole::streamTick` | OPEN | Use a per-frame hook (needs a second hook slot), or note gaps via the `ts` field. |
| 1.8 | JSON stream formatting costs 10–50 ms per emit (loop stall while streaming) | `SerialConsole::streamTick` | OPEN | Rate-limit to a CPU budget or move off the loop task. |
| 1.9 | Silence reset / confidence gating used relative level | `AudioTracker` | IN-PR #152 | Absolute raw level + ~1 s EMA. Verify breakdowns no longer reset tempo. |
| 1.10 | `stream nn` + other `[FPS]`/debug prints share TeeStream with NUS callback task | `SerialConsole`, `BleNus` | OPEN | BLE RX `handleCommand` runs in the Bluefruit callback task, concurrently with loop-task output (pre-existing hazard). Defer NUS commands to the loop task via a queue. |
| 1.11 | NN computes 16 output frames to use 1 (receptive field 9) | model export | OPEN (ML) | Re-export with W=9 or streaming conv state → ~½ inference cost. |

## 2. Spectral / mic / features

| # | Issue | Location | Status | Notes / proposed fix |
|---|---|---|---|---|
| 2.1 | Raw mel saturates (band pinned at 1.0) above ~−37 dBFS bass; NN loses low-band contrast at high SPL | `SharedSpectralAnalysis::computeMelBandsFrom` | PARTIAL | Telemetry added (`melSaturated`/`melTotal` in `json info`). Fix needs digital pre-gain or PCEN **mirrored in training**; use field recordings. |
| 2.2 | Level normalizer pins at 1.0 for the whole of a slow build-up | `AdaptiveMic::update` | OPEN | Peak attack tau 2 s lags a riser. Consider a faster attack when input > peak continuously. |
| 2.3 | Breakdown/drop valley behaviour; single-transient suppression | `AdaptiveMic` | IN-PR #152 | 0.2 s valley attack, debounce, valley snap-up. Verify on device. |
| 2.4 | Non-overlapping Hamming frames → onset-strength wobble with hit position | FFT framing | OPEN (ML) | 50 % overlap requires a retrain. |
| 2.5 | Compressor makes `bassFlux` ∝ level^(1/3); `bassContrast` may clamp at 1 on loud kicks | `applyCompressor`, `processFrame` | OPEN | Unverified: add telemetry before changing. |
| 2.6 | Mel whitening gives little contrast (log-domain ÷ running max) | `whitenMelBands` | OPEN | Low priority; comment corrected in #152. |
| 2.7 | Noise estimator creeps in steady music (leaky minimum) | `estimateAndSubtractNoise` | OPEN | Off by default and not persisted. Keep off, or switch to windowed min-statistics. |
| 2.8 | Unused per-frame work: whitening (backs user settings), debug-only compressed flatness (127 `logf`) | `SharedSpectralAnalysis` | OPEN | `atan2f` phases removed in #152. Compute flatness lazily when streaming. |
| 2.9 | `noiseGate` setting not persisted across reboots | `SerialConsole.cpp:250`, `ConfigStorage` | DECIDE | Persisting changes the stored settings layout (settings reset fleet-wide). Bundle with the next intentional `SETTINGS_VERSION` bump. |
| 2.10 | Mic ring overrun splices a window → spurious flux spike | `AdaptiveMic`, `SharedSpectralAnalysis` | IN-PR #152 | `markDiscontinuity()`. |

## 3. Device safety / BLE

| # | Issue | Location | Status | Notes / proposed fix |
|---|---|---|---|---|
| 3.1 | `configPrphBandwidth(BANDWIDTH_MAX)`: SoftDevice RAM requirement grows | `blinky-things.ino` | IN-PR #152 | Same config as the stock `bleuart` example on this core and linker. On failure: logged, BLE skipped, USB intact. **Verify:** boot log has no `[FALLBACK] Bluefruit.begin() FAILED`; `ble` shows MTU 247. |
| 3.2 | Loop-task stack 4 KB with ~3.8 KB used at HEAD (`updatePlpAnalysis`) | `AudioTracker` | IN-PR #152 | Scratch buffers moved to members: ~1.5 KB deepest path. No runtime high-water telemetry exists; consider logging `uxTaskGetStackHighWaterMark` in `json info`. |
| 3.3 | No runtime free-heap report; boot allocation failure is a halt | firmware | OPEN | Add free heap (`dbgHeapFree` or a mallinfo equivalent) to `json info`. |
| 3.4 | NUS has no pairing; any nearby central can send `bootloader` and config writes | `BleNus`, `SerialConsole` | DECIDE | Exposure grows with phone record sessions in crowds. Options: pairing/bonding, a command allowlist over BLE, or a session PIN. |
| 3.5 | During record sessions the HVN queue is shared with NUS; NUS text output can block the loop up to 100 ms per chunk | `BleNus::drainTxBuffer` | OPEN | Keep console quiet during sessions; consider a non-blocking notify check. |
| 3.6 | Fleet scanner gets less radio time while a central is connected (event length 125 ms) | SoftDevice config | OPEN | Measure broadcast reception with a phone connected. |
| 3.7 | Settings save still halts the CPU (NVMC erase) long enough to drop audio | `ConfigStorage::saveToFlash` | PARTIAL | Trailing debounce + skip-unchanged in #152. Consider deferring saves while audio energy is high. |

## 4. Visual generators

| # | Issue | Location | Status | Notes |
|---|---|---|---|---|
| 4.1 | Fire burst re-arm, particle pool exhaustion, fps-dependent aging/decays, Water beat sweep, phase-threshold beats | generators, `ParticleGenerator` | IN-PR #152 | `onset`/`beatCount` events, `killOldest`, stochastic rounding, dt scaling. Verify visually on the hat (linear layout). |
| 4.2 | LED `show()` DMA wait caps fps on large strips (~30 µs/LED) | `Nrf52PwmLedStrip` | OPEN | Not a stall; relevant to the ≥30 fps gate on 1000+ LED devices. |
| 4.3 | PlasmaGlobe replaces dt > 0.5 s with 16 ms; loop clamps dt at 100 ms | generators / loop | OPEN (low) | Time "freezes" briefly after stalls; acceptable. |

## 5. Phone record mode

| # | Issue | Status | Notes |
|---|---|---|---|
| 5.1 | End-to-end test on Android Chrome: 30 min, 1 m and 10 m, 0 audio gaps, fps within 5 %, FFT overruns unchanged | OPEN | Bench gate in `PHONE_RECORD_MODE_PLAN.md`. |
| 5.2 | Behaviour when the screen locks / tab is backgrounded | OPEN | Wake Lock is used; verify Chrome keeps notifications flowing. |
| 5.3 | Record-scoped hardware-gain override (headroom for loud venues) | DECIDE | Decide after the first field recording shows whether gain 32 clips. |
| 5.4 | Copilot review findings (10) | IN-PR #152 | All addressed in `0640b435`; verify the ambiguous-start / unsaved-tail / navigation paths on a phone. |

## 6. Tooling / CI / repo hygiene

| # | Issue | Status | Notes |
|---|---|---|---|
| 6.1 | Windows checkout (`core.autocrlf=true`) yields CRLF working files; the console pre-push Prettier check fails on 60+ untouched files | OPEN | Add `.gitattributes` (`* text=auto eol=lf` for the console) or set Prettier `endOfLine: auto`. |
| 6.2 | `blinky-simulator/build_vs.cmd` / `build.bat` still list removed `Lightning.cpp` and lack `Particle.cpp` | OPEN | CMake list fixed in #152; scripts not. |
| 6.3 | cppcheck failed at HEAD (2 findings) — CI said pass | OPEN | Check why CI's cppcheck (Ubuntu version) didn't flag them, or pin the version. Fixed in #152 for now. |
| 6.4 | `tests/tracker` harness not in CI | OPEN | Needs a host C++ compiler + ffmpeg in CI; at minimum the UBSan stress set. |
| 6.5 | No host test runs the NN path (no TFLite on host) | OPEN | TFLite Micro host build, or a recorded-activation replay. |
| 6.6 | `docs/OPEN_ISSUES_2026_05_20.md` still listed as "current" in CLAUDE.md | OPEN | Fold its still-open items here or keep both indexed. |

## 7. Device verification checklist for PR #152 (hat)

1. Flash only via `./scripts/deploy.sh --devices=<hat>`, then do not reset again for at least 60 s.
2. Boot log: no `[FALLBACK] Bluefruit.begin() FAILED`; `[BLE] Record service started` present.
3. Fleet server still reaches the hat over BLE; `ble` reports MTU 247; a `device upload` round-trip works (244-byte chunks, 1024-byte RX FIFO).
4. `json info`:
   - fps ≥ 30;
   - `audioOverruns` not climbing;
   - `spectralAccumOverflow` = 0;
   - `clipSamples` / `melSaturated` plausible.
5. Visual: tempo lock holds through a breakdown → drop. Fire bursts continue through dense hats. No particle starvation on the hat's linear layout.
