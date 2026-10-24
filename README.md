# 🔥 Blinky Time - LED Fire Effect Controller

[![License: CC BY-SA 4.0](https://img.shields.io/badge/License-CC%20BY--SA%204.0-lightgrey.svg)](http://creativecommons.org/licenses/by-sa/4.0/)
[![Arduino](https://img.shields.io/badge/Arduino-Compatible-blue.svg)](https://www.arduino.cc/)
[![Platform](https://img.shields.io/badge/Platform-nRF52840-green.svg)](https://www.nordicsemi.com/Products/nRF52840)

A sophisticated LED fire effect controller for wearable art installations, designed for multiple hardware configurations including hats, tubes, and matrix displays.

## ✨ Features

### Visual Effects
- **🔥 Realistic Fire Simulation** - Particle-based fire with audio-reactive sparks
- **💧 Water Effects** - Particle-based falling drops
- **⚡ Plasma Globe** - Electric tendrils (the "lightning" generator)
- **🎚 Audio Visualizer** - Direct view of the audio signals
- **🌈 Post-Processing** - Hue rotation and effect chaining
- **Multiple Device Support** - Hats, tube lights, bucket totems, cart strings and matrix displays (see [Hardware](docs/HARDWARE.md))
- **Unified Architecture** - Generator→Effect→Renderer pipeline supporting matrix, linear, and custom layouts

### Audio Analysis
- **🥁 Onset Detection** - Fires on kicks and snares (on or off the beat), not hi-hats. Devices use spectral flux; a Conv1D onset network (`FrameOnsetNN`) is built in but off until a model beats flux (see `audio/AudioTracker.h`)
- **🎵 AudioTracker** - Multi-source ACF tempo estimation + PLP (epoch-fold) phase and pattern tracking
- **🎤 Adaptive Microphone** - Fixed hardware gain with window/range normalization (AGC removed v72)
- **50+ Tunable Parameters** - Comprehensive audio parameter system

### System Features
- **🌐 Web App Control** - [Blinky Console](blinky-console/) React PWA for real-time settings and visualization
- **📡 Bluetooth + Fleet Server** - BLE serial (NUS) and fleet broadcasts; [blinky-server](blinky-server/) manages the deployed fleet
- **⬆️ Firmware Updates** - Signed stable releases, installed from the browser at [blinky-time.joshwentworth.com/#firmware](https://blinky-time.joshwentworth.com/#firmware) (USB) or by `scripts/deploy.sh` for the fleet
- **🔋 Battery Management** - Charging detection and low-battery warnings
- **🧪 Testing Infrastructure** - Parameter tuning CLI, MCP server for AI integration, ground truth validation
- **🛡️ Multi-Layer Safety** - Compile-time checks, runtime validation, flash protection, upload enforcement
- **⚙️ Configuration Storage** - Flash persistence with validation (`SETTINGS_VERSION` in `config/ConfigStorage.h`)

## 🛠 Hardware Compatibility

All devices run on the **Seeed XIAO nRF52840 Sense**, with one firmware binary. Each device gets its layout from a JSON config at runtime; the configs live in [`devices/registry/`](devices/registry/). Examples:

| Device Type | LEDs | Layout |
|-------------|------|--------|
| **Hat** (`hat_v1`) | 89 | String |
| **Tube Light** (`tube_v2`) | 60 | 4x15 zigzag matrix |
| **Bucket Totem** (`bucket_v3`) | 128 | 16x8 matrix |

See [docs/HARDWARE.md](docs/HARDWARE.md) for the full device list. ESP32-S3 support was cut in March 2026; the next generation of fixtures is planned on the XIAO ESP32-S3 Sense ([plan](docs/NEXTGEN_ESP32S3_PLAN.md)).

## 🚀 Quick Start

### Update a device you own (no build needed)
Open [blinky-time.joshwentworth.com/#firmware](https://blinky-time.joshwentworth.com/#firmware) in Chrome or Edge and connect the device over USB. The page installs the latest **stable** release through the device's signed update path. See [docs/WEB_FLASHER.md](docs/WEB_FLASHER.md).

### Build the firmware

1. **Clone the repository**
   ```bash
   git clone https://github.com/Jdubz/blinky_time.git
   cd blinky_time
   ```

2. **Install the pinned toolchain** (once per machine). This puts the pinned arduino-cli core, libraries, TFLite and core patches into `.arduino/` (gitignored):
   ```bash
   bash scripts/setup_toolchain.sh
   ```

3. **Compile** (always through `build.sh`, which also writes `types/Version.h`):
   ```bash
   ./scripts/build.sh --no-bump    # compile check
   ./scripts/build.sh --dev        # dev build for a test device, version bN-dev-<sha>
   ```

### Flash a device

> ⚠️ **Never use `arduino-cli upload`, the Arduino IDE Upload button, or `adafruit-nrfutil dfu serial` on the nRF52840.** They can brick the device. See [CLAUDE.md](CLAUDE.md) and [docs/SAFETY.md](docs/SAFETY.md).

- **Fleet devices:** `./scripts/deploy.sh --devices=<id>` (latest stable release, with version check).
- **A device on your own USB:** `python tools/deploy_local.py --port COM4 --device-id <id> --dry-run`, then without `--dry-run`.
- **Dev builds and release candidates** go only to a test device listed in `devices/test_devices.json`.

Releases are cut and promoted as described in [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md).

### Configure the device
The firmware is the same for every device. A new chip boots unconfigured until it receives its device config (layout, LED pin, LED count) from `devices/registry/<id>.json` via the `device upload <json>` serial command. See [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md).

## 📁 Project Structure

```
blinky_time/
├── blinky-things/           # Main Arduino sketch (see blinky-things/README.md)
│   ├── blinky-things.ino   # Main sketch file
│   ├── generators/         # Pattern generators (Fire, Water, PlasmaGlobe, Audio)
│   ├── effects/            # Visual effects (HueRotation, NoOp)
│   ├── render/             # Render pipeline and LED mapping
│   ├── audio/              # AudioTracker, spectral analysis, onset NN
│   ├── inputs/             # AdaptiveMic, SerialConsole, battery, IMU
│   ├── comms/              # BLE serial (NUS), BLE scanner, phone record mode
│   ├── ota/                # Signed OTA (manifest check, QSPI staging)
│   └── config/             # Settings and configuration storage
├── blinky-console/          # Web control interface + firmware updater (React PWA, Firebase)
├── blinky-server/           # Fleet server: serial/BLE connections, flashing, tests
├── blinky-serial-mcp/       # MCP server for AI-assisted testing and tuning
├── blinky-simulator/        # Runs the real generator code on a PC, renders GIFs
├── blinky-test-player/      # Test audio player + validation corpus (music/edm/)
├── ml-training/             # Onset NN training pipeline
├── bootloader/              # Bootloader source (submodule) and staged images
├── devices/                 # Device registry JSON + test device list
├── scripts/                 # build.sh, deploy.sh, setup_toolchain.sh, ...
├── tools/                   # deploy_local.py, release tools, validators
├── tests/                   # Host tests (timing, OTA, tracker, parity); see tests/README.md
├── docs/                   # 📚 Documentation
├── examples/               # Example code snippets
├── .github/workflows/      # CI (PR validation), releases, console deploy
├── LICENSE                 # Creative Commons BY-SA 4.0
└── README.md              # This file
```

## 📚 Documentation

Comprehensive documentation is available in the [`docs/`](docs/) folder:

**Quick Links:**
- [📖 **Documentation Index**](docs/README.md) - Complete documentation overview
- [⚠️ **CLAUDE.md**](CLAUDE.md) - Safety and workflow rules (flashing, releases, bootloader) — read first
- [🔧 **Hardware Guide**](docs/HARDWARE.md) - Supported devices and wiring
- [📦 **Build Guide**](docs/BUILD_GUIDE.md) - Build setup and device configuration
- [🚀 **Release Process**](docs/RELEASE_PROCESS.md) - Stable / candidate / dev channels
- [⬆️ **Web Updater**](docs/WEB_FLASHER.md) - Browser firmware updates
- [🏛️ **Generator Architecture**](docs/GENERATOR_EFFECT_ARCHITECTURE.md) - Generator→Effect→Renderer pattern
- [🎵 **Audio Architecture**](docs/AUDIO_ARCHITECTURE.md) - AudioTracker: ACF tempo, PLP phase, onset detection
- [🧪 **Testing Guide**](TESTING.md) - Where the tests live and how to run them

## 🎛 Configuration

### Device Selection
Device type is not a compile-time setting. Each device stores its config (layout, LED count, LED pin, color order) in flash. To set it, send the JSON from `devices/registry/<id>.json`:

```
device upload <json>   - Store a device config
device show            - Show the current device config
```

On a fleet device, config changes go through `deploy.sh` (see [CLAUDE.md](CLAUDE.md)), not ad-hoc commands.

## 🔥 Fire Effect Parameters

Fire parameters are universal and dimension-normalized — they auto-adapt to any LED layout. Tune via serial console (`set <param> <value>`) or web UI. See `FireParams` in `generators/Fire.h` for the full list.

## 🎵 Audio Features

- **Adaptive Microphone** - Fixed hardware gain with window/range normalization
- **Onset Detection** - Responds to drum hits (kicks and snares), on or off the beat
- **Rhythm Tracking** - Tempo and phase for beat-synced animation
- **Configurable Sensitivity** - Adjustable via web app or serial API
- **Real-time Visualization** - Live audio data via `stream on`

> 📝 **Note**: Audio features use the built-in PDM microphone on the XIAO nRF52840 Sense.

## 🔋 Power Management

- **Battery Monitoring** - Real-time voltage and charge level tracking
- **Charging Detection** - Logs when charging starts and stops (battery-powered devices)
- **Low Battery Warnings** - Serial warning below the device config's threshold
- **Power Optimization** - Efficient LED driving and CPU usage

## 🛠 Development

### Serial Console API
Connect via serial monitor (115200 baud) or use the [Blinky Console](blinky-console/) web app:

```
JSON API (for web app):
  json info           - Device info as JSON
  json settings       - All settings as JSON with metadata
  stream on/off       - Audio data streaming (~20Hz)

Settings:
  set <name> <value>  - Set a parameter value
  get <name>          - Get a parameter value
  show [category]     - Show all settings or by category
  categories          - List all setting categories
  settings            - Show settings with help text

Configuration:
  save                      - Save settings to flash
  load                      - Load settings from flash
  restore_runtime_settings  - Restore default tunables (keeps device identity)
  device show               - Show the device config
```

`wipe_device_identity` (old names `reset` / `factory`) erases the device config as well. On fleet devices it, `device upload` and `reboot` go through `deploy.sh` only.

### Adding New Device Types
1. Add a `devices/registry/<id>.json` config (see `devices/registry/README.md`)
2. Upload it to the device with `device upload <json>`
3. Test and tune generator parameters on the device

## 🤝 Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

For additional development resources, check the [comprehensive documentation](docs/).

### Development Setup

**Arduino Firmware:**
1. Create a feature branch from `master` (see [.github/BRANCHING_STRATEGY.md](.github/BRANCHING_STRATEGY.md))
2. Install the toolchain once: `bash scripts/setup_toolchain.sh`
3. Compile with `./scripts/build.sh`
4. Test on a test device (`devices/test_devices.json`) with `tools/deploy_local.py --dev`
5. Open a pull request to `master`; CI (`.github/workflows/pr-validation.yml`) must pass
6. Releases are built by CI from master and promoted to stable after a bench gate ([docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md))

**Blinky Console (Web Interface):**
```bash
cd blinky-console
npm install          # Installs dependencies and sets up git hooks
npm run dev          # Start development server with hot reload
npm run test         # Run unit tests
npm run lint         # Lint code
npm run build        # Build for production
```

> **Note**: Run `npm install` from within the `blinky-console/` directory to properly set up git hooks (husky) for pre-commit linting and pre-push validation.

## 📜 License

This project is licensed under the **Creative Commons Attribution-ShareAlike 4.0 International License**.

You are free to:
- **Share** — copy and redistribute the material in any medium or format
- **Adapt** — remix, transform, and build upon the material for any purpose, even commercially

Under the following terms:
- **Attribution** — You must give appropriate credit and indicate if changes were made
- **ShareAlike** — If you remix or build upon the material, you must distribute your contributions under the same license

See [LICENSE](LICENSE) for full details.

## 🙏 Credits

- **Hardware Platform**: [Seeed Studio XIAO nRF52840 Sense](https://www.seeedstudio.com/Seeed-XIAO-BLE-Sense-nRF52840-p-5253.html)
- **LED Library**: [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)
- **Fire Algorithm**: Based on classic demo scene fire effects with modern optimizations

## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/Jdubz/blinky_time/issues)

---

**Made with 🔥 for the maker community**
