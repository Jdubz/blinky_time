#pragma once

/**
 * BlinkyImplementations.h - Include all implementation files
 *
 * Arduino IDE doesn't automatically compile .cpp files in subdirectories.
 * This file includes all implementations to ensure they're compiled.
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
