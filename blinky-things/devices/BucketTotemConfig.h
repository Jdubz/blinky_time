#pragma once
#include "DeviceConfig.h"
#include "../config/TotemDefaults.h"

// Bucket Totem: 16x8 horizontal matrix (128 LEDs total)
// Physical orientation: HORIZONTAL (standard row-major layout)
// Layout: 16 columns of 8 LEDs each, standard wiring pattern
// Row 0: LEDs 0-15 (left to right)
// Row 1: LEDs 16-31 (left to right)
// Row 2: LEDs 32-47 (left to right)
// Row 3: LEDs 48-63 (left to right)
// Row 4: LEDs 64-79 (left to right)
// Row 5: LEDs 80-95 (left to right)
// Row 6: LEDs 96-111 (left to right)
// Row 7: LEDs 112-127 (left to right)
// Top row: LEDs 0-15 | Bottom row: LEDs 112-127

const DeviceConfig BUCKET_TOTEM_CONFIG = {
  .deviceName = "Bucket Totem",
  .matrix = {
    .width = 16,
    .height = 8,
    .ledPin = D10,
    .brightness = 80,
    .ledType = NEO_RGB + NEO_KHZ800,
    .orientation = HORIZONTAL,
    .layoutType = MATRIX_LAYOUT,      // New unified layout system
    .fireType = MATRIX_FIRE           // Kept for backward compatibility
  },
  // All XIAO BLE devices use single-cell LiPo batteries (3.0-4.2V range)
  .charging = {
    .fastChargeEnabled = true,
    .lowBatteryThreshold = Platform::Battery::DEFAULT_LOW_THRESHOLD,
    .criticalBatteryThreshold = Platform::Battery::DEFAULT_CRITICAL_THRESHOLD,
    .minVoltage = Platform::Battery::VOLTAGE_EMPTY,
    .maxVoltage = Platform::Battery::VOLTAGE_FULL
  },
  .imu = {
    .upVectorX = 0.0f,
    .upVectorY = 0.0f,
    .upVectorZ = 1.0f,
    .invertZ = true,
    .rotationDegrees = 0.0f,
    .swapXY = false,
    .invertX = false,
    .invertY = false
  },
  .serial = {
    .baudRate = 115200,
    .initTimeoutMs = 3000
  },
  .microphone = {
    .sampleRate = 16000,
    .bufferSize = 32
  },
  .fireDefaults = {
    // Bucket totem fire parameters - uses standard totem defaults
    .baseCooling = 85,         // Standard cooling rate
    .sparkHeatMin = 40,        // Standard spark heat range
    .sparkHeatMax = 200,       // Standard maximum
    .sparkChance = 0.32f,      // Standard spark probability
    .audioSparkBoost = 0.3f,   // Standard audio boost
    .audioHeatBoostMax = 60,   // Standard audio heat boost
    .coolingAudioBias = -20,   // Standard audio cooling bias
    .bottomRowsForSparks = 1,  // Single bottom row for sparks
    .transientHeatMax = 100    // Standard transient heat maximum
  }
};
