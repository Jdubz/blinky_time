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
#include "render/RenderPipeline.cpp"

// HAL implementations
#include "hal/hardware/NeoPixelLedStrip.cpp"  // LED strip wrapper
// NOTE: Nrf52PdmMic.cpp is in sketch root for separate compilation (avoids pinDefinitions.h conflict)

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
#include "audio/EnsembleFusion.cpp"
#include "audio/EnsembleDetector.cpp"
#include "audio/detectors/DrummerDetector.cpp"
#include "audio/detectors/SpectralFluxDetector.cpp"
#include "audio/detectors/HFCDetector.cpp"
#include "audio/detectors/BassBandDetector.cpp"
#include "audio/detectors/ComplexDomainDetector.cpp"
#include "audio/detectors/MelFluxDetector.cpp"
#include "audio/AudioController.cpp"

// Test implementations (only when testing enabled)
#ifdef ENABLE_TESTING
#include "tests/GeneratorTestRunner.cpp"
#include "render/tests/EffectRendererTest.cpp"
#include "effects/tests/GeneralEffectTests.cpp"
#include "effects/tests/HueRotationEffectTest.cpp"
#endif
