#include "Audio.h"
#include <Arduino.h>

Audio::Audio()
    : params_()
    , smoothedEnergy_(0.0f)
    , transientIntensity_(0.0f)
{}

bool Audio::begin(const DeviceConfig& config) {
    width_ = config.matrix.width;
    height_ = config.matrix.height;
    numLeds_ = width_ * height_;
    layout_ = config.matrix.layoutType;
    orientation_ = config.matrix.orientation;

    smoothedEnergy_ = 0.0f;
    transientIntensity_ = 0.0f;

    return true;
}

void Audio::generate(PixelMatrix& matrix, const AudioControl& audio) {
    // Update smoothed energy
    smoothedEnergy_ = smoothedEnergy_ * params_.levelSmoothing +
                      audio.energy * (1.0f - params_.levelSmoothing);

    // Update transient intensity (peak on pulse, then decay)
    if (audio.pulse > transientIntensity_) {
        transientIntensity_ = audio.pulse;
    } else {
        transientIntensity_ *= (1.0f - params_.transientDecayRate);
        if (transientIntensity_ < 0.01f) transientIntensity_ = 0.0f;
    }

    // Clear and draw background
    drawBackground(matrix);

    // Draw in order from back to front:
    // 1. Phase row (blue) - only if music mode active
    drawPhaseRow(matrix, audio.phase, audio.rhythmStrength);

    // 2. Energy row (yellow)
    drawEnergyRow(matrix, smoothedEnergy_);

    // 3. Transient (green gradient from top) - on top so it's most visible
    drawTransientRows(matrix, transientIntensity_);
}

void Audio::reset() {
    smoothedEnergy_ = 0.0f;
    transientIntensity_ = 0.0f;
}

void Audio::drawBackground(PixelMatrix& matrix) {
    uint8_t bg = params_.backgroundBrightness;
    for (int y = 0; y < height_; y++) {
        setRow(matrix, y, bg, bg, bg);
    }
}

void Audio::drawTransientRows(PixelMatrix& matrix, float intensity) {
    if (intensity < 0.01f) return;
    if (height_ < 1) return;  // Guard against zero height

    // Calculate how many rows for transient indicator
    // Clamp transientRowFraction to valid range
    float fraction = params_.transientRowFraction;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    int transientRows = (int)(fraction * height_);
    if (transientRows < 1) transientRows = 1;
    if (transientRows > height_) transientRows = height_;

    // Calculate green brightness based on intensity (clamp intensity to 0-1)
    float clampedIntensity = intensity;
    if (clampedIntensity > 1.0f) clampedIntensity = 1.0f;
    uint8_t maxGreen = (uint8_t)(params_.transientBrightness * clampedIntensity);

    // Draw gradient from top: brightest at top, fading down
    for (int i = 0; i < transientRows; i++) {
        int y = i;  // Start from top (y=0)

        // Gradient: full brightness at top, fading to 0 at bottom of transient region
        // transientRows is guaranteed >= 1, so no division by zero
        float gradientFactor = 1.0f - ((float)i / (float)transientRows);
        uint8_t green = (uint8_t)(maxGreen * gradientFactor);

        if (green > 0) {
            // Additive blend with existing pixels
            for (int x = 0; x < width_; x++) {
                RGB existing = matrix.getPixel(x, y);
                matrix.setPixel(x, y,
                               existing.r,
                               (uint8_t)min(255, (int)existing.g + (int)green),
                               existing.b);
            }
        }
    }
}

void Audio::drawEnergyRow(PixelMatrix& matrix, float energy) {
    if (height_ < 1) return;  // Guard against zero height

    // Clamp energy to 0-1
    if (energy < 0.0f) energy = 0.0f;
    if (energy > 1.0f) energy = 1.0f;

    // Map energy to Y position: high energy = near top (low Y), low energy = near bottom (high Y)
    // Reserve top rows for transient indicator
    // Clamp transientRowFraction to valid range
    float fraction = params_.transientRowFraction;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    int transientRows = (int)(fraction * height_);
    if (transientRows > height_) transientRows = height_;

    int usableHeight = height_ - transientRows;
    if (usableHeight < 1) return;

    // Y position: bottom of usable area (highest Y) to just below transient area
    int y = transientRows + (int)((1.0f - energy) * (usableHeight - 1));
    if (y < transientRows) y = transientRows;
    if (y >= height_) y = height_ - 1;

    // Draw yellow row
    uint8_t brightness = params_.levelBrightness;
    setRow(matrix, y, brightness, brightness, 0);  // Yellow: R=G, B=0
}

void Audio::drawPhaseRow(PixelMatrix& matrix, float phase, float rhythmStrength) {
    // Only show phase when music mode is active
    if (rhythmStrength < params_.musicModeThreshold) return;
    if (height_ < 1) return;  // Guard against zero height

    // Clamp phase to 0-1
    if (phase < 0.0f) phase = 0.0f;
    if (phase > 1.0f) phase = 1.0f;

    // Reserve top rows for transient indicator
    // Clamp transientRowFraction to valid range
    float fraction = params_.transientRowFraction;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    int transientRows = (int)(fraction * height_);
    if (transientRows > height_) transientRows = height_;

    int usableHeight = height_ - transientRows;
    if (usableHeight < 1) return;

    // Phase 0 (on-beat) = bottom, phase approaching 1 = top
    // Y position: bottom of usable area to top of usable area
    int y = transientRows + (int)((1.0f - phase) * (usableHeight - 1));
    if (y < transientRows) y = transientRows;
    if (y >= height_) y = height_ - 1;

    // Brightness modulated by rhythm confidence
    // Map rhythmStrength from [threshold, 1] to [0.3, 1.0] for brightness
    // Guard against division by zero when musicModeThreshold >= 1.0
    float confidenceScale;
    float denominator = 1.0f - params_.musicModeThreshold;
    if (denominator <= 0.0f) {
        // musicModeThreshold is 1.0 or higher - use full brightness
        confidenceScale = 1.0f;
    } else {
        confidenceScale = (rhythmStrength - params_.musicModeThreshold) / denominator;
    }

    // Clamp confidenceScale to valid range before applying
    if (confidenceScale < 0.0f) confidenceScale = 0.0f;
    if (confidenceScale > 1.0f) confidenceScale = 1.0f;

    confidenceScale = 0.3f + 0.7f * confidenceScale;  // 30% to 100%

    uint8_t brightness = (uint8_t)(params_.phaseBrightness * confidenceScale);

    // Draw blue row
    setRow(matrix, y, 0, 0, brightness);  // Blue: R=0, G=0, B=brightness
}

void Audio::setRow(PixelMatrix& matrix, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (y < 0 || y >= height_) return;

    for (int x = 0; x < width_; x++) {
        matrix.setPixel(x, y, r, g, b);
    }
}
