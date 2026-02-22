# Claude Code Instructions for Blinky Project

## CRITICAL: NEVER FLASH FIRMWARE VIA CLI

**DO NOT use arduino-cli, Bash, or any command-line tool to upload/flash firmware!**

- `arduino-cli upload` WILL BRICK THE DEVICE
- The device CANNOT be recovered without SWD hardware (J-Link, etc.)
- This is due to a Seeeduino mbed platform bug

### Safe Operations

**ALLOWED via CLI:**
- `arduino-cli compile` - Compiling is safe
- `arduino-cli core list/install` - Core management is safe
- Reading serial ports is safe

**NEVER DO via CLI:**
- `arduino-cli upload` - WILL CORRUPT BOOTLOADER
- Any command that writes to the device

### If the Device Becomes Unresponsive

1. Double-tap the reset button quickly (like double-click)
2. A drive letter should appear (e.g., "XIAO-SENSE")
3. The user can then flash via Arduino IDE

### Why This Happens

The Seeeduino mbed platform's arduino-cli upload routine starts writing
firmware before properly verifying the bootloader state, causing partial
writes that corrupt the bootloader region.

## Compilation Commands

Use the arduino-cli for compilation only:
```bash
arduino-cli compile --fqbn Seeeduino:mbed:xiaonRF52840Sense blinky-things
```

**User must flash via Arduino IDE after compilation.**

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
- **AudioControl struct** (`blinky-things/audio/AudioControl.h`) - Output: energy, pulse, phase, rhythmStrength

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
EnsembleDetector (BandWeightedFlux Solo)
    ↓
AudioController (CBSS beat tracking)
    ↓
AudioControl {energy, pulse, phase, rhythmStrength}
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
   - `SharedSpectralAnalysis.h` - FFT-256 (128 freq bins @ 62.5 Hz)
   - Window/range normalization (0-1 output)

2. **Transient Detection (Ensemble)**
   - `EnsembleDetector.h` - Weighted fusion of 7 detectors (1 enabled, 6 disabled)
   - **BandFlux Solo config (Feb 2026):** BandWeightedFlux (1.0, thresh 0.5) — all others disabled
   - BandFlux: log-compressed band-weighted spectral flux with additive threshold and onset delta filter
   - Agreement-based confidence scaling (single-detector boost 1.0 for solo config)
   - Adaptive cooldown (tempo-aware)

3. **Rhythm Tracking (AudioController)**
   - `AudioController.h/cpp` - CBSS beat tracking
   - OSS buffering (6 seconds @ 60 Hz)
   - Autocorrelation every 500ms with Gaussian tempo prior
   - CBSS: cumulative beat strength signal with log-Gaussian transition weighting
   - BTrack-style predict+countdown beat detection with deterministic phase
   - ODF pre-smoothing (5-point causal moving average)

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
   - `ConfigStorage.h/cpp` - Flash-based storage (CONFIG_VERSION: v19)
   - `SettingsRegistry.h/cpp` - 56+ tunable parameters
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
2. AdaptiveMic → EnsembleDetector (BandFlux Solo)
3. EnsembleDetector → fusion → transient strength (0-1)
4. Transient → AudioController OSS buffer (6s history)
5. AudioController → autocorrelation every 500ms → tempo estimation
6. CBSS backward search → cumulative beat strength signal
7. Predict+countdown beat detection → deterministic phase
8. Output: AudioControl{energy=0.45, pulse=0.85, phase=0.12, rhythmStrength=0.75}
9. Fire generator:
   - energy → baseline flame height
   - pulse → spark burst intensity
   - phase → breathing effect (0=on-beat)
   - rhythmStrength → blend music/organic mode
10. Fire heat diffusion (matrix propagation)
11. HueRotationEffect (optional color shift)
12. RenderPipeline → LED strip output
```

### Resource Usage (nRF52840)

**Memory:**
- RAM: ~16 KB total (baseline 10 KB + CBSS/OSS buffers ~3 KB + comb filters ~5 KB)
- Flash: ~222 KB firmware, ~30 KB settings storage
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
- Parameter boundary refinement (extended range testing)
- Pad rejection optimization (50-229 false positives)
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
- **BandFlux Solo**: Single detector outperforms multi-detector combos (avg Beat F1 0.472 across 9 tracks)
- **Agreement-based confidence**: Single-detector boost 1.0 (full pass-through for solo config)
- **Adaptive cooldown**: Tempo-aware cooldown (shorter at faster BPMs, min 40ms, max 150ms)
- **CBSS beat tracking**: Counter-based beat prediction with deterministic phase derivation
- **Onset delta filter**: Rejects slow-rising pads/swells (minOnsetDelta=0.3)
- **Post-onset decay gate**: Defers confirmation N frames, rejects if flux stays elevated (pads sustain, kicks decay). decayRatio+decayFrames, default 0.0=disabled
- **Spectral crest factor gate**: Rejects tonal onsets with high spectral peakiness (crestGate, default 0.0=disabled)
- **Band-dominance gate**: Rejects broadband onsets (bassRatioGate, default 0.0=disabled, tested ineffective)
- **Shared FFT**: All spectral detectors share a single FFT computation
- **Disabled detectors use zero CPU**: Only enabled detectors are processed each frame
