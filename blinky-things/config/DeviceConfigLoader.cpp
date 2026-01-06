#include "DeviceConfigLoader.h"
#include "../inputs/SerialConsole.h"

// Static storage for deviceName strings (since DeviceConfig uses const char*)
static char deviceNameBuffer[32];
static MatrixConfig matrixConfigBuffer;
static ChargingConfig chargingConfigBuffer;
static IMUConfig imuConfigBuffer;
static SerialConfig serialConfigBuffer;
static MicConfig micConfigBuffer;
static FireDefaults fireDefaultsBuffer;

bool DeviceConfigLoader::loadFromFlash(ConfigStorage& storage, DeviceConfig& outConfig) {
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

    // Convert string fields (copy to static buffers since DeviceConfig uses pointers)
    strncpy(deviceNameBuffer, stored.deviceName, sizeof(deviceNameBuffer) - 1);
    deviceNameBuffer[sizeof(deviceNameBuffer) - 1] = '\0';

    // Convert MatrixConfig
    matrixConfigBuffer.width = stored.ledWidth;
    matrixConfigBuffer.height = stored.ledHeight;
    matrixConfigBuffer.ledPin = stored.ledPin;
    matrixConfigBuffer.brightness = stored.brightness;
    matrixConfigBuffer.ledType = stored.ledType;
    matrixConfigBuffer.orientation = (MatrixOrientation)stored.orientation;
    matrixConfigBuffer.layoutType = (LayoutType)stored.layoutType;

    // Convert ChargingConfig
    chargingConfigBuffer.fastChargeEnabled = stored.fastChargeEnabled;
    chargingConfigBuffer.lowBatteryThreshold = stored.lowBatteryThreshold;
    chargingConfigBuffer.criticalBatteryThreshold = stored.criticalBatteryThreshold;
    chargingConfigBuffer.minVoltage = stored.minVoltage;
    chargingConfigBuffer.maxVoltage = stored.maxVoltage;

    // Convert IMUConfig
    imuConfigBuffer.upVectorX = stored.upVectorX;
    imuConfigBuffer.upVectorY = stored.upVectorY;
    imuConfigBuffer.upVectorZ = stored.upVectorZ;
    imuConfigBuffer.rotationDegrees = stored.rotationDegrees;
    imuConfigBuffer.invertZ = stored.invertZ;
    imuConfigBuffer.swapXY = stored.swapXY;
    imuConfigBuffer.invertX = stored.invertX;
    imuConfigBuffer.invertY = stored.invertY;

    // Convert SerialConfig
    serialConfigBuffer.baudRate = stored.baudRate;
    serialConfigBuffer.initTimeoutMs = stored.initTimeoutMs;

    // Convert MicConfig
    micConfigBuffer.sampleRate = stored.sampleRate;
    micConfigBuffer.bufferSize = stored.bufferSize;

    // Convert FireDefaults
    fireDefaultsBuffer.baseCooling = stored.baseCooling;
    fireDefaultsBuffer.sparkHeatMin = stored.sparkHeatMin;
    fireDefaultsBuffer.sparkHeatMax = stored.sparkHeatMax;
    fireDefaultsBuffer.sparkChance = stored.sparkChance;
    fireDefaultsBuffer.audioSparkBoost = stored.audioSparkBoost;
    fireDefaultsBuffer.coolingAudioBias = stored.coolingAudioBias;
    fireDefaultsBuffer.bottomRowsForSparks = stored.bottomRowsForSparks;

    // Populate output DeviceConfig with pointers to static buffers
    outConfig.deviceName = deviceNameBuffer;
    outConfig.matrix = matrixConfigBuffer;
    outConfig.charging = chargingConfigBuffer;
    outConfig.imu = imuConfigBuffer;
    outConfig.serial = serialConfigBuffer;
    outConfig.microphone = micConfigBuffer;
    outConfig.fireDefaults = fireDefaultsBuffer;

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

    // Validate brightness
    // Note: 0 is valid (LEDs off), so we don't check lower bound

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
    if (stored.sampleRate != 8000 && stored.sampleRate != 16000 &&
        stored.sampleRate != 32000 && stored.sampleRate != 44100 &&
        stored.sampleRate != 48000) {
        SerialConsole::logWarn(F("Non-standard sample rate"));
        // Not a hard error - allow it but warn
    }

    // Validate baud rate (common values)
    if (stored.baudRate != 9600 && stored.baudRate != 19200 &&
        stored.baudRate != 38400 && stored.baudRate != 57600 &&
        stored.baudRate != 115200 && stored.baudRate != 230400) {
        SerialConsole::logWarn(F("Non-standard baud rate"));
        // Not a hard error - allow it but warn
    }

    return true;
}
