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
    explicit MatrixBackground(BackgroundStyle style) : style_(style), intensity_(0.02f) {}

    void setIntensity(float intensity) override {
        intensity_ = constrain(intensity, 0.0f, 1.0f);
    }

    void render(PixelMatrix& matrix, uint16_t width, uint16_t height,
               float noiseTime, const AudioControl& audio) override {
        // Noise scales for organic movement
        const float noiseScale = (style_ == BackgroundStyle::WATER) ? 0.12f : 0.15f;

        // Beat-reactive brightness modulation (blended by rhythmStrength)
        float phasePulse = audio.phaseToPulse();
        float musicBrightness = 0.6f + 0.4f * phasePulse;
        float organicBrightness = 1.0f;
        float beatBrightness = organicBrightness * (1.0f - audio.rhythmStrength) +
                              musicBrightness * audio.rhythmStrength;

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
    float intensity_;

    float sampleNoise(int x, int y, uint16_t width, uint16_t height,
                     float scale, float time, float brightness) {
        float heightFalloff = getIntensityAt(x, y, width, height);

        // Sample 3D noise for lava lamp effect
        // Increased scale from 0.08 to 0.35 for smaller blobs (1/5 size)
        float lavaScale = (style_ == BackgroundStyle::FIRE) ? 0.35f : scale;
        float nx = x * lavaScale;
        float ny = y * lavaScale;
        float noiseVal = SimplexNoise::noise3D_01(nx, ny, time * 0.03f);  // VERY slow drift like real lava lamp

        // Add second octave for more organic detail (less influence for lava lamp)
        float noiseVal2 = SimplexNoise::noise3D_01(nx * 2.0f, ny * 2.0f, time * 0.05f);
        noiseVal = noiseVal * 0.8f + noiseVal2 * 0.2f;  // Primary octave dominates

        // LAVA LAMP EFFECT: Apply threshold and contrast boost
        // Only show noise above threshold (0.4), then boost brightness
        const float threshold = 0.4f;
        if (noiseVal < threshold) {
            noiseVal = 0.0f;  // Dark areas stay completely dark
        } else {
            // Remap 0.4-1.0 range to 0.0-1.0 and apply power curve for contrast
            noiseVal = (noiseVal - threshold) / (1.0f - threshold);
            noiseVal = noiseVal * noiseVal;  // Square for higher contrast (bright blobs)
        }

        // Combine noise with height falloff and beat brightness
        float intensity = noiseVal * heightFalloff * brightness;

        // Apply configurable intensity multiplier
        intensity *= intensity_;

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
