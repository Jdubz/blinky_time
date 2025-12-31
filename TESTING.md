# Testing Guide

This project has a comprehensive testing and parameter tuning system for audio-reactive LED effects.

## Testing Documentation

| Document | Purpose |
|----------|---------|
| [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) | **Primary guide** - All tunable parameters, test procedures, troubleshooting |
| [docs/TESTING_SUMMARY.md](docs/TESTING_SUMMARY.md) | Testing framework overview (unit tests, integration tests, CI/CD) |
| [blinky-test-player/PARAMETER_TUNING_HISTORY.md](blinky-test-player/PARAMETER_TUNING_HISTORY.md) | Historical calibration results |
| [blinky-test-player/NEXT_TESTS.md](blinky-test-player/NEXT_TESTS.md) | Priority testing tasks |

## Quick Start

### Run Transient Detection Tests

```bash
# List available test patterns
cd blinky-test-player
npx blinky-test-player list

# Run a quick test via MCP
run_test --pattern strong-beats --port COM5 --gain 40

# Fast binary search tuning (~30 min)
npm run tuner -- fast --port COM5 --gain 40

# Full validation suite
npm run tuner -- validate --port COM5 --gain 40
```

### Run Unit Tests

```bash
# Windows - Test all device compilations
tests\test.bat

# Python - Complete test suite with hardware
python tests/run_tests.py --device 2 --port COM4 --report
```

See [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) for the full test plan and parameter reference.
