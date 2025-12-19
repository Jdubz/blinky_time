# 🔥 Blinky Time - LED Fire Effect Controller

[![License: CC BY-SA 4.0](https://img.shields.io/badge/License-CC%20BY--SA%204.0-lightgrey.svg)](http://creativecommons.org/licenses/by-sa/4.0/)
[![Arduino](https://img.shields.io/badge/Arduino-Compatible-blue.svg)](https://www.arduino.cc/)
[![Platform](https://img.shields.io/badge/Platform-nRF52840-green.svg)](https://www.nordicsemi.com/Products/nRF52840)

A sophisticated LED fire effect controller for wearable art installations, designed for multiple hardware configurations including hats, tubes, and matrix displays.

## ✨ Features

- **Realistic Fire Simulation** - Advanced heat propagation algorithms create lifelike flame effects
- **Multiple Device Support** - Hat installations, tube lights, and bucket totems
- **Audio Reactive** - Microphone integration for sound-responsive effects with adaptive gain control
- **Unified Architecture** - Single generator supporting matrix, linear, and custom LED layouts
- **Battery Management** - Smart charging detection and low-battery warnings
- **IMU Integration** - Motion-responsive effects using built-in accelerometer
- **Serial Console** - Real-time debugging and effect parameter tuning
- **Zigzag Matrix Support** - Optimized for complex LED wiring patterns
- **Production Ready** - Fully tested and documented with comprehensive build system

## 🛠 Hardware Compatibility

| Device Type | LEDs | Matrix Size | Microcontroller | Status |
|-------------|------|-------------|-----------------|---------|
| **Hat Installation** | 89 | String | nRF52840 XIAO Sense | ✅ Tested |
| **Tube Light** | 60 | 4x15 Zigzag | nRF52840 XIAO Sense | ✅ Tested |
| **Bucket Totem** | 128 | 16x8 Matrix | nRF52840 XIAO Sense | ✅ Tested |

## 🚀 Quick Start

### Prerequisites
- **Arduino IDE** 2.0+ or **arduino-cli**
- **Seeeduino mbed nRF52 Board Package** 2.7.2+ (with platform patch - see [Platform Fix](docs/PLATFORM_FIX.md))
- **Adafruit NeoPixel Library** 1.15.0+

> ⚠️ **Important**: The Seeeduino mbed platform requires a [patch](docs/PLATFORM_FIX.md) to enable audio-reactive features. See the [Build Guide](docs/BUILD_GUIDE.md) for details.

### Installation

1. **Clone the repository**
   ```bash
   git clone https://github.com/Jdubz/blinky_time.git
   cd blinky_time
   ```

2. **Configure your device**
   Edit `blinky-things/blinky-things.ino` and set your device type:
   ```cpp
   #define DEVICE_TYPE 1  // 1=Hat, 2=Tube Light, 3=Bucket Totem
   ```

3. **Upload to your board**
   - Open `blinky-things/blinky-things.ino` in Arduino IDE
   - Select **Tools → Board → Seeed XIAO nRF52840 Sense**
   - Select your COM port
   - Click **Upload**

## 📁 Project Structure

```
blinky_time/
├── blinky-things/           # Main Arduino sketch
│   ├── blinky-things.ino   # Main sketch file
│   ├── BlinkyArchitecture.h # Single include for all components
│   ├── generators/         # Pattern generators (fire, etc.)
│   ├── effects/            # Visual effects (hue rotation, etc.)
│   ├── renderers/          # Hardware output mapping
│   ├── tests/              # Component test coordination
│   ├── devices/            # Device-specific configurations
│   ├── AdaptiveMic.cpp/.h  # Audio processing
│   ├── FireEffect.cpp/.h   # Legacy fire simulation
│   ├── BatteryMonitor.cpp/.h # Power management
│   └── SerialConsole.cpp/.h # Debug interface
├── blinky-console/          # Web-based control interface (React PWA)
│   ├── src/                # React components and hooks
│   ├── firebase.json       # Firebase hosting configuration
│   └── package.json        # Node.js dependencies
├── tests/                  # Project-wide test suite
│   ├── BlinkyTest.h        # Custom test framework
│   ├── test_runner.ino     # Hardware test runner
│   ├── run_tests.py        # Automated test script
│   └── unit/integration/   # Test categories
├── docs/                   # 📚 Comprehensive documentation
├── examples/               # Example configurations
├── .github/workflows/      # CI/CD pipelines for validation and deployment
├── LICENSE                 # Creative Commons BY-SA 4.0
└── README.md              # This file
```

## 📚 Documentation

Comprehensive documentation is available in the [`docs/`](docs/) folder:

**Quick Links:**
- [📖 **Documentation Index**](docs/README.md) - Complete documentation overview
- [🔧 **Hardware Guide**](docs/HARDWARE.md) - Supported devices and wiring
- [🏗️ **Build Guide**](docs/BUILD_GUIDE.md) - Step-by-step setup instructions (includes platform patch)
- [🛠️ **Platform Fix**](docs/PLATFORM_FIX.md) - Required patch for audio-reactive features
- [🏛️ **Architecture Guide**](docs/GENERATOR_EFFECT_ARCHITECTURE.md) - Modern code architecture
- [🔥 **Fire Settings**](docs/OPTIMAL_FIRE_SETTINGS.md) - Optimal configuration parameters
- [🧪 **Testing Guide**](docs/TESTING_SUMMARY.md) - Test framework and procedures
- [✅ **Audit Summary**](AUDIT_SUMMARY.md) - Recent code audit and platform bug fix

## 🎛 Configuration

### Device Selection
Choose your hardware configuration in `blinky-things.ino`:

```cpp
// Device Type Selection
#define DEVICE_TYPE 1  // Change this value:
// 1 = Hat (89 LEDs, STRING_FIRE mode)
// 2 = Tube Light (4x15 matrix, MATRIX_FIRE mode)  
// 3 = Bucket Totem (16x8 matrix, MATRIX_FIRE mode)
```

### Hardware-Specific Settings
Each device type has its own configuration file in `devices/`:
- `HatConfig.h` - Hat installation settings
- `TubeLightConfig.h` - Tube light parameters  
- `BucketTotemConfig.h` - Bucket totem configuration

## 🔥 Fire Effect Parameters

Fine-tune your fire effect in the device config files:

```cpp
.fireDefaults = {
  .baseCooling = 40,        // How fast fire cools down
  .sparkHeatMin = 50,       // Minimum spark intensity
  .sparkHeatMax = 200,      // Maximum spark intensity  
  .sparkChance = 0.200f,    // Probability of new sparks
  .audioSparkBoost = 0.300f, // Audio responsiveness
  // ... more parameters
}
```

## 🎵 Audio Features

- **Adaptive Microphone** - Automatic gain control and noise filtering
- **Beat Detection** - Responds to music transients and beats
- **Configurable Sensitivity** - Adjustable via serial console
- **Real-time Visualization** - Live audio levels in debug output
- **Low Latency** - ~16ms from sound to LED response

> 📝 **Note**: Audio features require the [platform patch](docs/PLATFORM_FIX.md) to resolve include guard issues in Seeeduino mbed platform.

## 🔋 Power Management

- **Battery Monitoring** - Real-time voltage and charge level tracking
- **Charging Detection** - Automatic display mode switching when plugged in
- **Low Battery Warnings** - Configurable voltage thresholds
- **Power Optimization** - Efficient LED driving and CPU usage

## 🛠 Development

### Serial Console Commands
Connect via serial monitor (115200 baud) for real-time control:

```
help        - Show available commands
status      - Display system information  
brightness  - Adjust LED brightness
fire        - Toggle fire effect on/off
audio       - Audio sensitivity settings
battery     - Battery status and settings
```

### Adding New Device Types
1. Create new config file in `devices/`
2. Add device type constant in `blinky-things.ino`
3. Implement device-specific LED mapping if needed
4. Test and validate fire effect parameters

## 🤝 Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

For additional development resources, check the [comprehensive documentation](docs/).

### Development Setup

**Arduino Firmware:**
1. Fork the repository
2. Work on `staging` branch or create feature branches from `staging`
3. Apply the [platform patch](docs/PLATFORM_FIX.md) for audio-reactive features
4. Test your changes on hardware
5. Submit a pull request to `staging` with detailed description
6. Production releases are promoted from `staging` to `master`

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
- **Discussions**: [GitHub Discussions](https://github.com/Jdubz/blinky_time/discussions)
- **Wiki**: [Project Wiki](https://github.com/Jdubz/blinky_time/wiki)

---

**Made with 🔥 for the maker community**
