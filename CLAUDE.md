# Claude Code Instructions for Blinky Project

This file contains **critical behavior rules only**. Architecture, status, and historical context live in `docs/` (see the index at the bottom).

> ESP32-S3 support was cut March 2026. All active development targets **nRF52840**.

## CRITICAL: Upload Safety (nRF52840)

**NEVER use `arduino-cli upload` or `adafruit-nrfutil dfu serial` on nRF52840.** The DFU serial protocol has race conditions that can brick devices, requiring SWD hardware to recover.

**Flashing connected devices: `./scripts/deploy.sh` is REQUIRED.** Direct `curl` against `/api/fleet/upload`, `/api/fleet/flash`, or `/api/devices/{id}/command` with a device-mutating command body (`device upload`, `reboot`) is forbidden — the server enforces this via an `X-Deploy-Tool` header check that only `deploy.sh` sets, so manual curl returns 403. The API key alone is not sufficient. Use:

```bash
./scripts/deploy.sh                     # compile + upload + flash + verify
./scripts/deploy.sh --skip-compile      # deploy already-compiled hex (e.g. after build.sh)
./scripts/deploy.sh --no-bump           # recompile without bumping build number
```

deploy.sh runs the full pipeline (compile → upload → flash → version-verify) and fails loud on any error. Bypassing it (even just to "save time") loses the version-verify step that catches partial flashes.

**The deploy.sh-only rule is broader than firmware flashing.** It also covers any sequence of API calls that mutates persistent device state or device lifecycle: `device upload` (writes config to flash), `reboot`, `wipe_device_identity` (and its deprecated aliases `factory` / `reset`), fleet-wide config sweeps that loop over multiple settings, etc. Even though those go through `/api/devices/{id}/command` (not `/api/fleet/upload`), running them in an ad-hoc loop without a tested workflow leaves devices in `error`/`connecting` states and requires manual recovery. The server enforces this for the destructive subset (`device upload`, `reboot`, `wipe_device_identity`, `factory`, `reset` — see `blinky_server/api/deps.py:_DEPLOY_GATED_COMMAND_LIST` for the canonical list); for everything else the discipline is on the operator. **If a real test needs different device configurations, build firmware variants with each config baked in and deploy each via `deploy.sh` — do NOT script API config flips on running fleet devices.**

**Bare-chip recovery only:** `make uf2-upload UPLOAD_PORT=/dev/ttyACM0` (use when a device is bricked and not yet enrolled in the fleet).

blinky-server owns all serial/BLE connections — no port contention. See `docs/SAFETY.md` for the full safety model, `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` for BLE DFU details.

**Risky firmware:** always test on unenclosed bare chips first (reset button + SWD pads accessible). Installed devices have no physical access.

**Unresponsive device recovery:** `uhubctl -a cycle -p <port>` → server flash → wait for re-enumeration. Last resort: physical reset.

## CRITICAL: Task is Onset Detection, Not Beat Detection

**The model and firmware are doing ONSET DETECTION** — fire on every percussive event (kick, snare, ghost notes, hi-hats) regardless of metrical position. **We do NOT care whether an event lands on or off the beat.** Beat-detection framing is wrong for this task and creates noise — both metric noise (scoring against beat positions when the goal is to fire on every drum hit) and label noise (training on beat consensus teaches the model to suppress off-beat onsets, which we want to keep).

Concrete consequences:
- **Training labels must be onset-flavored** (`onsets_consensus`, `kick_weighted_drums`, etc.). Never use `consensus_v*` (beat tracker output) or anything from `allin1`/`beat_this`/`beatnet`/`demucs_beats` as a training target. Those directories are archived under `_archive_beats_2026_04_29/`. `prepare_dataset.py` refuses to load paths with those markers.
- **Eval ground truth is `.onsets_consensus.json`**, not `.beats.json`. The blinky-server validation harness reads onsets_consensus when present (since 2026-04-29 it requires it). Beat GT files were archived out of `blinky-test-player/music/edm/`.
- **Beat-tracking systems (allin1, beat_this, beatnet, demucs_beats, the BPM half of madmom)** still exist as reference scripts but their *output is beats* and must not flow into the training-label pipeline.
- **BPM in firmware (PLP / ACF tempo tracking)** is fine — it's used to phase-lock the visualizer animation, not to gate onset detection. `beatGridPatternMin=0.0` (b149+ default) keeps the AND-gate off so the visualizer fires on every onset, not only on beats.

