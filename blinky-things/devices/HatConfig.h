#pragma once
#include "DeviceConfig.h"
#include "../config/TotemDefaults.h"

// Hat LED String: 89 LEDs in linear arrangement using LINEAR_LAYOUT
// Physical layout: Single string of 89 LEDs arranged around hat brim
// Layout type: LINEAR_LAYOUT - sideways heat dissipation, max-only heat combination
// Layout: LEDs 0-88 in sequence around hat circumference
//
// Design considerations for hat use:
// - Linear string (89 LEDs) for comfortable wear around hat brim
// - LINEAR_LAYOUT: heat dissipates sideways instead of upward
// - Visible brightness (100/255) tuned for outdoor/indoor use
// - Enhanced audio sensitivity optimized for head-mounted microphone position
// - Motion-aware settings for walking/movement
// - Higher battery thresholds for critical wearable device
// - Calm fire parameters: slow cooling (25), farther spread (12 LEDs), less decay (0.92)
// - Max-only heat combination prevents harsh bright spots
// - Fewer sparks (3 positions) for smoother, less chaotic effect

const DeviceConfig HAT_CONFIG = {
  .deviceName = "Hat Display",
  .matrix = {
    .width = 89,        // 89 LEDs in linear string
    .height = 1,        // Single row for string mode
    .ledPin = D0,
    .brightness = 100,  // Increased brightness for visibility
    .ledType = NEO_GRB + NEO_KHZ800,  // Changed from RGB to GRB for correct colors
    .orientation = HORIZONTAL,
    .layoutType = LINEAR_LAYOUT
  },
  .charging = {
    .fastChargeEnabled = true,
    // Higher thresholds for wearable safety (above Platform::Battery defaults)
    .lowBatteryThreshold = 3.6f,        // LiPo ~20%, higher for wearable safety
    .criticalBatteryThreshold = 3.4f,   // LiPo ~5%, earlier warning for hat use
    .minVoltage = Platform::Battery::VOLTAGE_EMPTY,
    .maxVoltage = Platform::Battery::VOLTAGE_FULL
  },
  .imu = {
    .upVectorX = 0.0f,
    .upVectorY = 0.0f,
    .upVectorZ = 1.0f,
    .invertZ = false,            // Standard orientation for hat mounting
    .rotationDegrees = 0.0f,     // Assume forward-facing mount
    .swapXY = false,
    .invertX = false,
    .invertY = false
  },
  .serial = {
    .baudRate = 115200,
    .initTimeoutMs = 2000        // Shorter timeout for wearable device
  },
  .microphone = {
    .sampleRate = 16000,         // Standard rate
    .bufferSize = 32             // Larger buffer for head movement noise
  },
  .fireDefaults = {
    // Hat fire parameters - PUNCHY beat-reactive fire
    .baseCooling = 90,          // Very fast cooling = quick fade to dark
    .sparkHeatMin = 200,        // Bright sparks
    .sparkHeatMax = 255,        // Maximum brightness
    .sparkChance = 0.080f,      // LOW base rate = quiet when no audio
    .audioSparkBoost = 0.800f,  // HUGE audio boost = explosive on hits
    .coolingAudioBias = -70,    // Big cooling reduction on audio
    .bottomRowsForSparks = 1    // Not relevant for string fire
  }
};
