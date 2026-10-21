# Changelog

> **This file is no longer maintained** (last updated 2025). Firmware releases are versioned `bN`, not semver. The record of changes is:
> - [GitHub Releases](https://github.com/Jdubz/blinky_time/releases): each release's notes list the changes since the previous one and flag `SETTINGS_VERSION` changes (see [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md)).
> - `git log` for everything else.
>
> The entries below are kept for history only and do not describe the current firmware.

## [Unreleased]

### Added
- Visual effects abstraction architecture
- Comprehensive fire effect testing framework
- Serial console test commands (`test all`, `test fire`, etc.)
- Automated CI/CD workflow with staging and production releases
- Branch protection strategy with automated versioning

### Changed
- Refactored fire effect to use EffectMatrix for better testability
- Fixed fire effect color generation (was showing green instead of red)
- Enhanced serial console with effect testing capabilities
- **BREAKING:** Refactored onset detection from 3-band to 2-band system
  - Removed mid-frequency band (was 500-2000 Hz)
  - Now uses only low (50-200 Hz) and high (2-8 kHz) bands
  - Transient events now report 'low' or 'high' type only
  - Removed `midthresh` serial command
  - Improved first-frame detection (no longer blocked by zero-energy history)

### Fixed
- EEPROM compilation errors on nRF52840 platform
- Fire effect color channel mapping (RGB vs GRB)

## [1.0.0] - 2025-01-15

### Added
- Initial release of Blinky Time LED fire effect controller
- Support for three device configurations:
  - Hat: 89 LEDs in string configuration
  - Tube Light: 4x15 zigzag matrix (60 LEDs)
  - Bucket Totem: 16x8 matrix (128 LEDs)
- Realistic fire simulation with heat diffusion
- Audio-reactive visual effects with adaptive microphone
- Battery monitoring and management
- IMU-based motion sensing
- Serial console for parameter tuning
- Configuration storage system
- Comprehensive build system and documentation

### Technical Details
- **Hardware**: nRF52840 XIAO Sense with WS2812B LED strips
- **Memory Usage**: ~13% flash, ~4% RAM (very efficient)
- **Frame Rate**: ~60 FPS fire animation
- **Audio Latency**: ~16ms from sound to LED response

[Unreleased]: https://github.com/Jdubz/blinky_time/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Jdubz/blinky_time/releases/tag/v1.0.0