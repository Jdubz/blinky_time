# Claude Code Instructions for Blinky Project

This file contains **critical behavior rules only**. Architecture, status, and historical context live in `docs/` (see the index at the bottom).

> ESP32-S3 support was cut March 2026. All active development targets **nRF52840**.

## CRITICAL: Upload Safety (nRF52840)

**NEVER use `arduino-cli upload` or `adafruit-nrfutil dfu serial` on nRF52840.** The DFU serial protocol has race conditions that can brick devices, requiring SWD hardware to recover.

**Safe upload paths:**
1. **Preferred:** `./scripts/deploy.sh` — compile → upload → flash via blinky-server → verify
2. **Manual fleet flash:**
   ```bash
   curl -X POST http://blinkyhost.local:8420/api/fleet/upload \
     -H "X-API-Key: $(cat ~/.blinky-api-key)" \
     -F "firmware=@/tmp/blinky-build/blinky-things.ino.hex"
   ```
3. **Direct UF2 (bare chip):** `make uf2-upload UPLOAD_PORT=/dev/ttyACM0`

blinky-server owns all serial/BLE connections — no port contention. See `docs/SAFETY.md` for the full safety model, `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` for BLE DFU details.

**Risky firmware:** always test on unenclosed bare chips first (reset button + SWD pads accessible). Installed devices have no physical access.

**Unresponsive device recovery:** `uhubctl -a cycle -p <port>` → server flash → wait for re-enumeration. Last resort: physical reset.

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
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator / Effect / Renderer pattern |
| `docs/HARDWARE.md` | XIAO nRF52840 Sense hardware specs |
| `docs/BUILD_GUIDE.md` | Build and installation instructions |
| `docs/DEVELOPMENT.md` | Development guide, config management |
| `docs/SAFETY.md` | Flashing safety mechanisms |
| `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` | BLE, WiFi, OTA, fleet server |
| `docs/FLEET_CONSOLE_REFACTOR_PLAN.md` | blinky-console refactor roadmap |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Historical calibration results |
| `blinky-test-player/NEXT_TESTS.md` | Priority testing tasks |
