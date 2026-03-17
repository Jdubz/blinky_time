# Claude Code Instructions for Blinky Project

## CRITICAL: Upload Safety

Upload safety depends on the platform. ESP32-S3 and nRF52840 use completely different upload protocols.

### ESP32-S3: `arduino-cli upload` is SAFE

```bash
arduino-cli compile --upload --fqbn esp32:esp32:XIAO_ESP32S3 -p /dev/ttyACM0 blinky-things
```

`arduino-cli upload` on ESP32-S3 calls **esptool**, which talks to the chip's hardware ROM bootloader. The ROM bootloader is burned into silicon and cannot be bricked. esptool verifies every write. If interrupted, the ROM bootloader still works and you just re-flash.

### nRF52840: NEVER use `arduino-cli upload`

**NEVER use `arduino-cli upload` or `adafruit-nrfutil dfu serial` on nRF52840!**

- `arduino-cli upload` calls `adafruit-nrfutil dfu serial` under the hood
- This protocol has race conditions in USB port re-enumeration
- Single-bank DFU can leave firmware partially written
- Can brick the device, requiring SWD hardware to recover

**Use UF2 instead:**
```bash
make uf2-upload UPLOAD_PORT=/dev/ttyACM0
```

**Why UF2 is safe:**
- Invalid/corrupt UF2 files are silently rejected (cannot brick)
- Simple file copy to mass storage (no serial protocol race conditions)
- Bootloader protects itself (hardware-enforced)
- If interrupted, old firmware stays intact

See `tools/uf2_upload.py --help` for all options.

### nRF52840 Pre-Flash Checklist

**Devices are physically installed — double-tap reset is NOT an option.**
Bootloader entry MUST succeed via software (serial command or 1200-baud touch).
A failed bootloader entry leaves the device running old firmware but wastes time.

**Before EVERY nRF52840 flash attempt:**
1. **Disconnect ALL MCP sessions** — `mcp__blinky-serial__disconnect` on every port, or verify `mcp__blinky-serial__status` shows no connections
2. **Wait 3 seconds** after MCP disconnect — the Node.js `SerialPort.close()` is async; the OS file descriptor may not be released immediately
3. **Do NOT flash immediately after interactive serial use** — always disconnect and wait
4. **Flash one device at a time** unless using `--parallel` mode

**Why this matters:** If an MCP server or console session holds the serial port, `uf2_upload.py` cannot send the bootloader entry command. The device resets but doesn't enter UF2 mode. The script retries 5 times (40+ seconds wasted), then fails. The `uf2_upload.py` script includes a port availability pre-check that will detect and report this condition.

### Safe Operations Summary

**ESP32-S3:**
- `arduino-cli compile --upload` — Safe (uses esptool)
- `arduino-cli upload` — Safe (uses esptool)

**nRF52840:**
- `arduino-cli compile` — Safe (compile only)
- `make uf2-upload` — Safe (uses mass storage, not DFU serial)
- `make uf2-check` — Safe (dry run, no upload)
- `arduino-cli upload` — **NEVER** (uses fragile DFU serial protocol)
- `adafruit-nrfutil dfu serial` — **NEVER**

**Both platforms:**
- `arduino-cli core list/install` — Safe
- Reading serial ports — Safe

### If a Device Becomes Unresponsive

**nRF52840** devices are physically installed and reset buttons are NOT accessible.
If a device stops responding to serial commands:
1. Try power-cycling via USB hub: `uhubctl -a cycle -p <port>`
2. Re-run: `python3 tools/uf2_upload.py --build-dir /tmp/blinky-build /dev/ttyACMx`
3. If the port disappeared entirely, wait 10 seconds and check `ls /dev/ttyACM*`
4. Last resort: physically access the device and double-tap reset

**ESP32-S3**: Just re-run `arduino-cli compile --upload`. The ROM bootloader always survives.

## CRITICAL: Long-Running Scripts

**NEVER run ML training or other long-running scripts as Claude session tasks.**
Claude background tasks die when the session ends, killing training mid-run.

