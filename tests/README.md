# Testing Guide

This guide explains the tests in `tests/`: what each one checks, how to run it, and which CI job runs it. For on-device tests (playing music to devices through blinky-server), see [TESTING.md](../TESTING.md).

## 🎯 Testing Philosophy

The testing system is designed to:
- **Prevent regressions** by running the real firmware code, not a copy of it
- **Run without hardware** wherever possible, so CI can run it on every PR
- **Fail loudly**: an assert, crash or undefined behavior fails the test (see CLAUDE.md, "No Silent Fallbacks")

## 🧪 Host Tests (run on a PC)

Each test compiles firmware sources from `blinky-things/` for the host, using the Arduino stubs in `blinky-simulator/include/`. Any C++17 compiler works (g++, clang++, or `python -m ziglang c++` on Windows). All run in `.github/workflows/pr-validation.yml`.

| Directory | What it checks | CI job | How to run |
|-----------|----------------|--------|------------|
| `timing/` | Animations and the rhythm phase keep absolute time: one 2 s frame ends where two hundred 10 ms frames would (#259-#261). Built with UBSan. | `Absolute Timing` | Build command at the top of `timing/timing_test.cpp` (same as CI) |
| `ota/` | The firmware's signed-OTA manifest verifier and the release signer (`tools/ota_manifest.py`) agree byte for byte | `OTA Manifest` | [ota/README.md](ota/README.md) |
| `tracker/` | The real `AudioTracker` on recorded audio: UBSan stress, frame-rate gate, and a corpus gate on `blinky-test-player/music/edm/`; optionally with the real onset NN | `Tracker Harness`, `Tracker Harness with real onset NN` | [tracker/README.md](tracker/README.md) |
| `parity/` | Firmware spectral feature math matches the Python training features | not in CI (run locally); CI's `Feature Parity` job runs only the Python unit tests | [parity/README.md](parity/README.md) |

### Quick Start

```bash
# Absolute timing test (from the repo root)
mkdir -p build
g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=undefined,float-cast-overflow,float-divide-by-zero \
  -fno-sanitize-recover=all -DARDUINO_ARCH_NRF52 -DBLINKY_PLATFORM_NRF52840 \
  -I blinky-simulator/include -I blinky-things tests/timing/timing_test.cpp \
  blinky-things/types/PixelMatrix.cpp blinky-things/types/BlinkyAssert.cpp \
  blinky-things/physics/PhysicsContext.cpp blinky-things/particles/Particle.cpp \
  blinky-things/math/SimplexNoise.cpp blinky-things/generators/Fire.cpp \
  blinky-things/generators/Water.cpp blinky-things/generators/PlasmaGlobe.cpp \
  blinky-things/generators/Audio.cpp -o build/timing_test
build/timing_test
```

## 📊 Continuous Integration

Besides the host tests above, `pr-validation.yml` runs:
- **Compile Firmware** - `scripts/build.sh` with the pinned toolchain, as releases do
- **Static Analysis (cppcheck)**, **Static Init Safety Check** (`scripts/check_static_init.py`), **Syntax Validation** (`tools/validate_syntax.py`), **Code Quality**, **Documentation Check**
- **Onset Model Input Check** (`tools/check_model_input.py`) - the embedded model fits the firmware's limits
- **Blinky Server Tests** and **Blinky Console Tests**

Which of these block a merge is listed in [.github/BRANCH_PROTECTION.md](../.github/BRANCH_PROTECTION.md).

### Local Pre-commit Testing
Before pushing:
```bash
# Compile check (pinned toolchain; run scripts/setup_toolchain.sh once first)
./scripts/build.sh --no-bump

# Plus the host test(s) for the code you touched (see the table above)
```

## ⚠️ Legacy On-Device Suite (do not use)

These files predate the runtime device config and the current flashing rules:

- `test_runner.ino`, `BlinkyTest.h/.cpp`, `unit/`, `integration/` - an on-device test sketch
- `validate_configs.py` - checks the old compiled-in `blinky-things/devices/*.h` configs. Devices now use the JSON configs in `devices/registry/`.

To test on hardware, flash a test device (`devices/test_devices.json`) with `python tools/deploy_local.py --port <port> --device-id <id> --dev` and use the on-device tests in [TESTING.md](../TESTING.md).

## 📝 Writing New Tests

- Prefer a host test that links the real firmware sources (see `timing/` and `tracker/` for the pattern) over an on-device test.
- Add it to `pr-validation.yml` so it runs on every PR. Build with `-fsanitize=undefined -fno-sanitize-recover=all` where you can.
- Put the build command in the test file's header comment or a README next to it.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for detailed contribution guidelines.
