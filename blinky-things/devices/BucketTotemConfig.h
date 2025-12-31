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
    .ledType = NEO_GRB + NEO_KHZ800,  // Standard GRB for WS2812B
    .orientation = HORIZONTAL,
    .layoutType = MATRIX_LAYOUT
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
    // Bucket totem fire parameters - tuned for 16x8 horizontal matrix
    .baseCooling = 25,         // Low cooling for tall flames
    .sparkHeatMin = 120,       // Hot sparks even without audio
    .sparkHeatMax = 255,       // Maximum heat on hits
    .sparkChance = 0.45f,      // Frequent sparks
    .audioSparkBoost = 0.5f,   // Strong audio reactivity
    .coolingAudioBias = -30,   // Flames persist longer with sound
    .bottomRowsForSparks = 2   // Two bottom rows for wider fire base
  }
};