Always use tmux:
```bash
tmux new-session -d -s training "cd ml-training && source venv/bin/activate && PYTHONUNBUFFERED=1 python train.py --config configs/frame_fc.yaml --output-dir outputs/<experiment_name> 2>&1 | tee outputs/<experiment_name>/training.log"
```

To check progress: `tmux attach -t training` or `tail -f ml-training/outputs/<experiment_name>/training.log`

`train.py` enforces this — it will refuse to start outside tmux/screen unless `--allow-foreground` is passed.

## Compilation Commands

```bash
# === ESP32-S3 ===
# Compile only
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 blinky-things

# Compile + upload (safe — uses esptool)
arduino-cli compile --upload --fqbn esp32:esp32:XIAO_ESP32S3 -p /dev/ttyACM0 blinky-things

# === nRF52840 ===
# Compile only (in-tree build, requires TFLite library)
arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Sense blinky-things

# Compile + validate + upload via UF2 (recommended, NEVER use arduino-cli upload)
make uf2-upload UPLOAD_PORT=/dev/ttyACM0

# Compile + validate only (dry run)
make uf2-check UPLOAD_PORT=/dev/ttyACM0
```

## Documentation Structure

### Primary Documentation

| Document | Purpose |
|----------|---------|
| `docs/VISUALIZER_GOALS.md` | **Design philosophy** - visual quality over metrics |
| `docs/AUDIO_ARCHITECTURE.md` | AudioController architecture (CBSS beat tracking) |
| `docs/AUDIO-TUNING-GUIDE.md` | **Main testing guide** - 56 tunable parameters, test procedures |
| `docs/IMPROVEMENT_PLAN.md` | Current status and roadmap |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator/Effect/Renderer pattern |

### Testing & Calibration

| Document | Purpose |
|----------|---------|
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Historical calibration results (F1 scores, optimal values) |
| `blinky-test-player/NEXT_TESTS.md` | Priority testing tasks |
| `TESTING.md` | Quick start for testing (links to AUDIO-TUNING-GUIDE.md) |

### Hardware & Build

| Document | Purpose |
|----------|---------|
| `docs/HARDWARE.md` | Hardware specifications (XIAO nRF52840 Sense) |
| `docs/BUILD_GUIDE.md` | Build and installation instructions |
| `docs/DEVELOPMENT.md` | Development guide (config management, safety procedures) |
| `docs/SAFETY.md` | Critical safety guidelines for flashing |

### Key Architecture Components

- **AudioController** (`blinky-things/audio/AudioController.h`) - Unified audio analysis
- **FrameBeatNN** (`blinky-things/audio/FrameBeatNN.h`) - Single Conv1D W64 TFLite NN inference (beat + downbeat multi-task)
- **SharedSpectralAnalysis** (`blinky-things/audio/SharedSpectralAnalysis.h`) - FFT → compressor → whitening → mel bands
- **AdaptiveMic** (`blinky-things/inputs/AdaptiveMic.h`) - Microphone input with fixed hardware gain (AGC removed v72)
- **AudioControl struct** (`blinky-things/audio/AudioControl.h`) - Output: energy, pulse, phase, rhythmStrength, onsetDensity

### Obsolete Documents (Removed)

