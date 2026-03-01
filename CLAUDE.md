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

## Compilation Commands

```bash
# Compile only (in-tree build)
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
- **EnsembleDetector** (`blinky-things/audio/EnsembleDetector.h`) - 7 detectors with weighted fusion (BandFlux Solo default)
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
SharedSpectralAnalysis (FFT-256 → compressor → whitening)
    ↓
EnsembleDetector (BandWeightedFlux Solo)
    ↓
AudioController (CBSS beat tracking)
    ↓
AudioControl {energy, pulse, phase, rhythmStrength, onsetDensity}
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

2. **Transient Detection (Ensemble)**
   - `EnsembleDetector.h` - Weighted fusion of 7 detectors (1 enabled, 6 disabled)
   - **BandFlux Solo config (Feb 2026):** BandWeightedFlux (1.0, thresh 0.5) — all others disabled
   - BandFlux: log-compressed band-weighted spectral flux with additive threshold and onset delta filter
   - Agreement-based confidence scaling (single-detector boost 1.0 for solo config)
   - Adaptive cooldown (tempo-aware)

3. **Rhythm Tracking (AudioController)**
   - `AudioController.h/cpp` - Bayesian tempo fusion + CBSS beat tracking
   - OSS buffering (6 seconds @ 60 Hz)
   - Bayesian tempo fusion: 20-bin posterior (60-180 BPM), comb filter bank + harmonic-enhanced ACF (0.8, v25). FT/IOI disabled (v28)
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
   - `ConfigStorage.h/cpp` - Flash-based storage (SETTINGS_VERSION: v33)
   - `SettingsRegistry.h/cpp` - 70+ tunable parameters
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
2. AdaptiveMic → SharedSpectralAnalysis (FFT-256 → compressor → per-bin whitening)
3. SharedSpectralAnalysis → EnsembleDetector (BandFlux Solo, sees whitened magnitudes)
4. EnsembleDetector → fusion → transient strength (0-1)
5. Transient → AudioController OSS buffer (6s history)
6. AudioController → autocorrelation every 250ms → Bayesian tempo fusion
   (ACF + Fourier tempogram + comb filter bank + IOI → 20-bin posterior → harmonic disambig → MAP → BPM)
7. CBSS backward search → cumulative beat strength signal
8. Predict+countdown beat detection → deterministic phase
9. Output: AudioControl{energy=0.45, pulse=0.85, phase=0.12, rhythmStrength=0.75, onsetDensity=3.2}
10. Fire generator:
    - energy → baseline flame height
    - pulse → spark burst intensity
    - phase → breathing effect (0=on-beat)
    - rhythmStrength → blend music/organic mode
    - onsetDensity → content classification (dance=2-6/s, ambient=0-1/s)
11. Fire heat diffusion (matrix propagation)
12. HueRotationEffect (optional color shift)
13. RenderPipeline → LED strip output
```

### Resource Usage (nRF52840)

**Memory:**
- RAM: ~21 KB total (20,872B measured; CBSS/OSS ~3 KB + comb filters ~10 KB + Bayesian transition matrix ~6 KB + ODF linear buffer ~1.4 KB)
- Flash: ~270 KB firmware, ~30 KB settings storage
- Available: 256 KB RAM, 1 MB Flash

**CPU (64 MHz):**
- Microphone + FFT: ~4%
- Ensemble detector: ~1%
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

### Current Status (February 2026)

**Production Ready:**
- ✅ AudioController with CBSS beat tracking
- ✅ BandFlux Solo detector (log-compressed band-weighted spectral flux)
- ✅ Fire/Water/Lightning generators
- ✅ Web UI (React + WebSerial)
- ✅ Testing infrastructure (MCP + param-tuner)
- ✅ Multi-layer safety mechanisms
- ✅ 3 device configurations (Hat, Tube, Bucket)

**In Progress:**
- Beat tracking octave disambiguation (v32 avg Beat F1 0.265, best-device 0.302 on 18 tracks; double-time lock at ~182 BPM remains primary bottleneck)
- Full hardware installation validation

**Planned (Not Started):**
- Bluetooth/BLE support (design doc complete)
- Dynamic device switching (runtime config)
- CI/CD automation

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

## Current Audio System (February 2026)

### Ensemble Detection Architecture
The system uses 7 detectors with weighted fusion (1 enabled, 6 disabled).
Design goal: trigger on kicks and snares only; hi-hats/cymbals create overly busy visuals. See [VISUALIZER_GOALS.md](docs/VISUALIZER_GOALS.md) for the full design philosophy.

| Detector | Weight | Thresh | Enabled | Notes |
|----------|--------|--------|---------|-------|
| **BandWeightedFlux** | 1.00 | 0.5 | **Yes** | Log-compressed band-weighted spectral flux, additive threshold, onset delta filter |
| Drummer | 0.50 | 4.5 | No | Good kick/snare recall, but BandFlux Solo outperforms (+14% avg Beat F1) |
| ComplexDomain | 0.50 | 3.5 | No | Good precision, but adds noise when combined with BandFlux |
| SpectralFlux | 0.20 | 1.4 | No | Fires on pad chords |
| HFC | 0.20 | 4.0 | No | Hi-hat detector, misses kicks |
| BassBand | 0.45 | 3.0 | No | Too noisy (100+ detections/30s) |
| Novelty | 0.12 | 2.5 | No | Near-zero detections on real music |

### Key Features
- **BandFlux Solo**: Single detector outperforms multi-detector combos
- **Spectral conditioning** (v23+): Soft-knee compressor (Giannoulis 2012) → per-bin adaptive whitening. Magnitudes modified in-place; totalEnergy/centroid reflect pre-whitened state
- **Bayesian tempo fusion**: 20-bin posterior over 60-200 BPM, comb filter bank + ACF (FT/IOI disabled v28). SETTINGS_VERSION 33
- **Harmonic disambiguation**: Per-sample ACF check after MAP extraction, prefers 2x or 1.5x BPM when raw ACF is strong
- **ODF mean subtraction disabled** (v32): Raw ODF feeds ACF — global mean sub was destroying peak structure (+70% F1)
- **Onset-density octave discriminator** (v32): Gaussian penalty on tempos where transients/beat < 0.5 or > 5.0 (+13% F1)
- **Shadow CBSS octave checker** (v32): Every 2 beats, compares CBSS score at T vs T/2; switches if T/2 scores 1.3x better (+13% F1)
- **CBSS adaptive threshold**: Beat fires only if CBSS > cbssthresh * running mean (prevents phantom beats during silence)
- **Adaptive cooldown**: Tempo-aware cooldown (shorter at faster BPMs, min 40ms, max 150ms)
- **CBSS beat tracking**: Counter-based beat prediction with deterministic phase derivation
- **Onset delta filter**: Rejects slow-rising pads/swells (minOnsetDelta=0.3)
- **Shared FFT + spectral pipeline**: All detectors share a single FFT → compressor → whitening chain
- **Disabled detectors use zero CPU**: Only enabled detectors are processed each frame
