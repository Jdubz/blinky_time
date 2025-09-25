# Blinky Things - Arduino CLI Build Guide

## Hardware Target
**Board**: Seeed XIAO nRF52840 Sense
- **MCU**: nRF52840 ARM Cortex-M4 @ 64MHz  
- **Flash**: 1MB
- **RAM**: 256KB
- **Features**: Built-in IMU (LSM6DS3), PDM microphone, battery charging

## Build Environment Setup

### Prerequisites
- Arduino CLI (tested with version 1.3.1)
- Windows PowerShell (or equivalent terminal)

### 1. Install Arduino CLI
Download from: https://arduino.cc/pro/cli  
Or use the version at: `D:\utilities\arduino-cli.exe`

### 2. Initialize Configuration
```powershell
arduino-cli config init
```

### 3. Add Seeed Board Manager URL
```powershell
arduino-cli config add board_manager.additional_urls https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
```

### 4. Update Board Index
```powershell
arduino-cli core update-index
```

### 5. Install Seeed nRF52 Core
```powershell
arduino-cli core install Seeeduino:nrf52
```

**Important**: Use the **non-mbed** core (`Seeeduino:nrf52`) not the mbed version (`Seeeduino:mbed`). The mbed core has header conflicts with Adafruit NeoPixel library.

### 6. Install Required Libraries
```powershell
# Core LED control library
arduino-cli lib install "Adafruit NeoPixel"

# IMU sensor library  
arduino-cli lib install "Seeed Arduino LSM6DS3"
```

## Device Configuration Selection

The project supports three device types via compile-time configuration:

### Device Type 1: Hat (Default)
- **LEDs**: 89 LEDs in linear string arrangement
- **Fire Mode**: STRING_FIRE (sideways heat dissipation)
- **Use Case**: Wearable hat display

### Device Type 2: Tube Light  
- **LEDs**: 4x15 LED matrix (60 total)
- **Fire Mode**: MATRIX_FIRE (upward flame propagation)
- **Orientation**: VERTICAL
- **Use Case**: Cylindrical tube lighting

### Device Type 3: Bucket Totem
- **LEDs**: 16x8 LED matrix (128 total) 
- **Fire Mode**: MATRIX_FIRE (upward flame propagation)
- **Orientation**: HORIZONTAL
- **Use Case**: Desktop fire simulation totem

## Build Commands

### Quick Build (Default = Hat)
```powershell
arduino-cli compile --fqbn "Seeeduino:nrf52:xiaonRF52840Sense" "d:\blinky_time\blinky-things"
```

### Build for Specific Device Types
```powershell
# Hat Configuration (default)
arduino-cli compile --fqbn "Seeeduino:nrf52:xiaonRF52840Sense" "d:\blinky_time\blinky-things"

# Tube Light Configuration  
arduino-cli compile --fqbn "Seeeduino:nrf52:xiaonRF52840Sense" --build-property "compiler.cpp.extra_flags=-DDEVICE_TYPE=2" "d:\blinky_time\blinky-things"

# Bucket Totem Configuration
arduino-cli compile --fqbn "Seeeduino:nrf52:xiaonRF52840Sense" --build-property "compiler.cpp.extra_flags=-DDEVICE_TYPE=3" "d:\blinky_time\blinky-things"
```

### Build with Upload
```powershell
arduino-cli compile --upload --port COM3 --fqbn "Seeeduino:nrf52:xiaonRF52840Sense" "d:\blinky_time\blinky-things"
```
*(Replace COM3 with your actual port)*

## Build Output (Successful)
```
Sketch uses 105848 bytes (13%) of program storage space. Maximum is 811008 bytes.
Global variables use 11808 bytes (4%) of dynamic memory, leaving 225760 bytes for local variables. Maximum is 237568 bytes.
```

**Memory Usage**: Very efficient - only 13% flash and 4% RAM used, leaving plenty of headroom.

## Libraries Used
| Library | Version | Purpose |
|---------|---------|---------|
| Adafruit NeoPixel | 1.15.1 | LED strip control |
| Seeed Arduino LSM6DS3 | 2.0.5 | IMU sensor interface |
| PDM (built-in) | 1.0 | Microphone audio input |
| Wire (built-in) | 1.0 | I2C communication |
| SPI (built-in) | 1.0 | SPI communication |
| Adafruit TinyUSB | 3.4.5 | USB communication |

