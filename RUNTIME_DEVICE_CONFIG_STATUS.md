# Runtime Device Configuration - Implementation Status

## Overview
Converting the Blinky firmware from compile-time device selection to runtime configuration loaded from flash memory.

## Completed Work

### 1. ConfigStorage Expansion ✅
- **File**: `blinky-things/config/ConfigStorage.h`
- Added `StoredDeviceConfig` struct (~160 bytes)
  - Device identification (name, ID)
  - Matrix/LED configuration (width, height, pin, type, layout)
  - Charging configuration (voltage thresholds, fast charge)
  - IMU configuration (orientation, rotation, axis inversion)
  - Serial configuration (baud rate, timeout)
  - Microphone configuration (sample rate, buffer size)
  - Fire effect defaults (legacy support)
  - Validity flag and reserved space for future expansion
- Updated `ConfigData` struct to include `StoredDeviceConfig`
- Bumped `CONFIG_VERSION` from 27 to 28
- Increased size limit from 400 to 512 bytes
- Added accessor methods: `getDeviceConfig()`, `setDeviceConfig()`, `isDeviceConfigValid()`

### 2. ConfigStorage Defaults ✅
- **File**: `blinky-things/config/ConfigStorage.cpp`
- Updated `loadDefaults()` to initialize device config as UNCONFIGURED
- Sets `isValid = false` to trigger safe mode on first boot

### 3. Device Config Loader ✅
- **Files**: `blinky-things/config/DeviceConfigLoader.h/cpp`
- Created conversion utilities between `StoredDeviceConfig` (flash) and `DeviceConfig` (runtime)
- **Functions**:
  - `loadFromFlash()` - Load and convert config from flash storage
  - `convertToStored()` - Convert runtime config to flash format
  - `validate()` - Comprehensive validation (LED count, pins, voltages, sample rates)
- Uses static buffers to avoid dynamic allocation
- Detailed validation with logging

### 4. Main Sketch Refactor ✅
- **File**: `blinky-things/blinky-things.ino`
- Removed compile-time `#ifdef DEVICE_TYPE` selection
- Added runtime `DeviceConfig config` and `bool validDeviceConfig` globals
- **Safe Mode Implementation**:
  - Boot sequence:
    1. Initialize serial (115200 baud)
    2. Load device config from flash
    3. If invalid → enter safe mode
    4. If valid → initialize LED system
  - Safe mode features:
    - Audio analysis ENABLED
    - Serial console ENABLED
    - LED output DISABLED
    - Heartbeat blink on built-in LED
    - Clear user instructions for configuration
- **Conditional Initialization**:
  - Audio/Battery: Always initialized (safe mode + normal mode)
  - LEDs/Generators/Pipeline: Only if valid device config
  - Serial console: Always initialized
- **Loop Modifications**:
  - `renderFrame()` only called if `validDeviceConfig == true`
  - Auto-save only runs if generators exist
  - Built-in LED heartbeat in safe mode

## Remaining Work

### 5. Serial Console Commands (HIGH PRIORITY)
- **Files**: `blinky-things/inputs/SerialConsole.h/cpp`
- **Tasks**:
  - [ ] Add ArduinoJson library dependency
  - [ ] Add `upload config <JSON>` command
    - Parse JSON device config
    - Validate fields
    - Convert to `StoredDeviceConfig`
    - Save to flash via `ConfigStorage`
    - Prompt user to reboot
  - [ ] Add `show config` command
    - Display current device configuration as JSON
    - Show validity status
    - Include LED count, pin, layout type
  - [ ] Update `json info` command to include device config status

### 6. Device Registry (MEDIUM PRIORITY)
- **Directory**: `devices/registry/`
- **Tasks**:
  - [ ] Create `hat_v1.json` (89 LEDs, linear layout)
  - [ ] Create `tube_v2.json` (60 LEDs, 4x15 matrix)
  - [ ] Create `bucket_v3.json` (128 LEDs, 16x8 matrix)
  - [ ] Document JSON schema with examples
  - [ ] Add validation script (optional)

