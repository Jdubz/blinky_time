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
    explicit LinearBackground(BackgroundStyle style) : style_(style), intensity_(0.15f) {}

    void setIntensity(float intensity) override {
        intensity_ = constrain(intensity, 0.0f, 1.0f);
    }

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
    float intensity_;

    float sampleNoise(int x, int y, uint16_t width, float time, float brightness) {
        // For linear layouts, create "lava lamp" flow with visible blobs
        // Mostly dark with bright blobs drifting around
        (void)y;  // Unused for 1D

        // Normalize position to 0-1 range
        float pos = (float)x / max(1, width - 1);

        // Map to circle for seamless wrapping (hat brim is circular)
        float angle = pos * 2.0f * PI;
        float radius = 4.0f;  // Controls blob size (larger = smaller blobs)
        float nx = cos(angle) * radius;
        float ny = sin(angle) * radius;

        // Flow: blobs drift around the circle over time
        float flowOffset = time * 0.2f;
        nx += cos(flowOffset * 0.7f) * 1.0f;
        ny += sin(flowOffset) * 1.0f;

        // Sample noise with slow time evolution
        float noiseVal = SimplexNoise::noise3D_01(nx, ny, time * 0.1f);

        // Threshold: only show the brightest peaks as blobs
        // Noise is 0-1, subtract threshold to make most of it dark
        float threshold = 0.55f;  // Only top 45% of noise shows
        noiseVal = (noiseVal - threshold) / (1.0f - threshold);
        noiseVal = max(0.0f, noiseVal);  // Clamp negatives to 0

        // Smooth the blob edges
        noiseVal = noiseVal * noiseVal;

        // Apply beat brightness and intensity
        float intensity = noiseVal * brightness * intensity_;

        return constrain(intensity, 0.0f, 1.0f);
    }

    void applyColorPalette(float intensity, uint8_t& r, uint8_t& g, uint8_t& b) {
        uint8_t level = (uint8_t)(intensity * 255.0f);

        switch (style_) {
            case BackgroundStyle::FIRE:
                // Pure red ember glow
                r = level;
                g = 0;
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
