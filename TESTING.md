# Testing Guide

This project has a comprehensive testing and parameter tuning system for audio-reactive LED effects.

## Testing Documentation

| Document | Purpose |
|----------|---------|
| [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) | **Primary guide** - All tunable parameters, test procedures, troubleshooting |
| [blinky-test-player/PARAMETER_TUNING_HISTORY.md](blinky-test-player/PARAMETER_TUNING_HISTORY.md) | Historical calibration results |
| [blinky-test-player/NEXT_TESTS.md](blinky-test-player/NEXT_TESTS.md) | Priority testing tasks |

## Quick Start

### Run A/B Tests via CLI (blinkyhost)

```bash
# Multi-device A/B test (18 EDM tracks, 3 devices)
cd blinky-test-player
NODE_PATH=node_modules node ../ml-training/tools/ab_test_nnbeat.cjs
```

### Run Tests via MCP

**Always use `run_test`** - it automatically connects, runs the test, and disconnects:

```
# MCP tool call (from Claude Code or other MCP client)
run_test(pattern: "steady-120bpm", port: "/dev/ttyACM0")
```

### Run Unit Tests

```bash
# Compile all device configs (verifies no build errors)
arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Sense blinky-things
```

See [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) for the full test plan and parameter reference.