The following were deleted as outdated:
- `RHYTHM-TRACKING-REFACTOR-PROPOSAL.md` - Replaced by AudioController implementation (Dec 2025)
- `TUNING-PLAN.md` - Superseded by AUDIO-TUNING-GUIDE.md (Dec 2025)
- `docs/plans/MUSIC_MODE_TESTING_PLAN.md` - Referenced old MusicMode/PLL (Dec 2025)
- `docs/plans/TRANSIENT_DETECTION_TESTING.md` - Work completed, merged into AUDIO-TUNING-GUIDE.md (Dec 2025)
- `docs/AUDIO_IMPROVEMENTS_PLAN.md` - Phase 1&2 completed, vanity doc (Jan 2026)
- `docs/RHYTHM_ANALYSIS_ENHANCEMENT_PLAN.md` - Replaced by AUDIO_ARCHITECTURE.md (Jan 2026)
- `docs/ENSEMBLE_CALIBRATION_ASSESSMENT.md` - Completed assessment (Jan 2026)
- `blinky-serial-mcp/TOOLING-ASSESSMENT.md` - Completed assessment (Jan 2026)
- `docs/MULTI_HYPOTHESIS_OPEN_QUESTIONS_ANSWERED.md` - Obsolete (Jan 2026)
- `docs/MULTI_HYPOTHESIS_TRACKING_PLAN.md` - Replaced by CBSS beat tracking (Feb 2026)
- `docs/RHYTHM_ANALYSIS_IMPROVEMENTS.md` - Obsolete, referenced deleted params (Feb 2026)
- `docs/RHYTHM_ANALYSIS_TEST_PLAN.md` - Referenced old hypothesis system (Feb 2026)
- `docs/RHYTHM_ANALYSIS_IMPROVEMENT_PLAN.md` - Referenced CombFilterPhaseTracker/fusion (Feb 2026)
- `docs/COMB_FILTER_IMPROVEMENT_PLAN.md` - Old comb filter plan, system replaced (Feb 2026)
- `docs/AUDIO_IMPROVEMENT_ANALYSIS.md` - Completed analysis (Feb 2026)
- `blinky-test-player/src/param-tuner/hypothesis-validator.ts` - Old hypothesis test (Feb 2026)
- `blinky-test-player/TUNING_SCENARIOS.md` - Merged into PARAM_TUNER_GUIDE.md (Jan 2026)
- `docs/PLAN-blinky-console-ui.md` - Console UI built, plan obsolete (Mar 2026)
- `docs/ML_TRAINING_PLAN.md` - Superseded by IMPROVEMENT_PLAN.md sections (Mar 2026)
- `docs/FREQUENCY_DETECTION.md` - Feature removed from firmware (Mar 2026)
- `docs/PLATFORM_FIX.md` - Applied to old Seeeduino mbed platform, no longer used (Mar 2026)
- `docs/COMMON_SCENARIO_TEST_PLAN.md` - Superseded by multi-device A/B test infrastructure (Mar 2026)

## System Architecture Overview

### Project Structure

The Blinky Time project consists of 5 major components:

```
blinky_time/
├── blinky-things/          Arduino firmware (nRF52840 + ESP32-S3)
├── blinky-console/         React web UI (WebSerial interface)
├── blinky-test-player/     Node.js testing CLI (parameter tuning)
├── blinky-serial-mcp/      MCP server (AI integration)
└── blinky-simulator/       Desktop GIF renderer (compiles actual firmware)
```

### Firmware Architecture (blinky-things/)

**Core Pipeline: Generator → Effect → Renderer**

```
PDM Microphone (16 kHz)
    ↓
AdaptiveMic (fixed gain + window/range normalization)
    ↓
SharedSpectralAnalysis (FFT-256 → compressor → whitening → mel bands)
    ↓
    ├── FrameBeatNN (Conv1D W64, 27ms, every frame) → beat activation (ODF) + downbeat activation
    ↓
AudioController (ODF info gate → CBSS beat tracking + pulse baseline tracking)
    ↓
AudioControl {energy, pulse, phase, rhythmStrength, onsetDensity, downbeat, beatInMeasure}
    ↓
Generator (Fire/Water/Lightning)
    ↓
Effect (HueRotation/NoOp)
    ↓
RenderPipeline → LED Output
```

**Key Components:**

1. **Audio Input & Processing**
   - `AdaptiveMic.h` - PDM microphone with fixed hardware gain (AGC removed v72; nRF52840: gain=32, ESP32-S3: gain=30)
   - `SharedSpectralAnalysis.h` - FFT-256 (128 freq bins @ 62.5 Hz), soft-knee compressor → per-bin whitening (v23+)
   - Window/range normalization (0-1 output) — sole dynamic range system

