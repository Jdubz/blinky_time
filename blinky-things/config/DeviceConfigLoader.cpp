#include "DeviceConfigLoader.h"
#include "../inputs/SerialConsole.h"
#include "../hal/PlatformDetect.h"
#include "../hal/PlatformConstants.h"

// Static storage for deviceName string only (since DeviceConfig.deviceName is const char*)
// Other fields are copied by value, so they can be local variables
static char deviceNameBuffer[32];

bool DeviceConfigLoader::loadFromFlash(const ConfigStorage& storage, DeviceConfig& outConfig) {
    if (!storage.isDeviceConfigValid()) {
        SerialConsole::logDebug(F("No valid device config in flash"));
        return false;
    }

    const ConfigStorage::StoredDeviceConfig& stored = storage.getDeviceConfig();

    // Validate config
    if (!validate(stored)) {
        SerialConsole::logWarn(F("Device config failed validation"));
        return false;
    }

    // Convert string fields (copy to static buffer since DeviceConfig.deviceName is const char*)
    strncpy(deviceNameBuffer, stored.deviceName, sizeof(deviceNameBuffer) - 1);
    deviceNameBuffer[sizeof(deviceNameBuffer) - 1] = '\0';

    // Convert to local structs (copied by value into outConfig)
    MatrixConfig matrix;
    matrix.width = stored.ledWidth;
    matrix.height = stored.ledHeight;
    matrix.ledPin = stored.ledPin;
    matrix.ledPin2 = stored.ledPin2;
    matrix.brightness = stored.brightness;
    matrix.ledType = stored.ledType;
    matrix.orientation = (MatrixOrientation)stored.orientation;
    matrix.layoutType = (LayoutType)stored.layoutType;

    // Single boolean. When false, blinky-things.ino skips
    // BatteryMonitor allocation entirely; when true, it allocates and
    // sources thresholds from `Platform::Battery::*` at the point of
    // use (no intermediary fields). The legacy per-device threshold
    // bytes in StoredDeviceConfig (fastChargeEnabled, lowBattery*,
    // critical*, minVoltage, maxVoltage) are intentionally ignored —
    // they exist for flash-layout back-compat but no longer
    // participate in runtime behavior.
    ChargingConfig charging;
    charging.battery = stored.battery;

    IMUConfig imu;
    imu.upVectorX = stored.upVectorX;
    imu.upVectorY = stored.upVectorY;
    imu.upVectorZ = stored.upVectorZ;
    imu.rotationDegrees = stored.rotationDegrees;
    imu.invertZ = stored.invertZ;
    imu.swapXY = stored.swapXY;
    imu.invertX = stored.invertX;
    imu.invertY = stored.invertY;

    BlinkySerialConfig serial;
    serial.baudRate = stored.baudRate;
    serial.initTimeoutMs = stored.initTimeoutMs;

    MicConfig mic;
    mic.sampleRate = stored.sampleRate;
    mic.bufferSize = stored.bufferSize;

    InputConfig input;
    input.buttonPin = stored.buttonPin;

    // Populate output DeviceConfig (structs copied by value, deviceName by pointer)
    outConfig.deviceName = deviceNameBuffer;
    outConfig.matrix = matrix;
    outConfig.charging = charging;
    outConfig.imu = imu;
    outConfig.serial = serial;
    outConfig.microphone = mic;
    outConfig.input = input;

    if (SerialConsole::getGlobalLogLevel() >= LogLevel::INFO) {
        Serial.print(F("[INFO] Loaded device: "));
        Serial.print(stored.deviceName);
        Serial.print(F(" ("));
        Serial.print(stored.ledWidth);
        Serial.print(F("x"));
        Serial.print(stored.ledHeight);
        Serial.print(F(" = "));
        Serial.print(stored.ledWidth * stored.ledHeight);
        Serial.println(F(" LEDs)"));
    }

    return true;
}

