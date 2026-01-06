#pragma once

#include "../devices/DeviceConfig.h"
#include "ConfigStorage.h"

/**
 * DeviceConfigLoader - Utilities for loading device configuration from flash
 *
 * This class handles the conversion between StoredDeviceConfig (flash-serializable)
 * and DeviceConfig (runtime structure with pointers).
 *
 * Usage:
 *   DeviceConfig runtimeConfig;
 *   if (DeviceConfigLoader::loadFromFlash(configStorage, runtimeConfig)) {
 *       // Config loaded successfully
 *   } else {
 *       // No valid config, enter safe mode
 *   }
 */
class DeviceConfigLoader {
public:
    /**
     * Load device config from flash storage and convert to runtime format
     *
     * @param storage ConfigStorage instance to load from
     * @param outConfig Output DeviceConfig structure (will be populated)
     * @return true if valid config loaded, false if unconfigured/invalid
     */
    static bool loadFromFlash(ConfigStorage& storage, DeviceConfig& outConfig);

    /**
     * Convert runtime DeviceConfig to flash-storable format
     *
     * @param config Runtime DeviceConfig to convert
     * @param outStored Output StoredDeviceConfig (will be populated)
     */
    static void convertToStored(const DeviceConfig& config, ConfigStorage::StoredDeviceConfig& outStored);

    /**
     * Validate that a device config is sane (non-zero LEDs, valid pins, etc.)
     *
     * @param stored StoredDeviceConfig to validate
     * @return true if config is valid and safe to use
     */
    static bool validate(const ConfigStorage::StoredDeviceConfig& stored);

private:
    // Static utility class - no instantiation
    DeviceConfigLoader() = delete;
};
