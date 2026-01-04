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
| `MUSIC_MODE_SIMPLIFIED.md` | AudioController v2 architecture (autocorrelation-based rhythm tracking) |
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
| `SAFETY.md` | Critical safety guidelines for flashing |

### Key Architecture Components

- **AudioController** (`blinky-things/audio/AudioController.h`) - Unified audio analysis
- **EnsembleDetector** (`blinky-things/audio/EnsembleDetector.h`) - 6 simultaneous detectors with weighted fusion
- **AdaptiveMic** (`blinky-things/inputs/AdaptiveMic.h`) - Microphone input with AGC
- **AudioControl struct** (`blinky-things/audio/AudioControl.h`) - Output: energy, pulse, phase, rhythmStrength

### Obsolete Documents (Removed)

The following were deleted as outdated (December 2025):
- `RHYTHM-TRACKING-REFACTOR-PROPOSAL.md` - Replaced by AudioController implementation
- `TUNING-PLAN.md` - Superseded by AUDIO-TUNING-GUIDE.md
- `docs/plans/MUSIC_MODE_TESTING_PLAN.md` - Referenced old MusicMode/PLL
- `docs/plans/TRANSIENT_DETECTION_TESTING.md` - Work completed, merged into AUDIO-TUNING-GUIDE.md

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