void DeviceConfigLoader::convertToStored(const DeviceConfig& config, ConfigStorage::StoredDeviceConfig& outStored) {
    // Copy device name
    strncpy(outStored.deviceName, config.deviceName, sizeof(outStored.deviceName) - 1);
    outStored.deviceName[sizeof(outStored.deviceName) - 1] = '\0';

    // Generate device ID from name (replace spaces with underscores, lowercase)
    strncpy(outStored.deviceId, config.deviceName, sizeof(outStored.deviceId) - 1);
    outStored.deviceId[sizeof(outStored.deviceId) - 1] = '\0';
    for (int i = 0; outStored.deviceId[i]; i++) {
        if (outStored.deviceId[i] == ' ') outStored.deviceId[i] = '_';
        else if (outStored.deviceId[i] >= 'A' && outStored.deviceId[i] <= 'Z') {
            outStored.deviceId[i] += 32;  // To lowercase
        }
    }

    // Copy matrix config
    outStored.ledWidth = config.matrix.width;
    outStored.ledHeight = config.matrix.height;
    outStored.ledPin = config.matrix.ledPin;
    outStored.ledPin2 = config.matrix.ledPin2;
    outStored.brightness = config.matrix.brightness;
    outStored.ledType = config.matrix.ledType;
    outStored.orientation = (uint8_t)config.matrix.orientation;
    outStored.layoutType = (uint8_t)config.matrix.layoutType;

    // Copy charging config. Only the `battery` byte is load-bearing now;
    // the legacy threshold/voltage/fastCharge bytes stay in
    // StoredDeviceConfig for flash layout back-compat but are no longer
    // written from the runtime side. They're zero-initialised in
    // uploadDeviceConfig and stay zero, which is fine because
    // loadFromFlash doesn't read them.
    outStored.battery = config.charging.battery;

    // Copy IMU config
    outStored.upVectorX = config.imu.upVectorX;
    outStored.upVectorY = config.imu.upVectorY;
    outStored.upVectorZ = config.imu.upVectorZ;
    outStored.rotationDegrees = config.imu.rotationDegrees;
    outStored.invertZ = config.imu.invertZ;
    outStored.swapXY = config.imu.swapXY;
    outStored.invertX = config.imu.invertX;
    outStored.invertY = config.imu.invertY;

    // Copy serial config
    outStored.baudRate = config.serial.baudRate;
    outStored.initTimeoutMs = config.serial.initTimeoutMs;

    // Copy mic config
    outStored.sampleRate = config.microphone.sampleRate;
    outStored.bufferSize = config.microphone.bufferSize;

    // Copy input config
    outStored.buttonPin = config.input.buttonPin;

    // Mark as valid
    outStored.isValid = true;

    // Clear reserved bytes
    memset(outStored.reserved, 0, sizeof(outStored.reserved));
}

