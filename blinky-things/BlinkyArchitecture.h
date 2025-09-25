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

// Core interfaces
#include "Generator.h"
#include "Effect.h"
#include "EffectMatrix.h"

// Generators
#include "generators/fire/FireGenerator.h"

// Effects
#include "effects/hue-rotation/HueRotationEffect.h"

// Renderers
#include "renderers/EffectRenderer.h"

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