When in doubt: an onset is *every percussive event*. A beat is *every metrical position*. We want the former. Beats are a strict subset of onsets, and a beat-detection model trained or scored on beats will *under-fire* on the deployment task.

## CRITICAL: Validation Corpus

**The only validation corpus is `blinky-test-player/music/edm/` (18 tracks).** It contains representative EDM with clear percussion (techno×5, trance×3, dnb×2, breakbeat×2, dubstep, garage, reggaeton, trap, afrobeat, amapiano) — the content the system is designed to perform on. Lock in performance here before measuring anything else.

**Do NOT introduce a "held-out" / "hard" / "adversarial" corpus and use it as a headline metric.** This has happened multiple times — most recently `edm_holdout/` (GiantSteps LOFI: ambient, phrase-shifting, intentionally challenging). Reporting F1 on adversarial content as the primary number normalises low scores ("F1 0.47 is fine, that's hard content") and burns weeks chasing model fixes for problems that look smaller on representative content. The adversarial-corpus pattern was deleted 2026-04-27 as part of a structural fix, not a policy.

If you need to test against harder material, do it *after* edm/ is solid and report it as a tier-2 number with separate framing, not next to the headline F1. New corpora must clear that bar before being added — and never as a default in scripts.

## CRITICAL: No Silent Fallbacks

**Errors must fail LOUDLY. Never silently substitute defaults, zero-fills, clamps, or empty values for invalid state.** A silent fallback is a production bug waiting to happen — it lets configuration drift, version mismatches, hardware faults, and model corruption masquerade as "working." When in doubt, assert, panic, or log at ERROR — never quietly continue with degraded data.

**The reference incident (2026-04-27, v33):** `MelBandDef MEL_BANDS[NUM_MEL_BANDS] = { /* 30 entries */ }` was left at the v32 size when `NUM_MEL_BANDS` bumped 30 → 50. C++ aggregate-init silently zero-filled the missing 20 entries to `{0,0,0}`, all reading FFT bin 0 (DC). The neural network received garbage in 40% of its input bands and the activation distribution collapsed. There was no compile error, no runtime error, no warning. Two weeks of training experiments and threshold sweeps were spent chasing a model-side hypothesis for a firmware-side data corruption.

**Concrete patterns to refuse:**

1. **Array literals smaller than the declared size.** Always either (a) static_assert that the initializer count equals the size, or (b) generate the full table programmatically. *Never rely on aggregate-init zero-fill for tail entries.*
2. **Ternary fallbacks that mask invalid state.** `(weightSum > 0) ? sum / weightSum : 0.0f` is wrong when `weightSum == 0` represents a configuration bug, not a runtime case — assert instead. The principled exception is denormal/silence handling where 0 is the *correct* answer (e.g., NaN guards on FFT magnitudes during true silence) — those must be commented as such.
3. **`validate*` / `clamp*` functions that silently coerce out-of-range values.** A corrupt config value should boot-loop with a logged error, not silently clamp to a default. If clamping is intentional (e.g., user-typed input), log every coercion at WARN with the original value.
4. **Functions that return `false` / `0` / `nullptr` / empty without a serial log.** Callers may not check; the symptom is a feature that "just doesn't work." If returning a sentinel is unavoidable, log on the way out.
5. **`switch` defaults / catchall `else` branches that absorb unexpected enum values.** Either handle every case explicitly or `panic("unhandled <enum>")` in the default. The whole point of the enum is exhaustiveness.
6. **`if (!initialized) { init_with_defaults(); }` patterns.** Lazy init can mask a forgotten explicit init. Fail at the call site if state is uninitialized.
7. **TFLite / TF status checks like `if (status != kTfLiteOk) return;` without a serial log.** The model silently stops inferring; the visualizer keeps running on stale activations.
8. **`#ifdef` / `#elif` chains with an unguarded `#else` that compiles to a placeholder.** Use `#error` in the unhandled case so the wrong build never ships.
9. **NaN/Inf substitution to 0** is acceptable only for known denormal cases (must be commented). For everything else, increment a counter *and* log on first occurrence.

