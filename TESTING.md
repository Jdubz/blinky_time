# Testing Guide

All testing is managed through blinky-server (REST API) and the MCP tools.

## Testing Documentation

| Document | Purpose |
|----------|---------|
| [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) | **Primary guide** - Tunable parameters, test procedures, troubleshooting |
| [blinky-test-player/PARAMETER_TUNING_HISTORY.md](blinky-test-player/PARAMETER_TUNING_HISTORY.md) | Historical calibration results |

## Quick Start

### Run Tests via blinky-server API

```bash
# Run validation suite (all tracks, single device)
curl -X POST http://blinkyhost.local:8420/api/test/validate \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["DEVICE_ID"], "track_dir": "/path/to/music/edm"}'

# Parameter sweep (3 devices, 9 values)
curl -X POST http://blinkyhost.local:8420/api/test/param-sweep \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["D1","D2","D3"], "param_name": "onsetthresh", "values": [1,2,3,4,5,6,7,8,9], "track_dir": "/path/to/music/edm"}'

# Poll for results
curl http://blinkyhost.local:8420/api/test/jobs/{job_id}
```

### Run Tests via MCP

```
# Validation test (from Claude Code)
run_test(port: "/dev/ttyACM0")

# Full validation suite
run_validation_suite(ports: ["/dev/ttyACM0", "/dev/ttyACM1"])

# Poll results
check_test_result(job_id: "abc123")
```

### Compile Check

```bash
# Verify firmware compiles without errors
arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Sense blinky-things
```

See [docs/AUDIO-TUNING-GUIDE.md](docs/AUDIO-TUNING-GUIDE.md) for the full parameter reference.