## Hardware Connections

### XIAO nRF52840 Sense Pin Mapping
| Function | Pin | Notes |
|----------|-----|-------|
| LED Data | D0 (Hat) / D10 (Tube/Bucket) | NeoPixel data line |
| Battery Voltage | A0 (Pin 31) | Internal voltage divider |
| Battery Enable | Pin 14 | Battery measurement control |
| Charge Status | Pin 17 | Charging indicator |
| Charge Rate | Pin 22 | Fast/slow charge control |
| IMU (LSM6DS3) | I2C | Built-in sensor |
| Microphone | PDM | Built-in MEMS mic |

## Configuration Files Structure
```
configs/
├── DeviceConfig.h      # Base configuration structure
├── HatConfig.h         # Hat device (89 LEDs, STRING_FIRE)
├── TubeLightConfig.h   # Tube light (4x15 matrix, MATRIX_FIRE)
└── BucketTotemConfig.h # Bucket totem (16x8 matrix, MATRIX_FIRE)
```

## Fire Effect Modes

### STRING_FIRE Mode
- **Used by**: Hat configuration
- **Behavior**: Heat dissipates sideways along the LED string
- **Ideal for**: Linear LED arrangements, wearable devices
- **Heat Combination**: Maximum value (prevents harsh bright spots)

### MATRIX_FIRE Mode  
- **Used by**: Tube Light and Bucket Totem
- **Behavior**: Heat propagates upward like real fire
- **Ideal for**: 2D LED matrices, realistic fire simulation
- **Heat Combination**: Additive (allows flame intensity buildup)

## Troubleshooting

### Common Issues

#### 1. Header Conflict Errors
**Problem**: `redefinition of 'struct _PinDescription'`  
**Solution**: Ensure you're using `Seeeduino:nrf52` core, NOT `Seeeduino:mbed`

#### 2. Pin Not Declared Errors  
**Problem**: `P0_14 was not declared in this scope`  
**Solution**: Pin definitions are auto-detected based on core. Ensure BatteryMonitor.h has correct conditional compilation.

#### 3. Library Not Found
**Problem**: `fatal error: Adafruit_NeoPixel.h: No such file or directory`
**Solution**: Install required libraries using `arduino-cli lib install`

#### 4. Upload Issues
**Problem**: Can't find board or upload fails
**Solution**: 
- Check USB cable and port
- Put board in bootloader mode (double-tap reset)
- Verify correct FQBN: `Seeeduino:nrf52:xiaonRF52840Sense`

### Verification Steps
1. **Compilation**: Should complete without errors showing ~13% flash usage
2. **Upload**: Should upload successfully and start LED test sequence  
3. **Serial Output**: Connect at 115200 baud to see startup messages
4. **LED Test**: First 3 LEDs should show Red, Green, Blue for 3 seconds
5. **Audio Response**: LEDs should react to audio input after startup

## Development Workflow

### Typical Development Cycle
1. **Edit Code**: Modify source files as needed
2. **Select Device**: Set `DEVICE_TYPE` in main .ino or use build flag
3. **Compile**: `arduino-cli compile --fqbn "Seeeduino:nrf52:xiaonRF52840Sense"`  
4. **Upload**: `arduino-cli upload --port COMx --fqbn "Seeeduino:nrf52:xiaonRF52840Sense"`
5. **Test**: Verify functionality via serial monitor and LED behavior

### Code Changes for Different Devices
- **No code changes needed** - just change `DEVICE_TYPE` define or build flag
- All device-specific parameters are in config files
- Same codebase supports all three hardware configurations

## Performance Characteristics
- **Startup Time**: ~3 seconds (includes IMU calibration and LED test)
- **Frame Rate**: ~60 FPS fire animation
- **Memory**: 13% flash, 4% RAM (very efficient)
- **Audio Latency**: ~16ms from sound to LED response
- **Battery Life**: Varies by LED count and brightness

---

*Build guide updated: September 23, 2025*  
*Tested with Arduino CLI 1.3.1 and Seeeduino:nrf52 core 1.1.10*