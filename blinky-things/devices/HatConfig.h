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
    .layoutType = LINEAR_LAYOUT,  // New unified layout system
    .fireType = STRING_FIRE       // Kept for backward compatibility
  },
  .charging = {
    .fastChargeEnabled = true,
    .lowBatteryThreshold = 1.4f,        // Higher threshold for critical device
    .criticalBatteryThreshold = 1.2f,   // Earlier warning for hat use
    .autoShowVisualizationWhenCharging = true,
    .minVoltage = 1.2f,
    .maxVoltage = 1.8f
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
    // Hat fire parameters - bright oozing fire with strong audio response
    .baseCooling = 12,          // Slightly faster cooling for brightness balance
    .sparkHeatMin = 100,        // Higher minimum for more brightness
    .sparkHeatMax = 200,        // Higher maximum for strong brightness
    .sparkChance = 0.120f,      // More sparks for better brightness
    .audioSparkBoost = 0.400f,  // Much higher audio sensitivity
    .audioHeatBoostMax = 80,    // Strong audio boost for responsiveness
    .coolingAudioBias = -15,    // Moderate negative bias for slow decay
    .bottomRowsForSparks = 1,   // Not relevant for string fire, set to 1
    .transientHeatMax = 120     // Higher transient for strong audio response
  }
};