bool DeviceConfigLoader::validate(const ConfigStorage::StoredDeviceConfig& stored) {
    // Check validity flag
    if (!stored.isValid) {
        SerialConsole::logDebug(F("Device config marked invalid"));
        return false;
    }

    // Validate LED count
    uint16_t ledCount = stored.ledWidth * stored.ledHeight;
    if (ledCount == 0) {
        SerialConsole::logWarn(F("Invalid LED count: 0"));
        return false;
    }
    // 2048 cap covers the largest current device (32×32 = 1024 LEDs) with headroom.
    // nRF52840 practical ceiling is ~512 LEDs before pixel buffers exhaust its 256 KB RAM;
    // the firmware will compile but may malloc-fail at runtime on that platform.
    if (ledCount > 2048) {
        SerialConsole::logWarn(F("LED count too high (>2048)"));
        return false;
    }

    // Validate LED pin — upper bound is platform-dependent
    // nRF52840: GPIO 0-47 (P0.00-P0.31, P1.00-P1.15)
    // ESP32-S3: GPIO 0-48
#ifdef BLINKY_PLATFORM_NRF52840
    constexpr uint8_t LED_PIN_MAX = 47;
#else
    constexpr uint8_t LED_PIN_MAX = 48;
#endif
    if (stored.ledPin > LED_PIN_MAX) {
        SerialConsole::logWarn(F("Invalid LED pin"));
        return false;
    }

    // Validate second LED pin (multi-strand). 0 means single-strand; any
    // other value must be a valid pin, distinct from ledPin, and the total
    // pixel count must be even (so the buffer can split cleanly across
    // strands).
    if (stored.ledPin2 != 0) {
        if (stored.ledPin2 > LED_PIN_MAX) {
            SerialConsole::logWarn(F("Invalid ledPin2"));
            return false;
        }
        if (stored.ledPin2 == stored.ledPin) {
            SerialConsole::logWarn(F("ledPin2 must differ from ledPin"));
            return false;
        }
        if ((ledCount & 1) != 0) {
            SerialConsole::logWarn(F("Multi-strand requires even total LED count"));
            return false;
        }
    }

    // Validate ledType: the lower byte encodes 2-bit fields for R/G/B byte
    // offsets in the on-wire pixel stream. Nrf52PwmLedStrip's constructor
    // (per the PR 142 buffer-overflow fix) only allocates 3 bytes per pixel,
    // so:
    //   - each offset must be 0-2 (no NEO_RGBW types)
    //   - r, g, b offsets must be DISTINCT (no two channels writing to the
    //     same byte; otherwise one channel silently overwrites the other)
    //
    // Catching this here means a bad ledType is rejected at upload time
    // (uploadDeviceConfig → validate → reject; nothing persisted) and at
    // boot from flash (loadFromFlash → validate → returns false; device
    // enters safe mode instead of haltWithError-looping into BLE-DFU).
    //
    // 2026-05-18: cart_inner / cart_outer shipped with ledType=12390
    // (0x3066), lower byte 0x66 → r=2, g=1, b=2 — duplicate r/b offset.
    // Pre-PR-142 the driver silently corrupted colors (R and B both wrote
    // byte slot 2); post-PR-142 it correctly refused to construct, and
    // setup()'s haltWithError put the device into a 3-strikes-out
    // BLE-DFU recovery cycle. See docs/POSTMORTEM_2026_05_18_LEDTYPE.md.
    {
        uint8_t order = static_cast<uint8_t>(stored.ledType & 0xFF);
        uint8_t bOff = (order >> 0) & 0x3;
        uint8_t gOff = (order >> 2) & 0x3;
        uint8_t rOff = (order >> 4) & 0x3;
        bool inRange = (rOff <= 2) && (gOff <= 2) && (bOff <= 2);
        bool distinct = (rOff != gOff) && (rOff != bOff) && (gOff != bOff);
        if (!inRange || !distinct) {
            Serial.print(F("[WARN] Invalid ledType 0x"));
            Serial.print(stored.ledType, HEX);
            Serial.print(F(" (decoded r="));
            Serial.print(rOff);
            Serial.print(F(" g="));
            Serial.print(gOff);
            Serial.print(F(" b="));
            Serial.print(bOff);
            Serial.print(F("): "));
            Serial.println(inRange ? F("offsets must be distinct")
                                   : F("offsets must be 0-2 (no RGBW)"));
            Serial.println(F("[WARN] Valid examples: 82 (NEO_GRB), 6 (NEO_RGB)"));
            return false;
        }
    }

    // Validate brightness (0-255)
    // Note: 0 is valid (LEDs off)
    if (stored.brightness > 255) {
        SerialConsole::logWarn(F("Brightness > 255"));
        return false;
    }

    // Validate orientation (must match MatrixOrientation enum in DeviceConfig.h)
    // 0=HORIZONTAL, 1=VERTICAL, 2=PANEL_GRID, 3=HORIZONTAL_ZIGZAG
    if (stored.orientation > 3) {
        SerialConsole::logWarn(F("Invalid orientation"));
        return false;
    }

    // Validate buttonPin (0 = unused; any other value must be a valid GPIO
    // distinct from ledPin / ledPin2 to prevent stomping on LED data).
    if (stored.buttonPin != 0) {
        if (stored.buttonPin > LED_PIN_MAX) {
            SerialConsole::logWarn(F("Invalid buttonPin"));
            return false;
        }
        if (stored.buttonPin == stored.ledPin || stored.buttonPin == stored.ledPin2) {
            SerialConsole::logWarn(F("buttonPin must differ from ledPin/ledPin2"));
            return false;
        }
    }

    // Validate layout type
    if (stored.layoutType > 2) {  // 0=MATRIX, 1=LINEAR, 2=RANDOM
        SerialConsole::logWarn(F("Invalid layout type"));
        return false;
    }

    // Voltage range check removed — the per-device minVoltage/maxVoltage
    // bytes in StoredDeviceConfig are no longer load-bearing. They're
    // zeroed by uploadDeviceConfig and ignored at load time;
    // battery-equipped devices source thresholds from
    // `Platform::Battery::*` constants in blinky-things.ino. Leaving
    // the check in would falsely reject any new upload with
    // `"battery": true` (zeroed fields make `0 >= 0` true → fail).
    // See StoredDeviceConfig::battery for the schema rationale.

    // Validate sample rate (common PDM rates: 8000, 16000, 32000, 44100, 48000)
    // Intentionally warn-only (not hard error) to allow flexibility for:
    // - Custom PDM configurations
    // - Testing non-standard rates
    // - Future hardware that supports different rates
    if (stored.sampleRate != 8000 && stored.sampleRate != 16000 &&
        stored.sampleRate != 32000 && stored.sampleRate != 44100 &&
        stored.sampleRate != 48000) {
        SerialConsole::logWarn(F("Non-standard sample rate (may fail at runtime)"));
    }

    // Validate baud rate (common values: 9600-230400)
    // Intentionally warn-only to allow:
    // - Custom serial configurations
    // - Non-standard terminal setups
    // - Future use cases
    if (stored.baudRate != 9600 && stored.baudRate != 19200 &&
        stored.baudRate != 38400 && stored.baudRate != 57600 &&
        stored.baudRate != 115200 && stored.baudRate != 230400) {
        SerialConsole::logWarn(F("Non-standard baud rate (ensure terminal matches)"));
    }

    return true;
}
