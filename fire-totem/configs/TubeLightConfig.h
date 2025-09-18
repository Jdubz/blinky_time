#pragma once
#include "DeviceConfig.h"
#include "TubeLightDefaults.h"
#include <Adafruit_NeoPixel.h>

// Tube Light: 4x16 zigzag matrix (64 LEDs total)
// Physical orientation: VERTICAL (strip runs top to bottom)
// Layout: 4 columns of 16 LEDs each, zigzag wiring pattern
// Col 0: LEDs 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15     (top to bottom)
// Col 1: LEDs 31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16 (bottom to top)
// Col 2: LEDs 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47 (top to bottom)
// Col 3: LEDs 63,62,61,60,59,58,57,56,55,54,53,52,51,50,49,48 (bottom to top)
// Bottom row: LEDs 0,16,32,48

const DeviceConfig TUBE_LIGHT_CONFIG = {
  .deviceName = "Tube Light",
  .matrix = {
    .width = 4,
    .height = 16,
    .ledPin = D10,
    .brightness = 120,
    .ledType = NEO_RGB + NEO_KHZ800,
    .orientation = VERTICAL
  },
  .charging = {
    .fastChargeEnabled = true,
    .lowBatteryThreshold = 1.5f,
    .criticalBatteryThreshold = 1.3f,
    .autoShowVisualizationWhenCharging = false,
    .minVoltage = 1.3f,
    .maxVoltage = 1.8f
  },
  .imu = {
    .upVectorX = 0.0f,
    .upVectorY = 0.0f,
    .upVectorZ = 1.0f,
    .invertZ = false,
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
    .baseCooling = TubeLightDefaults::BaseCooling,
    .sparkHeatMin = TubeLightDefaults::SparkHeatMin,
    .sparkHeatMax = TubeLightDefaults::SparkHeatMax,
    .sparkChance = TubeLightDefaults::SparkChance,
    .audioSparkBoost = TubeLightDefaults::AudioSparkBoost,
    .audioHeatBoostMax = TubeLightDefaults::AudioHeatBoostMax,
    .coolingAudioBias = TubeLightDefaults::CoolingAudioBias,
    .bottomRowsForSparks = TubeLightDefaults::BottomRowsForSparks,
    .transientHeatMax = TubeLightDefaults::TransientHeatMax
  }
};