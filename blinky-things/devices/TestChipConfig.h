#pragma once
#include "DeviceConfig.h"
#include "../config/TotemDefaults.h"

// Test Chip: Bare XIAO nRF52840 Sense with no enclosure or LED strip.
// Gets the device out of safe mode so audio analysis, NN inference,
// and serial streaming all work. LEDs render to a minimal 1-LED virtual
// strip (pin D0) — harmless if nothing is connected.
//
// Use: `device upload {"deviceId":"test_chip","ledWidth":1,"ledHeight":1}`
// Or flash firmware that ships with this as the default config.

const DeviceConfig TEST_CHIP_CONFIG = {
  .deviceName = "Test Chip",
  .matrix = {
    .width = 1,         // Minimal 1-LED virtual strip
    .height = 1,
    .ledPin = D0,
    .brightness = 10,   // Dim (in case something is connected)
    .ledType = NEO_GRB + NEO_KHZ800,
    .orientation = HORIZONTAL,
    .layoutType = LINEAR_LAYOUT
  },
  .charging = {
    .fastChargeEnabled = false,
    .lowBatteryThreshold = 3.5f,
    .criticalBatteryThreshold = 3.3f,
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
    .initTimeoutMs = 2000
  },
  .microphone = {
    .sampleRate = 16000,
    .bufferSize = 32
  }
};