2. **Beat/Downbeat Detection (single Conv1D model, deployed)**
   - `FrameBeatNN.h` - Single-model TFLite NN inference
     - Conv1D W64 (1.024s), [24,32] channels, 15.1 KB INT8, 27ms measured on device, every frame → beat activation (ODF) + downbeat activation
     - Beat This! sum head: downbeat output structurally constrained ≤ beat output
     - Beat F1=0.480, DB F1=0.160 (offline eval)
     - Arena: 7340/32768 bytes
   - Non-NN fallback: `mic_.getLevel()` (energy envelope as simple ODF)

3. **Rhythm Tracking (AudioController)**
   - `AudioController.h/cpp` - Bayesian tempo fusion + CBSS beat tracking
   - OSS buffering (6 seconds @ 60 Hz)
   - ODF source: FrameBeatNN beat activation (Conv1D). Falls back to mic level if model fails to load.
   - ODF information gate: suppresses low-confidence ODF when NN output is weak (prevents noise-driven false beats)
   - Bayesian tempo fusion: 20-bin posterior (~60-198 BPM), comb filter bank + harmonic-enhanced ACF (0.8, v25). FT/IOI disabled (v28)
   - Per-sample ACF harmonic disambiguation (2x and 1.5x checks after MAP extraction)
   - CBSS: cumulative beat strength signal with log-Gaussian transition weighting (tuned for faster convergence)
   - BTrack-style predict+countdown beat detection with CBSS adaptive threshold (cbssthresh=1.0)
   - Deterministic phase derivation
   - Pulse detection: floor-tracking baseline (replaces running-mean threshold)
   - Energy synthesis: hybrid mic level + bass mel energy + ODF peak-hold
   - ODF pre-smoothing (5-point causal moving average)
   - ODF mean subtraction disabled (v32: raw ODF preserves ACF structure)
   - Onset-density octave discriminator (v32: penalizes implausible tempos in posterior)
   - Shadow CBSS octave checker (v32: compares T vs T/2 every 2 beats)

4. **Generators (Visual Effects)**
   - `Fire.cpp/h` - HeatFire: hybrid audio-reactive design, dt-based scroll speed, energy drives full flame height
   - `Water.cpp/h` - Wave simulation with ripples
   - `Lightning.cpp/h` - Branching bolt effects
   - All generators consume `AudioControl` struct

5. **Post-Processing Effects**
   - `HueRotationEffect.h` - Color cycling
   - `NoOpEffect.h` - Pass-through (identity)
   - Effect chaining supported

6. **Configuration & Persistence**
   - `ConfigStorage.h/cpp` - Flash-based storage (SETTINGS_VERSION: v73)
   - `SettingsRegistry.h/cpp` - Tunable parameters (~30 after BandFlux removal)
   - Runtime validation (min/max bounds)
   - Factory reset capability

7. **Device Abstraction**
   - `HatConfig.h` - 89 LEDs (STRING layout)
   - `TubeLightConfig.h` - 60 LEDs (4x15 MATRIX)
   - `BucketTotemConfig.h` - 128 LEDs (16x8 MATRIX)
   - Compile-time device selection

8. **Hardware Abstraction Layer (HAL)**
   - `IPdmMic.h`, `ISystemTime.h`, `ILedStrip.h` - Interfaces
   - `DefaultHal.h` - Platform implementations
   - `MockHal.h` - Test implementations

9. **Serial Interface**
   - `SerialConsole.h/cpp` - Command interpreter
   - JSON API (settings, streaming, info)
   - 50+ commands (get/set/show/save/load)
   - Audio streaming (~20 Hz)

### Web UI Architecture (blinky-console/)

**Stack: React 18 + TypeScript + Vite + WebSerial**

```
React Components
├── ConnectionBar (device status, battery)
├── SettingsPanel (50+ parameters)
├── AudioVisualizer (Chart.js real-time display)
├── GeneratorSelector (Fire/Water/Lightning)
├── EffectSelector (HueRotation/NoOp)
├── SerialConsoleModal (raw command interface)
└── TabView (multi-panel layout)
```

