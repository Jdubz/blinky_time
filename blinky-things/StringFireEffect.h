#pragma once

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "TotemDefaults.h"
#include "Globals.h"

/*
 * StringFireEffect - Fire simulation mode for linear LED arrangements
 *
 * This is a FIRE EFFECT MODE, not a device. Used by devices like:
 * - Hat (89 LEDs in circular string)
 * - LED strips (any linear arrangement)
 * - Single-row installations
 *
 * Key differences from MatrixFireEffect:
 * - Heat dissipates sideways (laterally) instead of upward
 * - Multiple sparks use maximum heat value, not additive combination
 * - Optimized for linear LED arrangements where "up" doesn't make sense
 * - Sparks can originate from multiple positions along the string
 *
 * Usage in device configs:
 * 1. Set config.matrix.fireType = STRING_FIRE
 * 2. Configure matrix as 1D: width = LED_COUNT, height = 1
 * 3. The effect will automatically handle lateral heat propagation
 */

struct StringFireParams {
    // Same base parameters as FireParams but adapted for string behavior
    uint8_t baseCooling         = Defaults::BaseCooling;
    uint8_t sparkHeatMin        = Defaults::SparkHeatMin;
    uint8_t sparkHeatMax        = Defaults::SparkHeatMax;
    float   sparkChance         = Defaults::SparkChance;
    float   audioSparkBoost     = Defaults::AudioSparkBoost;
    uint8_t audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
    int8_t  coolingAudioBias    = Defaults::CoolingAudioBias;
    uint8_t transientHeatMax    = Defaults::TransientHeatMax;

    // String-specific parameters
    uint8_t sparkPositions      = 3;     // Fewer spark positions for calmer effect
    float   lateralDecay        = 0.92f; // Less decay for farther heat spread
    uint8_t spreadDistance      = 12;    // Much farther heat spread for smoother effect
};

class StringFireEffect {
public:
    // Constructors: support both pointer & reference forms
    StringFireEffect(Adafruit_NeoPixel &strip, int length);
    StringFireEffect(Adafruit_NeoPixel *strip, int length)
        : StringFireEffect(*strip, length) {}
    ~StringFireEffect();

    void begin();
    void update(float energy, float hit);
    void show();
    void render();
    void restoreDefaults();

    // Heat access for visualization
    float getHeat(int position) const {
        if (position < 0 || position >= LENGTH || !heat) return 0.0f;
        return heat[position];
    }

    // Make params public so SerialConsole can read/write them
    StringFireParams params;

private:
    Adafruit_NeoPixel &leds;
    int LENGTH;
    unsigned long lastUpdateMs = 0;

    // Heat array for linear LED string
    float *heat; // flat allocation: LENGTH elements

    void fadeInPlace();
    void propagateLateral();
    void injectSparks(float energy);

    // Color palette
    uint32_t heatToColorRGB(float h) const;

    // Helper functions
    inline float& getHeatRef(int pos) { return heat[pos]; }
    inline const float& getHeatValue(int pos) const { return heat[pos]; }
};