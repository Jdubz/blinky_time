#include "DeviceConfigLoader.h"
#include "../inputs/SerialConsole.h"

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
    matrix.brightness = stored.brightness;
    matrix.ledType = stored.ledType;
    matrix.orientation = (MatrixOrientation)stored.orientation;
    matrix.layoutType = (LayoutType)stored.layoutType;

    ChargingConfig charging;
    charging.fastChargeEnabled = stored.fastChargeEnabled;
    charging.lowBatteryThreshold = stored.lowBatteryThreshold;
    charging.criticalBatteryThreshold = stored.criticalBatteryThreshold;
    charging.minVoltage = stored.minVoltage;
    charging.maxVoltage = stored.maxVoltage;

    IMUConfig imu;
    imu.upVectorX = stored.upVectorX;
    imu.upVectorY = stored.upVectorY;
    imu.upVectorZ = stored.upVectorZ;
    imu.rotationDegrees = stored.rotationDegrees;
    imu.invertZ = stored.invertZ;
    imu.swapXY = stored.swapXY;
    imu.invertX = stored.invertX;
    imu.invertY = stored.invertY;

    SerialConfig serial;
    serial.baudRate = stored.baudRate;
    serial.initTimeoutMs = stored.initTimeoutMs;

    MicConfig mic;
    mic.sampleRate = stored.sampleRate;
    mic.bufferSize = stored.bufferSize;

    FireDefaults fire;
    fire.baseCooling = stored.baseCooling;
    fire.sparkHeatMin = stored.sparkHeatMin;
    fire.sparkHeatMax = stored.sparkHeatMax;
    fire.sparkChance = stored.sparkChance;
    fire.audioSparkBoost = stored.audioSparkBoost;
    fire.coolingAudioBias = stored.coolingAudioBias;
    fire.bottomRowsForSparks = stored.bottomRowsForSparks;

    // Populate output DeviceConfig (structs copied by value, deviceName by pointer)
    outConfig.deviceName = deviceNameBuffer;
    outConfig.matrix = matrix;
    outConfig.charging = charging;
    outConfig.imu = imu;
    outConfig.serial = serial;
    outConfig.microphone = mic;
    outConfig.fireDefaults = fire;

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
    outStored.brightness = config.matrix.brightness;
    outStored.ledType = config.matrix.ledType;
    outStored.orientation = (uint8_t)config.matrix.orientation;
    outStored.layoutType = (uint8_t)config.matrix.layoutType;

    // Copy charging config
    outStored.fastChargeEnabled = config.charging.fastChargeEnabled;
    outStored.lowBatteryThreshold = config.charging.lowBatteryThreshold;
    outStored.criticalBatteryThreshold = config.charging.criticalBatteryThreshold;
    outStored.minVoltage = config.charging.minVoltage;
    outStored.maxVoltage = config.charging.maxVoltage;

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

    // Copy fire defaults
    outStored.baseCooling = config.fireDefaults.baseCooling;
    outStored.sparkHeatMin = config.fireDefaults.sparkHeatMin;
    outStored.sparkHeatMax = config.fireDefaults.sparkHeatMax;
    outStored.sparkChance = config.fireDefaults.sparkChance;
    outStored.audioSparkBoost = config.fireDefaults.audioSparkBoost;
    outStored.coolingAudioBias = config.fireDefaults.coolingAudioBias;
    outStored.bottomRowsForSparks = config.fireDefaults.bottomRowsForSparks;

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
    if (ledCount > 500) {
        SerialConsole::logWarn(F("LED count too high (>500)"));
        return false;
    }

    // Validate LED pin (common GPIO pins on nRF52840)
    if (stored.ledPin > 48) {  // nRF52840 has up to 48 GPIO pins
        SerialConsole::logWarn(F("Invalid LED pin"));
        return false;
    }

    // Validate brightness (0-255)
    // Note: 0 is valid (LEDs off)
    if (stored.brightness > 255) {
        SerialConsole::logWarn(F("Brightness > 255"));
        return false;
    }

    // Validate orientation
    if (stored.orientation > 1) {  // 0=HORIZONTAL, 1=VERTICAL
        SerialConsole::logWarn(F("Invalid orientation"));
        return false;
    }

    // Validate layout type
    if (stored.layoutType > 2) {  // 0=MATRIX, 1=LINEAR, 2=RANDOM
        SerialConsole::logWarn(F("Invalid layout type"));
        return false;
    }

    // Validate voltage ranges
    if (stored.minVoltage >= stored.maxVoltage) {
        SerialConsole::logWarn(F("Invalid voltage range"));
        return false;
    }
    if (stored.minVoltage < 2.5f || stored.maxVoltage > 5.0f) {
        SerialConsole::logWarn(F("Voltage out of safe range"));
        return false;
    }

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
