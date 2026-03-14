# Claude Code Instructions for Blinky Project

## CRITICAL: Upload Safety

**NEVER use `arduino-cli upload` or `adafruit-nrfutil dfu serial`!**

- `arduino-cli upload` has race conditions in USB port re-enumeration
- `adafruit-nrfutil dfu serial` uses single-bank DFU that can leave firmware partially written
- Both methods can brick the device, requiring SWD hardware to recover

### Safe Upload Method: UF2

Use the UF2 upload script for safe CLI-based firmware upload:
```bash
make uf2-upload UPLOAD_PORT=/dev/ttyACM0
```

**Why UF2 is safe:**
- Invalid/corrupt UF2 files are silently rejected (cannot brick)
- Simple file copy to mass storage (no serial protocol race conditions)
- Bootloader protects itself (hardware-enforced)
- If interrupted, old firmware stays intact

See `tools/uf2_upload.py --help` for all options.

### CRITICAL: Pre-Flash Checklist

**Devices are physically installed — double-tap reset is NOT an option.**
Bootloader entry MUST succeed via software (serial command or 1200-baud touch).
A failed bootloader entry leaves the device running old firmware but wastes time.

**Before EVERY flash attempt:**
1. **Disconnect ALL MCP sessions** — `mcp__blinky-serial__disconnect` on every port, or verify `mcp__blinky-serial__status` shows no connections
2. **Wait 3 seconds** after MCP disconnect — the Node.js `SerialPort.close()` is async; the OS file descriptor may not be released immediately
3. **Do NOT flash immediately after interactive serial use** — always disconnect and wait
4. **Flash one device at a time** unless using `--parallel` mode

**Why this matters:** If an MCP server or console session holds the serial port, `uf2_upload.py` cannot send the bootloader entry command. The device resets but doesn't enter UF2 mode. The script retries 5 times (40+ seconds wasted), then fails. The `uf2_upload.py` script includes a port availability pre-check that will detect and report this condition.

### Safe Operations

**ALLOWED via CLI:**
- `arduino-cli compile` - Compiling is safe
- `make uf2-upload` - UF2 upload is safe (uses mass storage, not DFU serial)
- `make uf2-check` - Dry run (compile + validate + convert, no upload)
- `arduino-cli core list/install` - Core management is safe
- Reading serial ports is safe

**NEVER DO via CLI:**
- `arduino-cli upload` - Uses fragile DFU serial protocol
- `adafruit-nrfutil dfu serial` - Same protocol, same risk
- Any direct invocation of the DFU serial upload method

### If the Device Becomes Unresponsive

Devices are physically installed and reset buttons are NOT accessible.
If a device stops responding to serial commands:
1. Try power-cycling via USB hub: `uhubctl -a cycle -p <port>`
2. Re-run: `python3 tools/uf2_upload.py --build-dir /tmp/blinky-build /dev/ttyACMx`
3. If the port disappeared entirely, wait 10 seconds and check `ls /dev/ttyACM*`
4. Last resort: physically access the device and double-tap reset

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
# Compile only (in-tree build, requires TFLite library)
arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Sense blinky-things

# Compile + validate + upload via UF2 (recommended)
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
- **FrameBeatNN** (`blinky-things/audio/FrameBeatNN.h`) - Frame-level FC NN beat/downbeat activation (primary ODF)
- **SharedSpectralAnalysis** (`blinky-things/audio/SharedSpectralAnalysis.h`) - FFT → compressor → whitening → mel bands
- **AdaptiveMic** (`blinky-things/inputs/AdaptiveMic.h`) - Microphone input with AGC
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

The Blinky Time project consists of 4 major components:

```
blinky_time/
├── blinky-things/          Arduino firmware (nRF52840)
├── blinky-console/         React web UI (WebSerial interface)
├── blinky-test-player/     Node.js testing CLI (parameter tuning)
└── blinky-serial-mcp/      MCP server (AI integration)
```

### Firmware Architecture (blinky-things/)

**Core Pipeline: Generator → Effect → Renderer**

