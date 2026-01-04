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
| `docs/AUDIO_ARCHITECTURE.md` | AudioController architecture (autocorrelation-based rhythm tracking) |
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
- **EnsembleDetector** (`blinky-things/audio/EnsembleDetector.h`) - 6 simultaneous detectors with weighted fusion
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
- `docs/MULTI_HYPOTHESIS_OPEN_QUESTIONS_ANSWERED.md` - Merged into MULTI_HYPOTHESIS_TRACKING_PLAN.md (Jan 2026)
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
EnsembleDetector (6 algorithms + fusion)
    ↓
AudioController v3 (multi-hypothesis tracking)
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
   - `EnsembleDetector.h` - Weighted fusion of 6 detectors
   - Detectors (weights): Drummer (0.22), SpectralFlux (0.20), BassBand (0.18), HFC (0.15), ComplexDomain (0.13), MelFlux (0.12)
   - Agreement-based confidence scaling (1-6 detectors)
   - Cooldown: 80ms (prevents false positives)

3. **Rhythm Tracking (AudioController v3)**
   - `AudioController.h/cpp` - Multi-hypothesis tempo tracking
   - OSS buffering (6 seconds @ 60 Hz)
   - Autocorrelation every 500ms
   - 4 concurrent tempo hypotheses (LRU eviction)
   - Confidence-based promotion (≥8 beat requirement)
   - Dual decay: phrase-aware (32-beat half-life) + silence (5s half-life)

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
- 40+ test patterns (simple-beat, complex-rhythm, polyrhythmic, etc.)

**blinky-serial-mcp (AI Integration)**
- 20+ MCP tools for device interaction
- Connection management (list_ports, connect, status)
- Settings control (get_settings, set_setting, save_settings)
- Audio streaming (stream_start, get_audio, monitor_audio)
- Testing (run_test, start_test, stop_test)
- Pattern library (list_patterns)

**Unit & Integration Tests**
- `tests/unit/` - Device configs, LED mapping, parameter bounds
- `tests/integration/` - Generator output, effect chaining, serial commands
- Custom BlinkyTest.h framework (Arduino-compatible)

### Data Flow Example (Fire Effect)

```
1. PDM mic samples → AdaptiveMic (normalize 0-1, AGC)
2. AdaptiveMic → EnsembleDetector (6 parallel algorithms)
3. EnsembleDetector → fusion → transient strength (0-1)
4. Transient → AudioController OSS buffer (6s history)
5. AudioController → autocorrelation every 500ms
6. Extract 4 tempo peaks → 4 hypothesis slots
7. Update confidence, promote if >0.15 advantage + ≥8 beats
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
- RAM: ~11 KB total (baseline 10 KB + multi-hypothesis 1 KB)
- Flash: ~172 KB firmware, ~30 KB settings storage
- Available: 256 KB RAM, 1 MB Flash

**CPU (64 MHz):**
- Microphone + FFT: ~4%
- Ensemble detector: ~1%
- Autocorrelation (500ms): ~3% amortized
- Hypothesis updates: ~1%
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

### Current Status (January 2026)

**Production Ready:**
- ✅ AudioController v3 (multi-hypothesis tracking)
- ✅ 6-detector ensemble (calibrated weights)
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

## Current Audio System (December 2025)

### Ensemble Detection Architecture
The system uses 6 simultaneous detectors with weighted fusion:

| Detector | Weight | Specialty |
|----------|--------|-----------|
| Drummer | 0.22 | Time-domain amplitude transients |
| SpectralFlux | 0.20 | SuperFlux algorithm, robust recall |
| BassBand | 0.18 | Low-frequency kick/bass detection |
| HFC | 0.15 | High-frequency percussive attacks |
| ComplexDomain | 0.13 | Phase-based soft onset detection |
| MelFlux | 0.12 | Perceptually-scaled detection |

### Key Features
- **Agreement-based confidence**: Single-detector hits are suppressed (0.6x), multi-detector consensus is boosted (up to 1.2x)
- **Cooldown = 80ms**: Reduces false positives from echo/reverb
- **Autocorrelation rhythm tracking**: Replaced legacy PLL-based tracking
- **Shared FFT**: All spectral detectors share a single FFT computation

### Legacy Mode Switching (REMOVED)
The old `detectionMode` parameter and mode-switching code has been removed. All 6 detectors now run simultaneously.
