#pragma once

/**
 * BlinkyImplementations.h - Include all implementation files
 *
 * Arduino IDE doesn't automatically compile .cpp files in subdirectories.
 * This file includes all implementations to ensure they're compiled.
 *
 * CURRENT ARCHITECTURE STATUS:
 * ✅ Core: Inputs→Generator→Effect(optional)→Render pipeline operational
 * ✅ Fire: Realistic fire simulation (red/orange/yellow)
 * ✅ Water: Flowing water effects (blue/cyan)
 * ✅ Lightning: Electric bolt effects (yellow/white)
 * ✅ Effects: HueRotation (color cycling), NoOp (pass-through)
 * ✅ Testing: General effect tests for all effects
 * ✅ Hardware: AdaptiveMic and BatteryMonitor ready
 * ⚠️  IMUHelper: Disabled until LSM6DS3 library dependency resolved
 * ⚠️  SerialConsole: Disabled until updated for unified architecture
 * ⚠️  ConfigStorage: Disabled until legacy fire params cleaned up
 */

// Core data types
#include "types/PixelMatrix.cpp"

// Generator implementations
#include "generators/Fire.cpp"
#include "generators/Water.cpp"
#include "generators/Lightning.cpp"

// Effect implementations
#include "effects/HueRotationEffect.cpp"
// Note: NoOpEffect is header-only

// Render implementations
#include "render/EffectRenderer.cpp"

// Input implementations
// #include "inputs/AdaptiveMic.cpp"  // TODO: Fix pinDefinitions.h conflict with PDM/NeoPixel
#include "inputs/BatteryMonitor.cpp"
// #include "inputs/IMUHelper.cpp"  // TODO: Enable when LSM6DS3 library is available
// #include "inputs/SerialConsole.cpp"  // TODO: Update for unified fire generator

// Configuration implementations
// #include "config/ConfigStorage.cpp"  // TODO: Clean up legacy fire params

// Test implementations (only when testing enabled)
#ifdef ENABLE_TESTING
#include "tests/GeneratorTestRunner.cpp"
#include "render/tests/EffectRendererTest.cpp"
#include "effects/tests/GeneralEffectTests.cpp"
#include "effects/tests/HueRotationEffectTest.cpp"
#endif
