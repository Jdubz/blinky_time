# Contributing to Blinky Time

Thank you for your interest in contributing to Blinky Time! This document provides guidelines for contributing to this open-source LED fire effect controller project.

## 🤝 Code of Conduct

By participating in this project, you agree to maintain a respectful and inclusive environment for all contributors.

## 🚀 Getting Started

### Prerequisites

- Git, Python 3 and bash (Git Bash on Windows)
- The pinned build toolchain: run `bash scripts/setup_toolchain.sh` once. It installs the arduino-cli core, libraries, TFLite and core patches pinned in `tools/firmware_toolchain.json` into `.arduino/`. Don't install libraries anywhere else to "fix" a build.
- A test device for hardware testing (recommended). Test devices are listed in `devices/test_devices.json`.

**Read [CLAUDE.md](CLAUDE.md) before touching hardware.** It holds the flashing and release rules. In short: never use `arduino-cli upload`, the Arduino IDE Upload button or `adafruit-nrfutil dfu serial` on the nRF52840.

### Development Setup

1. **Fork the repository** on GitHub
2. **Clone your fork** locally:
   ```bash
   git clone https://github.com/YOUR_USERNAME/blinky_time.git
   cd blinky_time
   bash scripts/setup_toolchain.sh
   ```
3. **Create a feature branch** from master:
   ```bash
   git checkout master
   git pull origin master
   git checkout -b fix/your-change
   ```
4. **Make your changes**, compile with `./scripts/build.sh`, and test
5. **Commit and push** your changes
6. **Open a pull request to `master`** (see Workflow below)

## 📝 Contribution Types

We welcome various types of contributions:

### Code Contributions
- Bug fixes
- New features
- Performance improvements
- Code cleanup and refactoring
- New device configurations

### Documentation
- README improvements
- Code comments and documentation
- Example code

### Testing
- Hardware compatibility testing
- Bug reports with detailed reproduction steps
- Performance testing and optimization

### Design
- Circuit diagrams and schematics
- 3D printable enclosures
- Wiring guides and installation documentation

## 🛠 Development Guidelines

### Code Style

- **Indentation**: match the file (4 spaces in most of `blinky-things/`; no tabs)
- **Naming**: Use camelCase for variables and functions, PascalCase for classes
- **Comments**: Document complex algorithms and hardware-specific code
- **Line Length**: Keep lines under 100 characters when practical

### File Organization

- **Main sketch**: `blinky-things/blinky-things.ino`
- **Firmware code**: subfolders of `blinky-things/` (see [blinky-things/README.md](blinky-things/README.md)). A new `.cpp` in a subfolder must be added to `BlinkyImplementations.h`, or Arduino won't compile it.
- **Device configs**: JSON files in `devices/registry/` (uploaded at runtime, not compiled in)
- **Documentation**: Additional docs in `docs/` directory (index: [docs/README.md](docs/README.md))
- **No silent fallbacks**: invalid state must fail loudly (see CLAUDE.md, "No Silent Fallbacks")

### Commit Messages

Use clear, descriptive commit messages:
```
feat: add support for 8x8 matrix configuration
fix: correct zigzag mapping for tube lights
docs: update installation instructions for Windows
test: add battery monitoring validation
```

## 🧪 Testing

### Automated Tests

Every pull request runs `.github/workflows/pr-validation.yml`: compile with the pinned toolchain, cppcheck, host tests (OTA manifest, absolute timing, tracker harness), blinky-server and blinky-console tests, and more. See [TESTING.md](TESTING.md) and [tests/README.md](tests/README.md) for running them locally.

### Hardware Testing

When contributing code changes:

1. **Test on a test device** listed in `devices/test_devices.json`. Flash it with `python tools/deploy_local.py --port <port> --device-id <id> --dev` (a dev build of a clean, committed tree).
2. **Never flash a dev build to any other device.** Only stable releases go to fleet and personal devices ([docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md)).
3. **Leave at least 60 s of uptime between resets** (see CLAUDE.md, "60-second rule").
4. **Validate audio responsiveness** on the validation corpus (`blinky-test-player/music/edm/`).