### 7. Web Console Integration (MEDIUM PRIORITY)
- **Directory**: `blinky-console/`
- **Tasks**:
  - [ ] Add device config upload UI component
  - [ ] Device selector dropdown (loads from registry)
  - [ ] Custom JSON paste textarea
  - [ ] "Upload Config" button → sends to serial
  - [ ] Display current device status
  - [ ] Show safe mode indicator

### 8. Testing (HIGH PRIORITY)
- **Tasks**:
  - [ ] Test compilation of updated firmware
  - [ ] Test safe mode boot (no config in flash)
    - Verify audio still works
    - Verify serial console works
    - Verify LEDs are off
    - Verify heartbeat blink
  - [ ] Test device config upload via serial
    - Upload hat_v1 config
    - Reboot and verify correct initialization
    - Test LED output matches config
  - [ ] Test config switching
    - Upload tube_v2 config to hat device
    - Verify safe mode or validation failure
  - [ ] Test web console config upload
  - [ ] Test config persistence across reboots

### 9. Documentation (MEDIUM PRIORITY)
- **Files**: `docs/BUILD_GUIDE.md`, `docs/DEVELOPMENT.md`, `CLAUDE.md`
- **Tasks**:
  - [ ] Update build instructions (no more DEVICE_TYPE selection)
  - [ ] Document safe mode behavior
  - [ ] Document device config upload workflow
  - [ ] Update troubleshooting section
  - [ ] Add JSON schema reference
  - [ ] Update CLAUDE.md with new architecture

## JSON Schema (Draft)

```json
{
  "deviceId": "hat_v1",
  "deviceName": "Festival Hat v1",
  "ledWidth": 89,
  "ledHeight": 1,
  "ledPin": 0,
  "brightness": 100,
  "ledType": 12390,
  "orientation": 0,
  "layoutType": 1,
  "fastChargeEnabled": true,
  "lowBatteryThreshold": 3.6,
  "criticalBatteryThreshold": 3.4,
  "minVoltage": 3.0,
  "maxVoltage": 4.2,
  "upVectorX": 0.0,
  "upVectorY": 0.0,
  "upVectorZ": 1.0,
  "rotationDegrees": 0.0,
  "invertZ": false,
  "swapXY": false,
  "invertX": false,
  "invertY": false,
  "baudRate": 115200,
  "initTimeoutMs": 2000,
  "sampleRate": 16000,
  "bufferSize": 32,
  "baseCooling": 90,
  "sparkHeatMin": 200,
  "sparkHeatMax": 255,
  "sparkChance": 0.08,
  "audioSparkBoost": 0.8,
  "coolingAudioBias": -70,
  "bottomRowsForSparks": 1
}
```

## Benefits of This Architecture

1. **Single Universal Firmware**: One .hex file for all devices
2. **No Recompilation**: Change devices without touching code
3. **Field Configurable**: Update device config via serial/web
4. **Safe By Default**: Unconfigured devices don't drive LEDs blindly
5. **Scalable**: Easy to add 100+ device configs as JSON files
6. **Version Control Friendly**: Device configs are human-readable JSON
7. **Web UI Integration**: Click-to-configure device selection
8. **Persistent**: Config survives reboots and power cycles

## Next Steps

1. **Add Serial Commands** - `upload config` and `show config`
2. **Create Device Registry** - JSON files for existing 3 devices
3. **Test Compilation** - Ensure firmware compiles and boots
4. **Test Safe Mode** - Verify behavior with no config
5. **Test Config Upload** - Verify full workflow end-to-end

## Estimated Firmware Size Impact

- **Before**: ~172 KB (single device config compiled in)
- **After**: ~175 KB (config loading logic + validation)
- **Flash Usage**: +3 KB firmware, +128 bytes config storage
- **Result**: Minimal size increase for universal compatibility

## Risk Assessment

**Low Risk**:
- Config validation prevents invalid hardware settings
- Safe mode protects against LED misconfiguration
- Audio/Serial always functional for recovery
- Backward compatibility (old configs discarded, defaults loaded)

**Mitigation**:
- Comprehensive validation in `DeviceConfigLoader::validate()`
- Safe mode fallback if config invalid
- Built-in LED heartbeat for user feedback
- Serial console always available for reconfiguration

---

**Status**: Implementation 60% complete
**Next Action**: Add serial console commands for config upload
**Target Date**: TBD