**Features:**
- WebSerial API for direct USB serial connection
- Real-time audio visualization (energy, pulse, transients)
- Hierarchical settings organization
- PWA (offline capable, installable)
- Responsive design (desktop/mobile)

### Testing Infrastructure

**blinky-test-player (Parameter Tuning)**
- Playwright-based audio pattern playback
- Ground truth comparison (expected vs detected transients)
- Binary search optimization (~30 min per param)
- Sweep mode (exhaustive parameter range testing)
- 60+ test patterns (simple-beat, complex-rhythm, polyrhythmic, melodic, etc.)

**blinky-serial-mcp (AI Integration)**
- 20+ MCP tools for device interaction
- Connection management (list_ports, connect, status)
- Settings control (get_settings, set_setting, save_settings)
- Audio streaming (stream_start, get_audio, monitor_audio)
- Testing (run_test, start_test, stop_test)
- Pattern library (list_patterns)

### MCP Testing Best Practices

**ALWAYS use `run_test` for pattern testing** - it automatically:
1. Connects to the device
2. Plays the pattern and records detections
3. Disconnects when complete

```
run_test(pattern: "steady-120bpm", port: "COM11")
```

**Gain is fixed** - AGC was removed in v72. Hardware gain is set at platform optimal (nRF52840: 32, ESP32-S3: 30). Window/range normalization handles dynamic range.

**DO NOT manually connect/disconnect** - Using separate `connect`, `stream_start`, `start_test`, `stop_test`, `disconnect` calls:
- Risks leaving the port locked if an error occurs
- Prevents firmware flashing until manually disconnected
- Is more error-prone and verbose

**Exception**: Use manual connection only when:
- Exploring settings interactively (`get_settings`, `set_setting`)
- Monitoring audio continuously (`monitor_audio` with long duration)
- Debugging device state (`status`, `send_command`)

**Unit & Integration Tests**
- `tests/unit/` - Device configs, LED mapping, parameter bounds
- `tests/integration/` - Generator output, effect chaining, serial commands
- Custom BlinkyTest.h framework (Arduino-compatible)

### Data Flow Example (Fire Effect)

```
1. PDM mic samples → AdaptiveMic (fixed gain + window/range normalization)
2. AdaptiveMic → SharedSpectralAnalysis (FFT-256 → compressor → per-bin whitening → mel bands)
3. SharedSpectralAnalysis → FrameBeatNN (64-frame mel window → Conv1D → beat + downbeat activation)
4. Beat activation → ODF information gate → ODF value (0-1)
5. ODF → AudioController OSS buffer (6s history)
6. AudioController → autocorrelation every 250ms → Bayesian tempo fusion
   (ACF + comb filter bank → 20-bin posterior → harmonic disambig → MAP → BPM)
7. CBSS backward search → cumulative beat strength signal
8. Predict+countdown beat detection → deterministic phase
9. FrameBeatNN downbeat_activation → AudioControl.downbeat
10. Output: AudioControl{energy=0.45, pulse=0.85, phase=0.12, rhythmStrength=0.75,
    onsetDensity=3.2, downbeat=0.9, beatInMeasure=1}
11. Fire generator:
    - energy → baseline flame height
    - pulse → spark burst intensity
    - phase → breathing effect (0=on-beat)
    - rhythmStrength → blend music/organic mode
    - onsetDensity → content classification (dance=2-6/s, ambient=0-1/s)
    - downbeat → extra-dramatic effects on bar 1
    - beatInMeasure → syncopation patterns, accent beats
12. Fire heat diffusion (matrix propagation)
13. HueRotationEffect (optional color shift)
14. RenderPipeline → LED strip output
```

### Resource Usage (nRF52840)

**Memory:**
- RAM: ~20 KB globals + arena 7340/32768 bytes (Conv1D W64 model)
- Flash: ~359 KB with single model (15.1 KB INT8 + TFLite Micro runtime). ~30 KB settings storage.
- Available: 256 KB RAM, 1 MB Flash

