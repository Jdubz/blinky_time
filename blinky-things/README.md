# Blinky Things - Folder Structure

This document describes the folder structure of the firmware sketch (XIAO nRF52840 Sense). For the Generator → Effect → Renderer design itself, see [docs/GENERATOR_EFFECT_ARCHITECTURE.md](../docs/GENERATOR_EFFECT_ARCHITECTURE.md); for audio, [docs/AUDIO_ARCHITECTURE.md](../docs/AUDIO_ARCHITECTURE.md).

Build it with `./scripts/build.sh` from the repo root (pinned toolchain; see [CLAUDE.md](../CLAUDE.md)). Never flash it with `arduino-cli upload` or the Arduino IDE.

## 📁 Directory Structure

```
blinky-things/
├── blinky-things.ino              # Main sketch: setup(), loop(), globals
├── BlinkyArchitecture.h           # Single include for all headers
├── BlinkyImplementations.h        # #includes every .cpp in a subfolder (see below)
├── BUILD_NUMBER                   # Build number; build.sh writes types/Version.h from it
├── Nrf52PdmMic.cpp                # nRF52840 PDM mic driver
├── OtaMonocypher*.c               # Vendored Monocypher (Ed25519) units for signed OTA
├── audio/                         # AudioTracker (tempo, PLP phase, onsets),
│                                  #   SharedSpectralAnalysis, FrameOnsetNN + model data
├── comms/                         # BLE: NUS serial, fleet scanner, phone record stream
│                                  #   (Esp32*/Wifi* files are unused ESP32-S3 leftovers)
├── config/                        # ConfigStorage (flash), SettingsRegistry, DeviceConfigLoader
├── devices/                       # DeviceConfig struct + TestChipConfig (fallback for
│                                  #   unconfigured chips); other *Config.h are legacy
├── effects/                       # HueRotationEffect, NoOpEffect
│   └── tests/                     # On-device effect tests (ENABLE_TESTING only)
├── generators/                    # Fire, Water, PlasmaGlobe ("lightning"), Audio
├── hal/                           # Platform detection, SafeBootWatchdog,
│   │                              #   RebootFrequencyCounter, heap lock
│   ├── hardware/                  # LED strip drivers (NeoPixel, nRF52 PWM), mic
│   ├── interfaces/                # ILedStrip, IPdmMic, IGpio, IAdc, ISystemTime
│   └── mock/                      # Mocks for host builds
├── inputs/                        # AdaptiveMic, SerialConsole, BatteryMonitor, IMUHelper
├── math/                          # SimplexNoise
├── ota/                           # Signed OTA: OtaManifest, OtaKeys, QspiOtaStaging
├── particles/                     # Particle, ParticlePool, ParticleGenerator base
├── physics/                       # Spawn regions, boundaries, forces, backgrounds
├── render/                        # RenderPipeline, EffectRenderer, LEDMapper
│   └── tests/                     # On-device renderer tests (ENABLE_TESTING only)
├── tests/                         # GeneratorTestRunner, safety/static-init checks
└── types/                         # PixelMatrix, BlinkyAssert, FrameClock, Version.h (generated)
```

Device layouts are not compiled in. Each device gets its config as JSON (from `devices/registry/` at the repo root) through the `device upload` command, stores it in flash, and loads it at boot with `config/DeviceConfigLoader`.

## 🎯 Design Principles

### 1. **Clear Component Separation**
- **`generators/`** - Pattern creation (fire, water, plasma, audio)
- **`effects/`** - Pattern modification (hue rotation)
- **`render/`** - Mapping the pattern to the physical LED layout and output

### 2. **Shared Particle Physics**
- Fire and Water are particle generators (`particles/ParticleGenerator.h`); PlasmaGlobe and Audio derive from `Generator` directly
- `physics/PhysicsContext` picks spawn regions, boundaries and forces for the device layout (matrix or linear)

### 3. **Arduino IDE Compatibility**
- Arduino doesn't compile `.cpp` files in subfolders. **Every new `.cpp` in a subfolder must be added to `BlinkyImplementations.h`**, or it won't be built.
- `BlinkyArchitecture.h` provides the headers for the main sketch

### 4. **Host Builds**
- The same sources compile on a PC for the simulator (`blinky-simulator/`) and the host tests (`tests/timing`, `tests/tracker`, `tests/parity`), using the Arduino stubs in `blinky-simulator/include/`. Keep hardware access behind `hal/` so this keeps working.

## 🔧 Usage Patterns

### Main Sketch
```cpp
#include "BlinkyArchitecture.h"
#include "BlinkyImplementations.h"
```
`blinky-things.ino` builds a `RenderPipeline` from the stored device config. The pipeline owns the generators and effects; the serial command `gen <name>` switches the active generator.

### For Adding New Generators
1. Add `generators/MyGenerator.h/.cpp` (derive from `Generator`, or `ParticleGenerator` for particle effects)
2. Add a value to `GeneratorType` in `generators/Generator.h`
3. Wire it into `render/RenderPipeline`
4. Add the `.cpp` to `BlinkyImplementations.h`
5. Add it to the simulator and the timing test build lists (`blinky-simulator/CMakeLists.txt`, `build.sh`, and the `timing` job in `.github/workflows/pr-validation.yml`)

### For Adding New Effects
1. Add `effects/MyEffect.h/.cpp` (derive from `Effect`)
2. Wire it into `render/RenderPipeline`
3. Add the `.cpp` to `BlinkyImplementations.h`

## 🧪 Testing Architecture

- **Host tests** (run in CI): see [tests/README.md](../tests/README.md). These are the tests that matter for new code.
- **On-device tests** (`tests/`, `effects/tests/`, `render/tests/`): compiled only when `ENABLE_TESTING` is defined, which no current build does.
