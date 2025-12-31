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
- **AdaptiveMic** (`blinky-things/inputs/AdaptiveMic.h`) - 5 transient detection modes
- **AudioControl struct** (`blinky-things/audio/AudioControl.h`) - Output: energy, pulse, phase, rhythmStrength

### Obsolete Documents (Removed)

The following were deleted as outdated (December 2025):
- `RHYTHM-TRACKING-REFACTOR-PROPOSAL.md` - Replaced by AudioController implementation
- `TUNING-PLAN.md` - Superseded by AUDIO-TUNING-GUIDE.md
- `docs/plans/MUSIC_MODE_TESTING_PLAN.md` - Referenced old MusicMode/PLL
- `docs/plans/TRANSIENT_DETECTION_TESTING.md` - Work completed, merged into AUDIO-TUNING-GUIDE.md

## Current Audio System (December 2025)

- **Detection Mode 4 (Hybrid)** is recommended for general use
- **Equal weights (0.5/0.5)** outperform the original 0.7/0.3 flux/drummer split
- **Cooldown = 80ms** reduces false positives
- **PLL tracking was removed** - replaced by autocorrelation-based phase tracking
