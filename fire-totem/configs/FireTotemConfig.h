#pragma once
#include "DeviceConfig.h"
#include "../TotemDefaults.h"
#include <Adafruit_NeoPixel.h>

const DeviceConfig FIRE_TOTEM_CONFIG = {
  .deviceName = "Fire Totem",
  .matrix = {
    .width = 16,
    .height = 8,
    .ledPin = D10,
    .brightness = 80,
    .ledType = NEO_RGB + NEO_KHZ800,
    .orientation = HORIZONTAL
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