```
PDM Microphone (16 kHz)
    ↓
AdaptiveMic (AGC + normalization)
    ↓
SharedSpectralAnalysis (FFT-256 → compressor → whitening → mel bands)
    ↓
    ├── FrameBeatNN (frame-level FC, ~60-200µs) → ODF
    ↓
AudioController (CBSS beat tracking + pulse detection)
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
   - `AdaptiveMic.h` - PDM microphone with hardware/software AGC
   - `SharedSpectralAnalysis.h` - FFT-256 (128 freq bins @ 62.5 Hz), soft-knee compressor → per-bin whitening (v23+)
   - Window/range normalization (0-1 output)

2. **Onset Detection**
   - `FrameBeatNN.h` - **Sole ODF**: Frame-level FC neural network (up to 314 KB INT8, ~60-200µs inference)
   - Input: up to 192 frames × 26 raw mel bands (up to 3.07s window). Output: beat_activation + downbeat_activation
   - Non-NN fallback: `mic_.getLevel()` (energy envelope as simple ODF)

3. **Rhythm Tracking (AudioController)**
   - `AudioController.h/cpp` - Bayesian tempo fusion + CBSS beat tracking
   - OSS buffering (6 seconds @ 60 Hz)
   - ODF source: FrameBeatNN (frame-level FC, ~60-200µs). Falls back to mic level if model fails to load.
   - Bayesian tempo fusion: 20-bin posterior (~60-198 BPM), comb filter bank + harmonic-enhanced ACF (0.8, v25). FT/IOI disabled (v28)
   - Per-sample ACF harmonic disambiguation (2x and 1.5x checks after MAP extraction)
   - CBSS: cumulative beat strength signal with log-Gaussian transition weighting
   - BTrack-style predict+countdown beat detection with CBSS adaptive threshold (cbssthresh=1.0)
   - Deterministic phase derivation
   - ODF pre-smoothing (5-point causal moving average)
   - ODF mean subtraction disabled (v32: raw ODF preserves ACF structure)
   - Onset-density octave discriminator (v32: penalizes implausible tempos in posterior)
   - Shadow CBSS octave checker (v32: compares T vs T/2 every 2 beats)

4. **Generators (Visual Effects)**
   - `Fire.cpp/h` - Heat diffusion with sparks (13 params)
   - `Water.cpp/h` - Wave simulation with ripples
   - `Lightning.cpp/h` - Branching bolt effects
   - All generators consume `AudioControl` struct

5. **Post-Processing Effects**
   - `HueRotationEffect.h` - Color cycling
   - `NoOpEffect.h` - Pass-through (identity)
   - Effect chaining supported

6. **Configuration & Persistence**
   - `ConfigStorage.h/cpp` - Flash-based storage (SETTINGS_VERSION: v68)
   - `SettingsRegistry.h/cpp` - Tunable parameters (v70: ~30 after BandFlux removal)
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

**DO NOT lock gain** - Let the AGC auto-adapt for realistic testing conditions. Only use the `gain` parameter in rare cases where you need to isolate AGC behavior specifically.

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
1. PDM mic samples → AdaptiveMic (normalize 0-1, AGC)
2. AdaptiveMic → SharedSpectralAnalysis (FFT-256 → compressor → per-bin whitening → mel bands)
3. SharedSpectralAnalysis → FrameBeatNN (32-frame mel window → FC layers → beat + downbeat activation)
4. FrameBeatNN beat_activation → ODF value (0-1)
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
- RAM: ~40 KB base (CBSS/OSS ~3 KB + comb filters ~5.3 KB + Bayesian transition matrix ~3 KB + ODF linear buffer ~1.4 KB + 16 KB tensor arena + up to 19.5 KB mel frame buffer for W192).
- Flash: ~625 KB with W192 model (includes TFLite model + TFLite Micro runtime). ~30 KB settings storage.
- Available: 256 KB RAM, 1 MB Flash

**CPU (64 MHz):**
- Microphone + FFT: ~4%
- FrameBeatNN inference: <1%
- Autocorrelation (500ms): ~3% amortized
- CBSS + beat detection: ~1%
- Fire generator: ~5-8%
- LED rendering: ~2%
- **Total: ~15-20%** (ample headroom)

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
- ✅ AudioController with CBSS beat tracking
- ✅ FrameBeatNN (frame-level FC, up to 314 KB INT8, deployed on all devices)
- ✅ Fire/Water/Lightning generators
- ✅ Web UI (React + WebSerial)
- ✅ Testing infrastructure (MCP + param-tuner + batch A/B test scripts)
- ✅ Multi-layer safety mechanisms
- ✅ 3 device configurations (Hat, Tube, Bucket)
- ✅ Mic calibration pipeline + gain-aware training augmentation

**Removed (v64-v67):**
- v64: Forward filter, particle filter, HMM phase tracker, multi-agent beat tracking, template/subbeat/metrical octave checks, ODF sources 1-5, legacy spectral flux (~1500 lines)
- v67: EnsembleDetector, BandFlux, EnsembleFusion, BassSpectralAnalysis, IDetector, DetectionResult (~2600 lines, ~24 settings, ~22 KB flash, ~2 KB RAM saved)
- v68: Removed ENABLE_NN_BEAT_ACTIVATION ifdef and nnBeatActivation runtime toggle. FrameBeatNN always compiled in and active. TFLite is a required dependency.
- Spectral noise subtraction (`noiseest=0`): still in SharedSpectralAnalysis, default OFF

**In Progress:**
- NN mel calibration: target_rms_db corrected -35→-63 dB, dataset reprocessing underway
- Conv1D wide model evaluation (training complete, needs export and comparison)
- Window size sweep: configs for 16/32/48/64 frames ready to train

**Planned (Not Started):**
- Bluetooth/BLE support (design doc complete)
- Dynamic device switching (runtime config)
- CI/CD automation

**Closed (mel-spectrogram CNN, v4-v9):**
- All architectures (standard conv, BN-fused, DS-TCN) measured 79-98ms on Cortex-M4F — 8-10× over frame budget
- Superseded by frame-level FC approach (~60-200µs at 15.6 Hz)

**Closed (beat-synchronous hybrid, March 2026):**
- FC on accumulated spectral summaries at beat rate (~2 Hz). Circular dependency with CBSS, negligible discriminative power in per-beat features, misaligned with all leading approaches. Superseded by frame-level FC.

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
FrameBeatNN — frame-level FC neural network (sole ODF, v67). Up to FC(4992→64→32→2), up to 322K params, up to 314 KB INT8.
Input: 32 frames × 26 raw mel bands (0.5s window at 62.5 Hz). Output: beat_activation (ODF for CBSS) + downbeat_activation.
~60-200µs inference on Cortex-M4F. Deployed on all 3 devices (March 2026).
Fallback if model fails to load: mic_.getLevel() as simple energy ODF.
Pulse detection: ODF threshold against running mean with tempo-adaptive cooldown (inlined from removed EnsembleFusion).
Design goal: trigger on kicks and snares only; hi-hats/cymbals create overly busy visuals. See [VISUALIZER_GOALS.md](docs/VISUALIZER_GOALS.md) for the full design philosophy.
BandFlux/EnsembleDetector fully removed in v67 (~2600 lines, 10 files deleted).

### Key Features
- **FrameBeatNN** (v65+): Frame-level FC neural network, sole ODF source. Per-tensor INT8 quantization (CMSIS-NN requirement). ~60-200µs inference at ~15.6 Hz.
- **Spectral conditioning** (v23+): Soft-knee compressor (Giannoulis 2012) → per-bin adaptive whitening
- **Bayesian tempo fusion**: 20-bin posterior over ~60-198 BPM, comb filter bank + ACF. SETTINGS_VERSION 68
- **Harmonic disambiguation**: Per-sample ACF check after MAP extraction, prefers 2x or 1.5x BPM when raw ACF is strong
- **Onset-density octave discriminator** (v32): Gaussian penalty on tempos where transients/beat < 0.5 or > 5.0
- **Shadow CBSS octave checker** (v32): Every 2 beats, compares CBSS score at T vs T/2; switches if T/2 scores 1.3x better
- **CBSS beat tracking**: Counter-based beat prediction with deterministic phase derivation, adaptive threshold
- **Tempo-adaptive cooldown**: Shorter cooldown at faster tempos (min 40ms, max 150ms)
