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

### Run Transient Detection Tests via MCP

**Always use `run_test`** - it automatically connects, runs the test, and disconnects:

```
# MCP tool call (from Claude Code or other MCP client)
run_test(pattern: "steady-120bpm", port: "COM11", gain: 40)
```

The `run_test` tool:
1. Connects to the device on the specified port
2. Optionally locks hardware gain for consistent testing
3. Plays the audio pattern via Playwright
4. Records all detected transients
5. **Automatically disconnects** when complete (frees port for flashing)

**Do NOT use manual connect/disconnect** for pattern testing - this risks leaving the port locked.

### Run Tests via CLI

```bash
# List available test patterns
cd blinky-test-player
npx blinky-test-player list

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
