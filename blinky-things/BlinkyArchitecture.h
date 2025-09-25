#pragma once

/**
 * BlinkyArchitecture.h - Main include for the Generator-Effect-Renderer architecture
 * 
 * This file provides a single include for the Arduino IDE main sketch to access
 * all the visual architecture components while maintaining clean folder structure
 * for development and testing.
 * 
 * Usage in main sketch:
 * #include "BlinkyArchitecture.h"
 * 
 * Architecture Overview:
 * Generator -> Effects -> Renderer -> Hardware
 */

// Configuration and utilities
#include "config/Constants.h"
#include "config/Globals.h"
#include "config/TotemDefaults.h"
#include "config/ConfigStorage.h"

// Core interfaces
#include "core/Generator.h"
#include "core/Effect.h"
#include "core/EffectMatrix.h"

// Generators
#include "generators/legacy-fire/FireGenerator.h"  // Legacy fire generator
#include "generators/matrix-fire/MatrixFireGenerator.h"
#include "generators/string-fire/StringFireGenerator.h"

// Effects
#include "effects/hue-rotation/HueRotationEffect.h"

// Renderers
#include "renderers/EffectRenderer.h"

// Hardware components (temporarily commented out until updated for new architecture)
#include "hardware/AdaptiveMic.h"
// #include "hardware/SerialConsole.h"  // TODO: Update for new Generator architecture
// #include "hardware/BatteryMonitor.h"  // TODO: Update for new Generator architecture  
// #include "hardware/IMUHelper.h"       // TODO: Update for new Generator architecture

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