#pragma once

#include "BackgroundModel.h"
#include "MatrixBackground.h"  // For BackgroundStyle enum
#include "../math/SimplexNoise.h"
#include <Arduino.h>

/**
 * LinearBackground - 1D noise background without height falloff
 *
 * For linear layouts (hat brim), renders position-based noise
 * without the vertical gradient used in matrix layouts.
 * Creates uniform glow that varies along the string.
 */
class LinearBackground : public BackgroundModel {
public:
    explicit LinearBackground(BackgroundStyle style) : style_(style) {}

    void render(PixelMatrix& matrix, uint16_t width, uint16_t height,
               float noiseTime, const AudioControl& audio) override {
        // Beat-reactive brightness modulation
        float beatBrightness = 1.0f;
        if (audio.hasRhythm()) {
            float phasePulse = audio.phaseToPulse();
            beatBrightness = 0.6f + 0.4f * phasePulse;
        }

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                float intensity = sampleNoise(x, y, width, noiseTime, beatBrightness);

                uint8_t r, g, b;
                applyColorPalette(intensity, r, g, b);

                matrix.setPixel(x, y, r, g, b);
            }
        }
    }

    float getIntensityAt(int x, int y, uint16_t width, uint16_t height) const override {
        // Uniform intensity for linear layouts - no height falloff
        (void)x;
        (void)y;
        (void)width;
        (void)height;
        return 1.0f;
    }

private:
    BackgroundStyle style_;

    float sampleNoise(int x, int y, uint16_t width, float time, float brightness) {
        // Position-based noise without height dependency
        float nx = x * 0.1f;
        float ny = y * 0.1f;

        // Sample 3D noise
        float noiseVal = SimplexNoise::noise3D_01(nx, ny, time);

        // Add second octave for more organic detail
        float noiseVal2 = SimplexNoise::noise3D_01(nx * 2.5f, ny * 2.5f, time * 1.3f);
        noiseVal = noiseVal * 0.6f + noiseVal2 * 0.4f;

        // Apply beat brightness
        float intensity = noiseVal * brightness;

        // Very dark background - particles must be the star
        intensity *= 0.025f;

        return constrain(intensity, 0.0f, 1.0f);
    }

    void applyColorPalette(float intensity, uint8_t& r, uint8_t& g, uint8_t& b) {
        uint8_t level = (uint8_t)(intensity * 255.0f);

        switch (style_) {
            case BackgroundStyle::FIRE:
                // Warm ember glow - red/orange
                r = level;
                g = (uint8_t)(level * 0.25f);
                b = 0;
                break;

            case BackgroundStyle::WATER:
                // Cool blue glow
                r = (uint8_t)(level * 0.1f);
                g = (uint8_t)(level * 0.4f);
                b = level;
                break;

            case BackgroundStyle::LIGHTNING:
                // Purple/blue storm ambience
                r = (uint8_t)(level * 0.3f);
                g = (uint8_t)(level * 0.1f);
                b = (uint8_t)(level * 0.5f);
                break;
        }
    }
};