**CPU (64 MHz):**
- Microphone + FFT: ~4%
- FrameBeatNN inference (62.5 Hz): 27ms measured on device
- Autocorrelation (500ms): ~3% amortized
- CBSS + beat detection: ~1%
- Fire generator: ~5-8%
- LED rendering: ~2%
- **Total: ~40-45%** (inference time dominates but fits within frame budget)

### Safety Architecture (Multi-Layer Defense)

**Layer 1: Documentation**
- CLAUDE.md (persistent AI warnings)
- DEVELOPMENT.md (safety procedures)
- SAFETY.md (mechanism overview)

**Layer 2: Compile-Time**
- Static assertions (struct size validation)
- CONFIG_VERSION enforcement
- Type-safe parameter access

**Layer 3: Runtime**
- Parameter validation (min/max bounds)
- Corrupt data detection
- Flash address validation (bootloader protection)

**Layer 4: Flash Safety**
- Bootloader region protection (< 0x30000)
- Sector alignment validation
- System halt on unsafe write

**Layer 5: Automation**
- Pre-commit hooks
- Safety check scripts
- Git hooks enforcement

**Layer 6: Upload Enforcement**
- Arduino IDE only (NO arduino-cli)
- Device bricking prevention

### Current Status (March 2026)

**Production Ready:**
- ✅ AudioController with CBSS beat tracking + ODF information gate + pulse baseline tracking
- ✅ FrameBeatNN (Conv1D W64, 15.1 KB INT8, Beat F1=0.480, DB F1=0.160, deployed on all 7 devices)
- ✅ HeatFire/Water/Lightning generators
- ✅ Web UI (React + WebSerial)
- ✅ Testing infrastructure (MCP + param-tuner + batch A/B test scripts)
- ✅ Multi-layer safety mechanisms
- ✅ 3 device configurations (Hat, Tube, Bucket) + Display (32x32 matrix)
- ✅ Mic calibration pipeline + gain-aware training augmentation
- ✅ Fixed hardware gain (AGC removed v72; nRF52840: 32, ESP32-S3: 30)
- ✅ Simulator working (rebuilds with current firmware code)
- ✅ 7 devices: 3 nRF52840 + 2 ESP32-S3 on blinkyhost, 1 nRF52840 tube + 1 ESP32-S3 display local

**Removed (v64-v72):**
- v64: Forward filter, particle filter, HMM phase tracker, multi-agent beat tracking, template/subbeat/metrical octave checks, ODF sources 1-5, legacy spectral flux (~1500 lines)
- v67: EnsembleDetector, BandFlux, EnsembleFusion, BassSpectralAnalysis, IDetector, DetectionResult (~2600 lines, ~24 settings, ~22 KB flash, ~2 KB RAM saved)
- v68: Removed ENABLE_NN_BEAT_ACTIVATION ifdef and nnBeatActivation runtime toggle. FrameBeatNN always compiled in and active. TFLite is a required dependency.
- v72: AGC removed. Hardware gain fixed at platform optimal. Window/range normalization is sole dynamic range system.
- Spectral noise subtraction (`noiseest=0`): still in SharedSpectralAnalysis, default OFF

**Planned (Not Started):**
- NN model improvements: confidence-weighted loss, tempo auxiliary head, wider windows with Conv1D
- ESP32-S3 platform-specific model (larger compute budget allows bigger model)
- Bluetooth/BLE support (design doc complete)
- Dynamic device switching (runtime config)
- CI/CD automation

**Closed (mel-spectrogram CNN, v4-v9):**
- All architectures (standard conv, BN-fused, DS-TCN) measured 79-98ms on Cortex-M4F — 8-10× over frame budget
- Superseded by frame-level FC approach (~0.2-5ms at 31.25 Hz)

**Closed (beat-synchronous hybrid, March 2026):**
- FC on accumulated spectral summaries at beat rate (~2 Hz). Circular dependency with CBSS, negligible discriminative power in per-beat features, misaligned with all leading approaches. Superseded by frame-level FC.

