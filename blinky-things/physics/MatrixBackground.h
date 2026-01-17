#pragma once

#include "BackgroundModel.h"
#include "../math/SimplexNoise.h"
#include <Arduino.h>

/**
 * BackgroundStyle - What kind of visual style
 */
enum class BackgroundStyle {
    FIRE,       // Red/orange embers, brighter at bottom
    WATER,      // Blue/cyan waves, uniform
    LIGHTNING   // Purple/blue storm clouds, darker at top
};

/**
 * MatrixBackground - 2D noise background with height falloff
 *
 * Renders animated noise patterns with Y-based intensity:
 * - Fire: Brighter at bottom (heat source), darker at top
 * - Water: Uniform with wave patterns
 * - Lightning: Storm clouds with horizon glow at bottom
 */
class MatrixBackground : public BackgroundModel {
public:
    explicit MatrixBackground(BackgroundStyle style) : style_(style) {}

    void render(PixelMatrix& matrix, uint16_t width, uint16_t height,
               float noiseTime, const AudioControl& audio) override {
        // Noise scales for organic movement
        const float noiseScale = (style_ == BackgroundStyle::WATER) ? 0.12f : 0.15f;

        // Beat-reactive brightness modulation
        float beatBrightness = 1.0f;
        if (audio.hasRhythm()) {
            float phasePulse = audio.phaseToPulse();
            beatBrightness = 0.6f + 0.4f * phasePulse;
        }

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                float intensity = sampleNoise(x, y, width, height, noiseScale,
                                             noiseTime, beatBrightness);

                uint8_t r, g, b;
                applyColorPalette(intensity, (float)y / max(1, height - 1), r, g, b);

                matrix.setPixel(x, y, r, g, b);
            }
        }
    }

    float getIntensityAt(int x, int y, uint16_t width, uint16_t height) const override {
        // Height-based intensity for fire (brighter at bottom)
        float normalizedY = (float)y / max(1, height - 1);

        switch (style_) {
            case BackgroundStyle::FIRE:
                // Brighter at bottom (y = height-1), darker at top (y = 0)
                // normalizedY=0 at top, normalizedY=1 at bottom
                return 0.3f + normalizedY * 0.7f;  // Range 0.3-1.0

            case BackgroundStyle::WATER:
                // Uniform intensity
                return 1.0f;

            case BackgroundStyle::LIGHTNING:
                // Slightly brighter at bottom (horizon glow)
                return 0.7f + normalizedY * 0.3f;

            default:
                return 1.0f;
        }
    }

private:
    BackgroundStyle style_;

    float sampleNoise(int x, int y, uint16_t width, uint16_t height,
                     float scale, float time, float brightness) {
        float heightFalloff = getIntensityAt(x, y, width, height);

        // Sample 3D noise
        float nx = x * scale;
        float ny = y * scale;
        float noiseVal = SimplexNoise::noise3D_01(nx, ny, time);

        // Add second octave for more organic detail
        float noiseVal2 = SimplexNoise::noise3D_01(nx * 2.0f, ny * 2.0f, time * 1.3f);
        noiseVal = noiseVal * 0.7f + noiseVal2 * 0.3f;

        // Combine noise with height falloff and beat brightness
        float intensity = noiseVal * heightFalloff * brightness;

        // Very dark background - particles must be the star
        intensity *= 0.02f;

        return constrain(intensity, 0.0f, 1.0f);
    }

    void applyColorPalette(float intensity, float normalizedY,
                          uint8_t& r, uint8_t& g, uint8_t& b) {
        uint8_t level = (uint8_t)(intensity * 255.0f);

        switch (style_) {
            case BackgroundStyle::FIRE:
                // Fire gradient: deep red -> orange at bottom
                if (normalizedY > 0.6f) {
                    // Bottom 40%: orange-red embers
                    r = level;
                    g = (uint8_t)(level * 0.3f * (1.0f - normalizedY));
                    b = 0;
                } else {
                    // Upper 60%: deep red only
                    r = level;
                    g = (uint8_t)(level * 0.1f);
                    b = 0;
                }
                break;

            case BackgroundStyle::WATER:
                // Tropical sea: blue/green/cyan
                r = (uint8_t)(level * 0.1f);
                g = (uint8_t)(level * 0.5f);
                b = level;
                break;

            case BackgroundStyle::LIGHTNING:
                // Storm sky: purple clouds, orange at horizon
                if (normalizedY > 0.7f) {
                    // Horizon glow
                    r = (uint8_t)(level * 0.6f);
                    g = (uint8_t)(level * 0.2f);
                    b = (uint8_t)(level * 0.3f);
                } else if (normalizedY > 0.3f) {
                    // Purple storm clouds
                    r = (uint8_t)(level * 0.4f);
                    g = (uint8_t)(level * 0.1f);
                    b = (uint8_t)(level * 0.5f);
                } else {
                    // Dark blue night sky
                    r = (uint8_t)(level * 0.15f);
                    g = (uint8_t)(level * 0.1f);
                    b = (uint8_t)(level * 0.4f);
                }
                break;
        }
    }
};