When you discover a silent fallback, the fix is to (a) assert / panic at the failing site, (b) add a `[FALLBACK]` serial log with file:line and the offending value, or (c) add a compile-time check that prevents the bad state. Do not just "improve" the comment.

## CRITICAL: Long-Running Scripts

**NEVER run ML training or other long-running scripts as Claude session tasks** — they die when the session ends.

Always use tmux:
```bash
tmux new-session -d -s training "cd ml-training && source venv/bin/activate && PYTHONUNBUFFERED=1 python train.py --config configs/<name>.yaml --output-dir outputs/<experiment> 2>&1 | tee outputs/<experiment>/training.log"
```

`train.py` enforces this — it refuses to start outside tmux/screen unless `--allow-foreground` is passed.

## Build Workflow

**Always use `scripts/build.sh` to compile.** It auto-increments `blinky-things/BUILD_NUMBER` and regenerates `types/Version.h`. Build numbers are the source of truth for deployed firmware — git is for collaboration, not deployment.

```bash
./scripts/build.sh                          # nRF52840 compile only
./scripts/build.sh --upload /dev/ttyACM0    # compile + UF2 upload
./scripts/build.sh --no-bump                # recompile without incrementing (e.g. after failed upload)
```

Use `./scripts/deploy.sh` for full compile → upload → fleet flash → verify.

## Device Workflow Discipline

- Don't ad-hoc flash or reconfigure devices; use scripted workflows.
- Validation resets devices to defaults before each run — device settings persist across flashes and can corrupt tests otherwise.
- `run_test` / `run_validation_suite` / `check_test_result` MCP tools submit jobs to blinky-server; no direct port management needed.

## Documentation Guidelines

**Only create docs with future value:** architecture specs, implementation plans, outstanding-action todo lists, testing/calibration guides.

**Do NOT create:**
- Code review documents (delete after fixes land)
- Analysis reports of completed work (vanity docs)
- Historical "what we did" summaries (git log is the record)
- Post-mortems (capture lessons in architecture docs or commit messages)

Reviews and analysis must focus on **outstanding actions**, not documenting past work.

## Documentation Index

| Document | Purpose |
|----------|---------|
| `docs/VISUALIZER_GOALS.md` | Design philosophy — visual quality over metrics |
| `docs/AUDIO_ARCHITECTURE.md` | AudioTracker, FrameOnsetNN, PLP, spectral analysis |
| `docs/AUDIO-TUNING-GUIDE.md` | Main testing guide, tunable params, test procedures |
| `docs/IMPROVEMENT_PLAN.md` | Current status and roadmap |
| `docs/ML_IMPROVEMENT_PLAN.md` | NN training roadmap and experiment history |
| `docs/AUDIO_SYSTEM_AUDIT_2026_04_24.md` | Point-in-time audit of training/firmware/validation pipelines with prioritized fix plan (tasks #77–#88) |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator / Effect / Renderer pattern |
| `docs/HARDWARE.md` | XIAO nRF52840 Sense hardware specs |
| `docs/BUILD_GUIDE.md` | Build and installation instructions |
| `docs/DEVELOPMENT.md` | Development guide, config management |
| `docs/SAFETY.md` | Flashing safety mechanisms |
| `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` | BLE, WiFi, OTA, fleet server |
| `docs/FLEET_CONSOLE_REFACTOR_PLAN.md` | blinky-console refactor roadmap |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Historical calibration results |
| `blinky-test-player/NEXT_TESTS.md` | Priority testing tasks |
