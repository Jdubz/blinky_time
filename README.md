# 🔥 Blinky Time - LED Fire Effect Controller

[![License: CC BY-SA 4.0](https://img.shields.io/badge/License-CC%20BY--SA%204.0-lightgrey.svg)](http://creativecommons.org/licenses/by-sa/4.0/)
[![Arduino](https://img.shields.io/badge/Arduino-Compatible-blue.svg)](https://www.arduino.cc/)
[![Platform](https://img.shields.io/badge/Platform-nRF52840-green.svg)](https://www.nordicsemi.com/Products/nRF52840)

A sophisticated LED fire effect controller for wearable art installations, designed for multiple hardware configurations including hats, tubes, and matrix displays.

## ✨ Features

- **Realistic Fire Simulation** - Advanced heat propagation algorithms create lifelike flame effects
- **Multiple Device Support** - Hat installations, tube lights, and bucket totems
- **Audio Reactive** - Microphone integration for sound-responsive effects  
- **Battery Management** - Smart charging detection and low-battery warnings
- **IMU Integration** - Motion-responsive effects using built-in accelerometer
- **Serial Console** - Real-time debugging and effect parameter tuning
- **Zigzag Matrix Support** - Optimized for complex LED wiring patterns

## 🛠 Hardware Compatibility

| Device Type | LEDs | Matrix Size | Microcontroller | Status |
|-------------|------|-------------|-----------------|---------|
| **Hat Installation** | 89 | String | nRF52840 XIAO Sense | ✅ Tested |
| **Tube Light** | 60 | 4x15 Zigzag | nRF52840 XIAO Sense | ✅ Tested |
| **Bucket Totem** | 128 | 16x8 Matrix | nRF52840 XIAO Sense | ✅ Tested |

## 🚀 Quick Start

### Prerequisites
- **Arduino IDE** 2.0+ or **arduino-cli**
- **Seeeduino nRF52 Board Package** installed
- **Adafruit NeoPixel Library** 1.15.0+

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
│   ├── configs/            # Device-specific configurations
│   ├── AdaptiveMic.cpp/.h  # Audio processing
│   ├── FireEffect.cpp/.h   # Fire simulation engine
│   ├── BatteryMonitor.cpp/.h # Power management
│   └── SerialConsole.cpp/.h # Debug interface
├── tests/                  # Comprehensive test suite
│   ├── BlinkyTest.h        # Custom test framework
│   ├── test_runner.ino     # Hardware test runner
│   ├── run_tests.py        # Automated test script
│   └── unit/integration/   # Test categories
├── docs/                   # Documentation
├── examples/               # Example configurations
├── scratch/                # Experimental code (git-ignored)
├── .github/workflows/      # CI/CD automation
├── LICENSE                 # Creative Commons BY-SA 4.0
└── README.md              # This file
```

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
Each device type has its own configuration file in `configs/`:
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
1. Create new config file in `configs/`
2. Add device type constant in `blinky-things.ino`
3. Implement device-specific LED mapping if needed
4. Test and validate fire effect parameters

## 🤝 Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

### Development Setup
1. Fork the repository
2. Create a feature branch
3. Test your changes on hardware
4. Submit a pull request with detailed description

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
