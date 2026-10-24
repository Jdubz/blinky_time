# Testing Guide

There are two kinds of tests:
- **Host tests** run firmware code on a PC (no device needed). CI runs them on every PR. See [tests/README.md](tests/README.md).
- **On-device tests** play music to real devices and score what they detect. They are managed through blinky-server (REST API) and the MCP tools.

## Testing Documentation

| Document | Purpose |
|----------|---------|
| [tests/README.md](tests/README.md) | Host tests (timing, OTA manifest, tracker harness, parity) and the CI jobs that run them |
| [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) | **Primary on-device guide** - Tunable parameters, test procedures, troubleshooting |
| [docs/TESTING_METHODOLOGY.md](docs/TESTING_METHODOLOGY.md) | Tiered testing approach, from the host harness up to full on-device validation |
| [blinky-test-player/PARAMETER_TUNING_HISTORY.md](blinky-test-player/PARAMETER_TUNING_HISTORY.md) | Historical calibration results |

The only validation corpus is `blinky-test-player/music/edm/` (18 tracks). Ground truth is each track's `.onsets_consensus.json`: the task is onset detection, not beat detection (see [CLAUDE.md](CLAUDE.md)).

## Quick Start

### Compile Check

```bash
# Verify firmware compiles (pinned toolchain; run scripts/setup_toolchain.sh once first)
./scripts/build.sh --no-bump
```

### Host Tests

```bash
# Rhythm tracker on the edm/ corpus, no device needed (build steps in tests/tracker/README.md)
python tests/tracker/run_tracker_eval.py --harness build/tracker_harness
```

### Run Tests via blinky-server API

Run these against test devices, not installed fleet devices.

```bash
# Run validation suite (all tracks, single device)
curl -X POST http://blinkyhost.local:8420/api/test/validate \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["DEVICE_ID"], "track_dir": "/path/to/blinky-test-player/music/edm"}'

# Parameter sweep (3 devices, 3 values)
curl -X POST http://blinkyhost.local:8420/api/test/param-sweep \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["D1","D2","D3"], "param_name": "fluxdelta", "values": [8, 11, 14], "track_dir": "/path/to/blinky-test-player/music/edm"}'

# Poll for results
curl http://blinkyhost.local:8420/api/test/jobs/{job_id}
```

### Run Tests via MCP

```
# Validation test (from Claude Code); port is a serial port or device ID
run_test(port: "/dev/ttyACM0")

# Full validation suite
run_validation_suite(ports: ["/dev/ttyACM0", "/dev/ttyACM1"])

# Poll results
check_test_result(job_id: "abc123")
```

See [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) for the full parameter reference.
