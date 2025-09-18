#pragma once
#include "DeviceConfig.h"
#include "../TotemDefaults.h"
#include <Adafruit_NeoPixel.h>

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
    .fireType = MATRIX_FIRE
  },
  .charging = {
    .fastChargeEnabled = true,
    .lowBatteryThreshold = 1.5f,
    .criticalBatteryThreshold = 1.3f,
    .autoShowVisualizationWhenCharging = true,
    .minVoltage = 1.3f,
    .maxVoltage = 1.8f
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
    .baseCooling = Defaults::BaseCooling,
    .sparkHeatMin = Defaults::SparkHeatMin,
    .sparkHeatMax = Defaults::SparkHeatMax,
    .sparkChance = Defaults::SparkChance,
    .audioSparkBoost = Defaults::AudioSparkBoost,
    .audioHeatBoostMax = Defaults::AudioHeatBoostMax,
    .coolingAudioBias = Defaults::CoolingAudioBias,
    .bottomRowsForSparks = Defaults::BottomRowsForSparks,
    .transientHeatMax = Defaults::TransientHeatMax
  }
};