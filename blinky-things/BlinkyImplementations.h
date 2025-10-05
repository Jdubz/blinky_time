#pragma once

/**
 * BlinkyImplementations.h - Include all implementation files
 *
 * Arduino IDE doesn't automatically compile .cpp files in subdirectories.
 * This file includes all implementations to ensure they're compiled.
 * 
 * CURRENT ARCHITECTURE STATUS:
 * ✅ Core: Generator→Effects→Renderer pipeline operational
 * ✅ UnifiedFireGenerator: Supports all layout types (MATRIX, LINEAR, RANDOM)
 * ✅ Hardware: AdaptiveMic and BatteryMonitor ready
 * ⚠️  IMUHelper: Disabled until LSM6DS3 library dependency resolved
 * ⚠️  SerialConsole: Disabled until updated for unified architecture
 * ⚠️  ConfigStorage: Disabled until legacy fire params cleaned up
 */

// Core implementations
#include "core/EffectMatrix.cpp"

// Generator implementations
#include "generators/UnifiedFireGenerator.cpp"

// Effect implementations
#include "effects/hue-rotation/HueRotationEffect.cpp"

// Renderer implementations
#include "renderers/EffectRenderer.cpp"

// Hardware implementations
#include "hardware/AdaptiveMic.cpp"
#include "hardware/BatteryMonitor.cpp"
// #include "hardware/IMUHelper.cpp"  // TODO: Enable when LSM6DS3 library is available
// #include "hardware/SerialConsole.cpp"  // TODO: Update for unified fire generator

// Configuration implementations
// #include "config/ConfigStorage.cpp"  // TODO: Clean up legacy fire params

// Test implementations (only when testing enabled)
#ifdef ENABLE_TESTING
#include "tests/GeneratorTestRunner.cpp"
#include "renderers/tests/EffectRendererTest.cpp"
#include "effects/hue-rotation/tests/HueRotationEffectTest.cpp"
#endif
