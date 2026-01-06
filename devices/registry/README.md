# Device Registry

This directory contains JSON configuration files for different Blinky hardware devices. Each JSON file defines a complete device configuration that can be uploaded to the firmware via serial or web console.

## Available Devices

| Device ID | Description | LEDs | Layout | File |
|-----------|-------------|------|--------|------|
| `hat_v1` | Festival Hat v1 | 89 (string) | LINEAR | `hat_v1.json` |
| `tube_v2` | Tube Light v2 | 60 (4x15 matrix) | MATRIX | `tube_v2.json` |
| `bucket_v3` | Bucket Totem v3 | 128 (16x8 matrix) | MATRIX | `bucket_v3.json` |

## JSON Schema

All device configuration files must follow this schema:

```json
{
  // Device identification
  "deviceId": "string",           // Unique ID (alphanumeric + underscore)
  "deviceName": "string",         // Human-readable name (max 31 chars)

  // LED Matrix Configuration
  "ledWidth": number,             // Matrix width (or total count for linear)
  "ledHeight": number,            // Matrix height (1 for linear)
  "ledPin": number,               // GPIO pin number for LED data (0-48)
  "brightness": number,           // Default brightness (0-255)
  "ledType": number,              // NeoPixel type constant (12390 = NEO_GRB + NEO_KHZ800)
  "orientation": number,          // 0=HORIZONTAL, 1=VERTICAL
  "layoutType": number,           // 0=MATRIX, 1=LINEAR, 2=RANDOM

  // Battery/Charging Configuration
  "fastChargeEnabled": boolean,   // Enable fast charge mode
  "lowBatteryThreshold": number,  // Low battery voltage (typically 3.5V)
  "criticalBatteryThreshold": number, // Critical voltage (typically 3.3V)
  "minVoltage": number,           // Minimum voltage (typically 3.0V)
  "maxVoltage": number,           // Maximum voltage (typically 4.2V for LiPo)

  // IMU Configuration (motion sensor)
  "upVectorX": number,            // Up vector X component (0.0-1.0)
  "upVectorY": number,            // Up vector Y component (0.0-1.0)
  "upVectorZ": number,            // Up vector Z component (0.0-1.0)
  "rotationDegrees": number,      // Rotation angle in degrees
  "invertZ": boolean,             // Invert Z axis
  "swapXY": boolean,              // Swap X and Y axes
  "invertX": boolean,             // Invert X axis
  "invertY": boolean,             // Invert Y axis

  // Serial Communication
  "baudRate": number,             // Baud rate (typically 115200)
  "initTimeoutMs": number,        // Serial init timeout in milliseconds

  // Microphone Configuration
  "sampleRate": number,           // Sample rate in Hz (8000, 16000, 32000, etc.)
  "bufferSize": number,           // Buffer size (typically 32)

  // Fire Effect Defaults (legacy - may be deprecated)
  "baseCooling": number,          // Base cooling rate (0-255)
  "sparkHeatMin": number,         // Minimum spark heat (0-255)
  "sparkHeatMax": number,         // Maximum spark heat (0-255)
  "sparkChance": number,          // Spark spawn chance (0.0-1.0)
  "audioSparkBoost": number,      // Audio reactivity multiplier (0.0-2.0)
  "coolingAudioBias": number,     // Cooling adjustment with audio (-128 to 127)
  "bottomRowsForSparks": number   // Number of bottom rows for spark spawning
}
```

## Field Descriptions

### LED Type Constants
- `12390` = `NEO_GRB + NEO_KHZ800` (standard WS2812B for nRF52840)
  - NEO_GRB = color order (green-red-blue)
  - KHZ800 = 800 kHz signal speed

### Orientation Values
- `0` = HORIZONTAL (fire rises upward in normal orientation)
- `1` = VERTICAL (fire rises sideways - for tube lights)

### Layout Type Values
- `0` = MATRIX_LAYOUT (2D grid, heat propagates upward)
- `1` = LINEAR_LAYOUT (single string, heat propagates sideways)
- `2` = RANDOM_LAYOUT (scattered LEDs, omnidirectional heat)

### Battery Voltage Guidelines (LiPo)
- **Max Voltage**: 4.2V (fully charged)
- **Low Warning**: 3.5V (~10% remaining)
- **Critical**: 3.3V (~0% remaining - shutdown soon)
- **Min Voltage**: 3.0V (over-discharge protection)

## Uploading a Device Config

### Via Serial Console
```
device upload {"deviceId":"hat_v1","deviceName":"Festival Hat v1","ledWidth":89,"ledHeight":1,...}
```

Or paste the entire JSON:
```
device upload <paste entire JSON here>
```

Then reboot the device to apply the configuration.

### Via Web Console
1. Open the web console at http://localhost:3000 (or device IP)
2. Navigate to Settings → Device Configuration
3. Select a device from the dropdown OR paste custom JSON
4. Click "Upload Config"
5. Reboot the device

## Creating a Custom Device Config

1. Copy an existing JSON file (e.g., `hat_v1.json`)
2. Rename it with a unique device ID (e.g., `custom_hat_v2.json`)
3. Modify the values:
   - Update `deviceId` and `deviceName`
   - Set `ledWidth` and `ledHeight` to match your LED count
   - Set `ledPin` to your data pin number
   - Adjust `brightness` for your preference
   - Configure battery thresholds if needed
4. Upload via serial or web console

## Validation

The firmware validates all uploaded configurations:
- LED count must be 1-500
- LED pin must be 0-48 (nRF52840 GPIO range)
- Voltage range must be 2.5V - 5.0V
- Sample rate must be standard (8000, 16000, 32000, 44100, 48000 Hz)
- Baud rate must be standard (9600, 19200, 38400, 57600, 115200, 230400)

Invalid configurations will be rejected with an error message.

## Troubleshooting

**Device won't boot after config upload:**
- Check serial console for validation errors
- Ensure LED count matches physical hardware
- Verify LED pin is correct
- Try factory reset and re-upload

**LEDs show wrong colors:**
- Check `ledType` value (may need NEO_RGB instead of NEO_GRB)
- Verify wiring matches configuration

**Safe mode after reboot:**
- Config may have failed validation
- Check serial console for error messages
- Upload a known-good config (e.g., `hat_v1.json`)

## Notes

- The firmware stores ONE device config in flash (~160 bytes)
- Config persists across reboots and power cycles
- Uploading a new config overwrites the previous one
- First boot with no config enters safe mode (LED output disabled)
- All devices share the same universal firmware binary

---

**Last Updated**: January 2026
**Firmware Version**: v28+ (runtime device configuration)