### Testing Checklist

- [ ] `./scripts/build.sh` compiles without warnings
- [ ] Generators display correctly
- [ ] Audio responsiveness works as expected
- [ ] Battery monitoring works (battery-powered devices)
- [ ] Serial console commands respond correctly
- [ ] LED colors and brightness are accurate

## 🌿 Branching Workflow

All work branches from `master` and merges back to `master` by pull request. There is no `develop` or `staging` flow any more. See [.github/BRANCHING_STRATEGY.md](.github/BRANCHING_STRATEGY.md).

- **`master`** - Integration branch. PRs need the required CI checks to pass.
- **`fix/...`, `feat/...`, `docs/...`** - Short-lived branches, one change each.

Merging to master does **not** ship firmware. A release is a separate step: CI builds a candidate from master, it is bench-gated on a test device, then promoted to stable ([docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md)).

## 📋 Pull Request Process

### Before Submitting

1. **Ensure your code compiles** without errors or warnings
2. **Test on hardware** if changes affect device functionality
3. **Update documentation** if you've changed APIs or added features
4. **Write clear commit messages** describing your changes

### Pull Request Template

When submitting a pull request, please include:

```markdown
## Description
Brief description of the changes and their purpose.

## Type of Change
- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to not work as expected)
- [ ] Documentation update

## Hardware Tested
- [ ] Test device (which one, which build)
- [ ] No hardware testing required

## Testing
Describe the tests you ran and their results.

## Screenshots/Videos
If applicable, add visual documentation of your changes.
```

### Review Process

1. **Automated checks** will run on your pull request
2. **Maintainer review** will check code quality and functionality
3. **Hardware testing** may be requested for significant changes
4. **Merge** will occur after approval and successful testing

## 🐛 Bug Reports

### Before Reporting

1. **Search existing issues** to avoid duplicates
2. **Test with the latest stable release** (or current `master`)
3. **Isolate the problem** to specific conditions

### Bug Report Template

```markdown
**Hardware Configuration**
- Device ID: [e.g. hat_v1, from `json info`]
- Board: [nRF52840 XIAO Sense]
- LEDs: [Count and type]
- Power: [Battery/USB/External]

**Software Environment**
- Firmware version: [e.g. b214, from `json info`]
- Bootloader version (if relevant):
- Operating System:

**Description**
Clear description of the bug and expected behavior.

**Steps to Reproduce**
1. Step one
2. Step two
3. Step three

**Additional Context**
Serial output, photos, or videos if helpful.
```

## 💡 Feature Requests

We encourage feature suggestions! Please include:

- **Use case**: Describe the problem you're trying to solve
- **Proposed solution**: Your idea for implementing the feature
- **Hardware requirements**: Any specific hardware needs
- **Alternatives considered**: Other approaches you've thought about

## 📚 Documentation

### Code Documentation

- **Header comments** for all functions explaining purpose and parameters
- **Inline comments** for complex algorithms or hardware-specific code
- **Configuration comments** explaining parameter effects and valid ranges

### Project Documentation

- Keep README.md up to date with new features
- Document hardware setup procedures
- Provide example configurations for new device types
- Update troubleshooting guides with common issues

## 🏷 Versioning

Firmware is versioned by build number: `bN` for releases, `bN-dev-<sha>` for dev builds. The release workflow assigns `N`; `scripts/build.sh` writes it into `types/Version.h`. The saved-settings format has its own `SETTINGS_VERSION` (`config/ConfigStorage.h`); bumping it resets saved tuning on every device, so call it out in the PR. See [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md).

## 📄 License

By contributing to this project, you agree that your contributions will be licensed under the Creative Commons Attribution-ShareAlike 4.0 International License.

## 📞 Questions?

- **General questions**: Use GitHub Issues
- **Bug reports**: Use GitHub Issues
- **Feature requests**: Use GitHub Issues with "enhancement" label

Thank you for contributing to Blinky Time! 🔥