#pragma once
#include "DeviceConfig.h"
#include "../config/TotemDefaults.h"

// Long Tube: 4x60 zigzag matrix (240 LEDs total)
// Physical orientation: VERTICAL (strip runs top to bottom)
// Layout: 4 columns of 60 LEDs each, zigzag wiring pattern
// Col 0: LEDs 0-59    (top to bottom)
// Col 1: LEDs 119-60  (bottom to top)
// Col 2: LEDs 120-179 (top to bottom)
// Col 3: LEDs 239-180 (bottom to top)
// Same form factor as archive/long-tube/ but running blinky-things firmware

const DeviceConfig LONG_TUBE_CONFIG = {
  .deviceName = "Long Tube",
  .matrix = {
    .width = 4,
    .height = 60,
    .ledPin = D10,
    .brightness = 80,
    .ledType = NEO_GRB + NEO_KHZ800,  // CRITICAL: NEO_GRB for nRF52840 XIAO Sense
    .orientation = VERTICAL,
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
    // Long tube fire parameters - tuned for tall 60-row vertical matrix
    // Lower cooling than tube light (15 rows) so flames propagate higher
    .baseCooling = 20,        // Low cooling for tall matrix (tube: 40)
    .sparkHeatMin = 60,       // Slightly higher min for visibility over 60 rows
    .sparkHeatMax = 220,      // High max for bright base
    .sparkChance = 0.150f,    // Moderate spark rate
    .audioSparkBoost = 0.350f, // Audio responsiveness
    .coolingAudioBias = -15,   // Moderate cooling reduction on audio
    .bottomRowsForSparks = 2   // Two rows for wider fire base on tall matrix
  }
};
