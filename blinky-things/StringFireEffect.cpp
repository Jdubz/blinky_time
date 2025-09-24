#include "StringFireEffect.h"
#include "SerialConsole.h"
#include "configs/DeviceConfig.h"
#include "Constants.h"
#include <new>
#include <math.h>

StringFireEffect::StringFireEffect(Adafruit_NeoPixel &strip, int length)
    : leds(strip), LENGTH(length), heat(nullptr) {
    restoreDefaults();
}

StringFireEffect::~StringFireEffect() {
    delete[] heat;
}

void StringFireEffect::begin() {
    if (heat) {
        delete[] heat;
        heat = nullptr;
    }

    // Allocate heat array with error checking
    heat = new(std::nothrow) float[LENGTH];
    if (!heat) {
        return; // Failed to allocate memory
    }

    // Initialize all positions to zero heat
    for (int i = 0; i < LENGTH; i++) {
        heat[i] = 0.0f;
    }
}

void StringFireEffect::update(float energy, float hit) {
    unsigned long now = millis();
    if (lastUpdateMs == 0) lastUpdateMs = now;

    float dt = (now - lastUpdateMs) * 0.001f;
    dt = constrain(dt, Constants::MIN_FRAME_TIME, Constants::MAX_FRAME_TIME);
    lastUpdateMs = now;

    if (!heat) return; // Safety check

    // Simple in-place fading like seeed-hat
    fadeInPlace();

    // Lateral heat propagation
    propagateLateral();

    // Spark injection
    injectSparks(energy);

    // Render to LEDs
    render();
}

void StringFireEffect::fadeInPlace() {
    // Simple in-place fading like seeed-hat example
    float fadeAmount = 0.03f; // Fixed fade rate for smooth decay

    for (int i = 0; i < LENGTH; i++) {
        if (getHeatRef(i) >= fadeAmount) {
            getHeatRef(i) -= fadeAmount;
        } else {
            getHeatRef(i) = 0.0f;
        }
    }
}

void StringFireEffect::propagateLateral() {
    // Create temporary array for new heat values
    float *newHeat = new(std::nothrow) float[LENGTH];
    if (!newHeat) return;

    // Copy current heat values
    for (int i = 0; i < LENGTH; i++) {
        newHeat[i] = getHeatValue(i);
    }

    // Gentle heat diffusion - each pixel shares heat with immediate neighbors
    for (int i = 0; i < LENGTH; i++) {
        float currentHeat = getHeatValue(i);

        if (currentHeat > 0.02f) { // Very low threshold for continuous oozing
            // Share heat with immediate neighbors only (distance 1-3)
            for (int distance = 1; distance <= 3; distance++) {
                // Much stronger propagation for oozing effect
                float diffusionRate = (distance == 1) ? 0.6f : (distance == 2) ? 0.4f : 0.2f;
                float spreadHeat = currentHeat * diffusionRate;

                // Spread to left neighbor
                if (i - distance >= 0) {
                    newHeat[i - distance] = max(newHeat[i - distance], spreadHeat);
                }

                // Spread to right neighbor
                if (i + distance < LENGTH) {
                    newHeat[i + distance] = max(newHeat[i + distance], spreadHeat);
                }
            }
        }
    }

    // Copy back the new heat values
    for (int i = 0; i < LENGTH; i++) {
        getHeatRef(i) = newHeat[i];
    }

    delete[] newHeat;
}

void StringFireEffect::injectSparks(float energy) {
    // Simple spark injection like seeed-hat example
    int numNewSparks = 2 + (int)(8 * energy); // 2-10 sparks based on audio

    for (int spark = 0; spark < numNewSparks; spark++) {
        int sparkPos = random(LENGTH);

        // Simple heat addition with audio boost
        float sparkIntensity = 0.3f + (0.7f * energy); // 0.3 to 1.0 based on audio

        // Add heat directly, allowing multiple sparks to accumulate
        getHeatRef(sparkPos) += sparkIntensity;

        // Clamp to maximum
        if (getHeatRef(sparkPos) > 1.0f) {
            getHeatRef(sparkPos) = 1.0f;
        }
    }
}

void StringFireEffect::render() {
    if (!heat) return;

    for (int i = 0; i < LENGTH; i++) {
        float h = constrain(getHeatValue(i), 0.0f, 1.0f);
        uint32_t color = heatToColorRGB(h);
        leds.setPixelColor(i, color);
    }
}

void StringFireEffect::show() {
    render();
    leds.show();
}

void StringFireEffect::restoreDefaults() {
    // Use config defaults instead of global defaults
    params.baseCooling = config.fireDefaults.baseCooling;
    params.sparkHeatMin = config.fireDefaults.sparkHeatMin;
    params.sparkHeatMax = config.fireDefaults.sparkHeatMax;
    params.sparkChance = config.fireDefaults.sparkChance;
    params.audioSparkBoost = config.fireDefaults.audioSparkBoost;
    params.audioHeatBoostMax = config.fireDefaults.audioHeatBoostMax;
    params.coolingAudioBias = config.fireDefaults.coolingAudioBias;
    params.transientHeatMax = config.fireDefaults.transientHeatMax;

    // String-specific defaults optimized for oozing
    params.sparkPositions = 3;      // More sparks for better brightness
    params.lateralDecay = 0.9f;     // Less decay for farther spread
    params.spreadDistance = 4;      // Moderate spread distance
}

uint32_t StringFireEffect::heatToColorRGB(float h) const {
    // Clamp heat to [0, 1]
    h = constrain(h, 0.0f, 1.0f);

    // Same color palette as original FireEffect
    uint8_t r, g, b;

    if (h < 0.25f) {
        // Black to dark red
        float t = h * 4.0f;
        r = (uint8_t)(t * 64);
        g = 0;
        b = 0;
    } else if (h < 0.5f) {
        // Dark red to red
        float t = (h - 0.25f) * 4.0f;
        r = (uint8_t)(64 + t * 191);
        g = 0;
        b = 0;
    } else if (h < 0.75f) {
        // Red to yellow
        float t = (h - 0.5f) * 4.0f;
        r = 255;
        g = (uint8_t)(t * 255);
        b = 0;
    } else {
        // Yellow to white
        float t = (h - 0.75f) * 4.0f;
        r = 255;
        g = 255;
        b = (uint8_t)(t * 255);
    }

    return leds.Color(r, g, b);
}