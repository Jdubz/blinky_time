#pragma once

#include "../generators/Generator.h"
#include "../generators/Fire.h"
#include "../generators/Water.h"
#include "../generators/Lightning.h"
#include "../generators/Audio.h"
#include "../effects/Effect.h"
#include "../effects/NoOpEffect.h"
#include "../effects/HueRotationEffect.h"
#include "../types/PixelMatrix.h"
#include "../audio/AudioControl.h"
#include "../devices/DeviceConfig.h"
#include "EffectRenderer.h"

/**
 * EffectType - Enumeration of available post-processing effects
 */
enum class EffectType {
    NONE,           // No effect (pass-through)
    HUE_ROTATION    // Hue rotation/color cycling
};

/**
 * RenderPipeline - Manages the Generator -> Effect -> Renderer flow
 *
 * Owns all generators and effects, handling switching and configuration.
 * Enforces:
 * - Exactly one active generator at all times
 * - Zero or one active effect (none is valid)
 *
 * Usage:
 *   RenderPipeline pipeline;
 *   pipeline.begin(config, leds, mapper);
 *
 *   // Switch generators
 *   pipeline.setGenerator(GeneratorType::WATER);
 *
 *   // Enable/disable effects
 *   pipeline.setEffect(EffectType::HUE_ROTATION);
 *   pipeline.setEffect(EffectType::NONE);  // Disable effect
 *
 *   // Render frame
 *   pipeline.render(audio);
 *   leds.show();
 */
class RenderPipeline {
public:
    RenderPipeline();
    ~RenderPipeline();

    // Initialization
    bool begin(const DeviceConfig& config, ILedStrip& leds, LEDMapper& mapper);

    // Frame rendering
    void render(const AudioControl& audio);

    // Generator control
    bool setGenerator(GeneratorType type);
    GeneratorType getGeneratorType() const;
    Generator* getCurrentGenerator() { return currentGenerator_; }
    const char* getGeneratorName() const;

    // Effect control
    bool setEffect(EffectType type);
    EffectType getEffectType() const;
    const char* getEffectName() const;
    bool hasEffect() const { return effectType_ != EffectType::NONE; }

    // Type-safe parameter access for each generator
    FireParams* getFireParams();
    WaterParams* getWaterParams();
    LightningParams* getLightningParams();
    AudioParams* getAudioVisParams();

    // Apply parameters after modification
    void applyFireParams();
    void applyWaterParams();
    void applyLightningParams();
    void applyAudioVisParams();

    // Effect parameter access
    HueRotationEffect* getHueRotationEffect() { return hueRotation_; }

    // Direct generator access (for advanced use)
    Fire* getFireGenerator() { return fire_; }
    Water* getWaterGenerator() { return water_; }
    Lightning* getLightningGenerator() { return lightning_; }
    Audio* getAudioVisGenerator() { return audioVis_; }

    // Utility
    PixelMatrix* getPixelMatrix() { return pixelMatrix_; }
    bool isValid() const { return initialized_; }

    // List available options (returns count)
    static constexpr int NUM_GENERATORS = 4;
    static constexpr int NUM_EFFECTS = 2;  // Including NONE
    static const char* getGeneratorNameByIndex(int index);
    static const char* getEffectNameByIndex(int index);
    static GeneratorType getGeneratorTypeByIndex(int index);
    static EffectType getEffectTypeByIndex(int index);

private:
    // Generators (all owned, one active)
    Fire* fire_;
    Water* water_;
    Lightning* lightning_;
    Audio* audioVis_;
    Generator* currentGenerator_;
    GeneratorType generatorType_;

    // Effects (all owned, zero or one active)
    NoOpEffect* noOp_;
    HueRotationEffect* hueRotation_;
    Effect* currentEffect_;
    EffectType effectType_;

    // Rendering
    PixelMatrix* pixelMatrix_;
    EffectRenderer* renderer_;

    // State
    bool initialized_;
    int width_, height_;

    // Prevent copying
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
};
