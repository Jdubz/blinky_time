#pragma once

/**
 * BlinkyImplementations.h - Include all implementation files
 *
 * Arduino IDE doesn't automatically compile .cpp files in subdirectories.
 * This file includes all implementations to ensure they're compiled.
 *
 * ARCHITECTURE STATUS:
 * ✅ Core: Generator→Effect(optional)→Render pipeline operational
 * ✅ Fire: Layout-aware fire with orientation support
 * ✅ Water: Flowing water effects (float hit interface)
 * ✅ Lightning: Electric bolt effects (float hit interface)
 * ✅ Effects: HueRotation, NoOp
 * ✅ Hardware: AdaptiveMic, BatteryMonitor, SerialConsole
 * ✅ ConfigStorage: Flash persistence (nRF52)
 * ✅ SettingsRegistry: Unified settings management
 * ⚠️  IMUHelper: Disabled (LSM6DS3 library dependency)
 */

// Core data types
#include "types/BlinkyAssert.cpp"
#include "types/PixelMatrix.cpp"

// Math implementations
#include "math/SimplexNoise.cpp"

// Physics implementations
#include "physics/PhysicsContext.cpp"

// Generator implementations
#include "generators/Fire.cpp"
#include "generators/HeatFire.cpp"
#include "generators/Water.cpp"
#include "generators/Lightning.cpp"
#include "generators/Audio.cpp"

// Effect implementations
#include "effects/HueRotationEffect.cpp"
// Note: NoOpEffect is header-only

// Render implementations
#include "render/EffectRenderer.cpp"
#include "render/RenderPipeline.cpp"

// HAL implementations
#include "hal/hardware/NeoPixelLedStrip.cpp"  // LED strip wrapper
// NOTE: Nrf52PdmMic.cpp is in sketch root for separate compilation (avoids pinDefinitions.h conflict on nRF52 mbed core)
// On ESP32-S3 the I2S library has no such conflict so Esp32PdmMic.cpp compiles normally here.
#include "hal/PlatformDetect.h"
#ifdef BLINKY_PLATFORM_ESP32S3
  #include "hal/hardware/Esp32PdmMic.cpp"
#endif

// Input implementations
#include "inputs/AdaptiveMic.cpp"
#include "inputs/BatteryMonitor.cpp"
#include "inputs/IMUHelper.cpp"     // Stub mode without LSM6DS3; define IMU_ENABLED to activate
#include "inputs/SerialConsole.cpp"

// Configuration implementations
#include "config/ConfigStorage.cpp"     // Persistent settings storage
#include "config/DeviceConfigLoader.cpp"  // v28: Runtime device config loading
#include "config/SettingsRegistry.cpp"  // Settings abstraction layer

// Audio processing implementations
#include "audio/SharedSpectralAnalysis.cpp"
#include "audio/AudioController.cpp"

// Test implementations (only when testing enabled)
#ifdef ENABLE_TESTING
#include "tests/GeneratorTestRunner.cpp"
#include "render/tests/EffectRendererTest.cpp"
#include "effects/tests/GeneralEffectTests.cpp"
#include "effects/tests/HueRotationEffectTest.cpp"
#endif
