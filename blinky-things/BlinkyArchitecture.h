#pragma once

/**
 * BlinkyArchitecture.h - Main include for the rendering pipeline architecture
 *
 * This file provides a single include for the Arduino IDE main sketch to access
 * all the visual architecture components while maintaining clean folder structure
 * for development and testing.
 *
 * Usage in main sketch:
 * #include "BlinkyArchitecture.h"
 *
 * Architecture Overview:
 * Inputs -> Generator -> Effect (optional) -> Render -> LEDs
 */

// Configuration and utilities
#include "config/Constants.h"
#include "config/Globals.h"
#include "config/TotemDefaults.h"
#include "config/ConfigStorage.h"     // Persistent settings storage
#include "config/SettingsRegistry.h"  // Settings abstraction layer

// Core data types
#include "types/PixelMatrix.h"

// Generators
#include "generators/Generator.h"     // Base generator class
#include "generators/Fire.h"          // Fire simulation generator
#include "generators/Water.h"         // Water flow generator
#include "generators/Lightning.h"     // Lightning bolt generator

// Effects
#include "effects/Effect.h"              // Base effect interface
#include "effects/HueRotationEffect.h"   // Hue rotation effect
#include "effects/NoOpEffect.h"          // Pass-through effect (no transformation)

// Render
#include "render/EffectRenderer.h"
#include "render/LEDMapper.h"

// Input components
// NOTE: Requires patched pinDefinitions.h with include guards (see docs/PLATFORM_FIX.md)
#include "inputs/AdaptiveMic.h"
#include "inputs/BatteryMonitor.h"
#include "inputs/IMUHelper.h"     // Compiles without LSM6DS3; define IMU_ENABLED to activate
#include "inputs/SerialConsole.h"

// Testing (for development/debugging)
#ifdef ENABLE_TESTING
#include "tests/GeneratorTestRunner.h"
#endif

/**
 * Architecture Usage Example:
 *
 * #include "BlinkyArchitecture.h"
 *
 * FireGenerator fireGen;
 * HueRotationEffect hueEffect(0.1f);
 * EffectRenderer renderer;
 * EffectMatrix matrix(width, height);
 *
 * void setup() {
 *   fireGen.begin(width, height);
 *   hueEffect.begin(width, height);
 *   renderer.begin(width, height, &leds);
 * }
 *
 * void loop() {
 *   fireGen.setAudioInput(energy, hit);
 *   fireGen.update();
 *   fireGen.generate(&matrix);
 *   hueEffect.apply(&matrix);
 *   renderer.render(&matrix);
 * }
 */