**Closed (W192 FC, March 2026):**
- FC(4992→64→32→2), 322K params, 314 KB INT8. Beat F1=0.370, DB F1=0.145 — severe regression from W32 (0.491/0.238). FC flattening destroys temporal locality for wide windows.

**Closed (Dual-model architecture, March 2026):**
- OnsetNN + RhythmNN split abandoned. Every published beat/downbeat system uses a single joint model. Split underperformed FC baseline on both tasks. Superseded by single Conv1D W64 with Beat This! sum head.

## Documentation Guidelines

**Only create documentation with future value:**
- Architecture designs and technical specifications
- Implementation plans and roadmaps
- Todo lists and outstanding action items
- Testing procedures and calibration guides

**DO NOT create:**
- Code review documents (delete after fixes are implemented)
- Analysis reports of completed work (vanity documentation)
- Historical "what we did" summaries (use git commit history instead)
- Post-mortem reports (capture lessons in architecture docs or commit messages)

**Reviews and analysis must focus on outstanding actions**, not documenting past work. Git history serves as the permanent record of changes and decisions.

## Current Audio System (March 2026)

### Detection Architecture
**Previous (v68):** FrameBeatNN — single FC model, FC(832→64→32→2), 56.8 KB INT8, W32 (0.5s). Beat F1=0.491, DB F1=0.238.
**Previous (v69):** Dual-model (OnsetNN + RhythmNN) — abandoned Mar 16. Every published system uses single joint model; split underperformed FC baseline.
**Current (v73, deployed):** Single Conv1D W64 with Beat This! sum head. Conv1D(26→24,k=5) → Conv1D(24→32,k=5) → Conv1D(32→2,k=1). 15.1 KB INT8, 27ms inference measured on device. Beat F1=0.480, DB F1=0.160. Arena: 7340/32768 bytes.
Fallback if model fails to load: mic_.getLevel() as simple energy ODF.
Design goal: trigger on kicks and snares only; hi-hats/cymbals create overly busy visuals. See [VISUALIZER_GOALS.md](docs/VISUALIZER_GOALS.md) for the full design philosophy.
Training data: consensus_v5 labels (7-system), cal63 mel calibration.

### Key Features
- **Single Conv1D NN** (deployed): Conv1D W64 [24,32] with Beat This! sum head, 15.1 KB INT8, 27ms. Multi-task: beat activation (ODF) + downbeat (constrained ≤ beat). Per-tensor INT8 quantization (CMSIS-NN requirement).
- **AGC removed** (v72): Hardware gain fixed at platform optimal (nRF52840: 32, ESP32-S3: 30). Window/range normalization is sole dynamic range system.
- **ODF information gate**: Suppresses low-confidence ODF when NN output is weak (prevents noise-driven false beats)
- **Pulse baseline tracking**: Floor-tracking baseline replaces running-mean threshold for pulse detection
- **Energy synthesis**: Hybrid mic level + bass mel energy + ODF peak-hold
- **Spectral conditioning** (v23+): Soft-knee compressor (Giannoulis 2012) → per-bin adaptive whitening
- **Bayesian tempo fusion**: 20-bin posterior over ~60-198 BPM, comb filter bank + ACF. SETTINGS_VERSION 73
- **Harmonic disambiguation**: Per-sample ACF check after MAP extraction, prefers 2x or 1.5x BPM when raw ACF is strong
- **Onset-density octave discriminator** (v32): Gaussian penalty on tempos where transients/beat < 0.5 or > 5.0
- **Shadow CBSS octave checker** (v32): Every 2 beats, compares CBSS score at T vs T/2; switches if T/2 scores 1.3x better
- **CBSS beat tracking**: Counter-based beat prediction with deterministic phase derivation, adaptive threshold (tuned for faster convergence)
- **Tempo-adaptive cooldown**: Shorter cooldown at faster tempos (min 40ms, max 150ms)
