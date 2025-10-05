#include "StringFireGenerator.h"
#include <Arduino.h>

StringFireGenerator::StringFireGenerator(int len)
    : length(len), heat(nullptr), lastUpdateMs(0) {

    // Allocate heat buffer
    heat = (float*)malloc(sizeof(float) * length);
    if (!heat) {
        Serial.println(F("StringFireGenerator: Failed to allocate heat buffer"));
        return;
    }

    reset();
}

StringFireGenerator::~StringFireGenerator() {
    if (heat) {
        free(heat);
        heat = nullptr;
    }
}

void StringFireGenerator::reset() {
    if (!heat) return;

    // Clear heat array
    for (int i = 0; i < length; i++) {
        heat[i] = 0.0f;
    }

    lastUpdateMs = 0;
}

void StringFireGenerator::generate(EffectMatrix& matrix, float energy, float hit) {
    if (!heat) return;

    // Frame timing
    unsigned long now = millis();
    if (lastUpdateMs == 0) lastUpdateMs = now;
    float dt = (now - lastUpdateMs) * 0.001f;
    dt = constrain(dt, 0.001f, 0.1f); // 1ms to 100ms
    lastUpdateMs = now;

    // Update fire simulation
    coolCells();
    propagateLateral();
    injectSparks(energy);

    // For string fires, we typically map to a 1D matrix (width = length, height = 1)
    // or fill the entire matrix with the string pattern
    if (matrix.getHeight() == 1) {
        // 1D string mapping
        int matrixWidth = matrix.getWidth();
        for (int x = 0; x < matrixWidth; x++) {
            int stringIndex = (x * length) / matrixWidth; // Map matrix to string
            float h = (stringIndex < length) ? heat[stringIndex] : 0.0f;
            h = constrain(h, 0.0f, 1.0f);
            uint32_t color = heatToColorRGB(h);
            matrix.setPixel(x, 0, color);
        }
    } else {
        // 2D matrix - fill entire matrix with string pattern
        int totalPixels = matrix.getWidth() * matrix.getHeight();
        for (int y = 0; y < matrix.getHeight(); y++) {
            for (int x = 0; x < matrix.getWidth(); x++) {
                int pixelIndex = y * matrix.getWidth() + x;
                int stringIndex = (pixelIndex * length) / totalPixels;
                float h = (stringIndex < length) ? heat[stringIndex] : 0.0f;
                h = constrain(h, 0.0f, 1.0f);
                uint32_t color = heatToColorRGB(h);
                matrix.setPixel(x, y, color);
            }
        }
    }
}

float StringFireGenerator::getHeat(int index) const {
    if (index < 0 || index >= length || !heat) {
        return 0.0f;
    }
    return heat[index];
}

void StringFireGenerator::coolCells() {
    // Simple in-place fading
    float fadeAmount = 0.03f; // Fixed fade rate for smooth decay

    for (int i = 0; i < length; i++) {
        if (heat[i] >= fadeAmount) {
            heat[i] -= fadeAmount;
        } else {
            heat[i] = 0.0f;
        }
    }
}

void StringFireGenerator::propagateLateral() {
    // Create temporary array for new heat values
    float *newHeat = (float*)malloc(sizeof(float) * length);
    if (!newHeat) return;

    // Copy current heat values
    for (int i = 0; i < length; i++) {
        newHeat[i] = heat[i];
    }

    // Gentle heat diffusion - each pixel shares heat with neighbors
    for (int i = 0; i < length; i++) {
        float currentHeat = heat[i];

        if (currentHeat > 0.02f) { // Low threshold for continuous oozing
            // Share heat with immediate neighbors (distance 1-3)
            for (int distance = 1; distance <= params.sparkSpreadRange; distance++) {
                // Stronger propagation for oozing effect
                float diffusionRate = (distance == 1) ? 0.6f :
                                     (distance == 2) ? 0.4f : 0.2f;
                float spreadHeat = currentHeat * diffusionRate;

                // Spread to left neighbor
                if (i - distance >= 0) {
                    newHeat[i - distance] = max(newHeat[i - distance], spreadHeat);
                }

                // Spread to right neighbor
                if (i + distance < length) {
                    newHeat[i + distance] = max(newHeat[i + distance], spreadHeat);
                }
            }
        }
    }

    // Copy back the new heat values
    for (int i = 0; i < length; i++) {
        heat[i] = newHeat[i];
    }

    free(newHeat);
}

void StringFireGenerator::injectSparks(float energy) {
    // Simple spark injection based on energy
    int numNewSparks = 2 + (int)(8 * energy); // 2-10 sparks based on audio

    for (int spark = 0; spark < numNewSparks; spark++) {
        int sparkPos = random(length);

        // Simple heat addition with audio boost
        float sparkIntensity = 0.3f + (0.7f * energy); // 0.3 to 1.0 based on audio

        // Add heat directly, allowing multiple sparks to accumulate
        heat[sparkPos] += sparkIntensity;

        // Clamp to maximum
        if (heat[sparkPos] > 1.0f) {
            heat[sparkPos] = 1.0f;
        }
    }
}

uint32_t StringFireGenerator::heatToColorRGB(float h) const {
    // Clamp heat to [0, 1]
    h = constrain(h, 0.0f, 1.0f);

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

    // Return RGB packed color
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
