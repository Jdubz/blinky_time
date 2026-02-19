#include "Audio.h"
#include <Arduino.h>

Audio::Audio()
    : params_()
    , smoothedEnergy_(0.0f)
    , transientIntensity_(0.0f)
    , beatPulseIntensity_(0.0f)
    , prevPhase_(-1.0f)
{}

bool Audio::begin(const DeviceConfig& config) {
    width_ = config.matrix.width;
    height_ = config.matrix.height;
    numLeds_ = width_ * height_;
    layout_ = config.matrix.layoutType;
    orientation_ = config.matrix.orientation;

    smoothedEnergy_ = 0.0f;
    transientIntensity_ = 0.0f;
    beatPulseIntensity_ = 0.0f;
    prevPhase_ = -1.0f;

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

    // Detect beat event via phase wrap (high phase -> low phase)
    if (prevPhase_ > 0.8f && audio.phase < 0.2f &&
        audio.rhythmStrength >= params_.musicModeThreshold) {
        beatPulseIntensity_ = 1.0f;
    } else {
        beatPulseIntensity_ *= (1.0f - params_.beatPulseDecay);
        if (beatPulseIntensity_ < 0.01f) beatPulseIntensity_ = 0.0f;
    }
    prevPhase_ = audio.phase;

    // Clear and draw background
    drawBackground(matrix);

    // Draw in order from back to front:
    // 1. Beat pulse (blue band in center) - behind everything
    drawBeatPulse(matrix, audio.rhythmStrength);

    // 2. Phase row (blue) - only if music mode active, full height range
    drawPhaseRow(matrix, audio.phase, audio.rhythmStrength);

    // 3. Energy row (yellow)
    drawEnergyRow(matrix, smoothedEnergy_);

    // 4. Transient (green gradient from top) - on top so it's most visible
    drawTransientRows(matrix, transientIntensity_);
}

void Audio::reset() {
    smoothedEnergy_ = 0.0f;
    transientIntensity_ = 0.0f;
    beatPulseIntensity_ = 0.0f;
    prevPhase_ = -1.0f;
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

    // Use +0.5f for rounding to get proper row count (0.2 * 8 = 1.6 -> 2 rows, not 1)
    int transientRows = (int)(fraction * height_ + 0.5f);
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

    // Use +0.5f for rounding to match transient row calculation
    int transientRows = (int)(fraction * height_ + 0.5f);
    if (transientRows > height_) transientRows = height_;

    int usableHeight = height_ - transientRows;
    if (usableHeight < 1) return;

    // Y position: bottom of usable area (highest Y) to just below transient area
    // Use +0.5f for rounding to avoid off-by-1 from truncation
    int y = transientRows + (int)((1.0f - energy) * (usableHeight - 1) + 0.5f);
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

    // Phase uses full display height: phase 0 (on-beat) = bottom, phase ~1 = top
    int y = (int)((1.0f - phase) * (height_ - 1) + 0.5f);
    if (y < 0) y = 0;
    if (y >= height_) y = height_ - 1;

    // Brightness modulated by rhythm confidence
    // Map rhythmStrength from [threshold, 1] to [0.3, 1.0] for brightness
    float denominator = 1.0f - params_.musicModeThreshold;
    float confidenceScale;
    if (denominator <= 0.0f) {
        confidenceScale = 1.0f;
    } else {
        confidenceScale = (rhythmStrength - params_.musicModeThreshold) / denominator;
    }
    if (confidenceScale < 0.0f) confidenceScale = 0.0f;
    if (confidenceScale > 1.0f) confidenceScale = 1.0f;
    confidenceScale = 0.3f + 0.7f * confidenceScale;  // 30% to 100%

    uint8_t brightness = (uint8_t)(params_.phaseBrightness * confidenceScale);

    // Draw blue row
    setRow(matrix, y, 0, 0, brightness);
}

void Audio::drawBeatPulse(PixelMatrix& matrix, float rhythmStrength) {
    if (beatPulseIntensity_ < 0.01f) return;
    if (height_ < 3) return;  // Need at least 3 rows

    // Calculate band size centered on display
    float widthFrac = params_.beatPulseWidth;
    if (widthFrac < 0.0f) widthFrac = 0.0f;
    if (widthFrac > 1.0f) widthFrac = 1.0f;

    int bandRows = (int)(widthFrac * height_ + 0.5f);
    if (bandRows < 1) bandRows = 1;
    if (bandRows > height_) bandRows = height_;

    int centerY = height_ / 2;
    int startY = centerY - bandRows / 2;
    if (startY < 0) startY = 0;
    int endY = startY + bandRows;
    if (endY > height_) endY = height_;

    uint8_t maxBlue = (uint8_t)(params_.beatPulseBrightness * beatPulseIntensity_);

    // Draw band with soft edges: brightest in center, fading at edges
    for (int y = startY; y < endY; y++) {
        // Distance from center of band (0 = center, 1 = edge)
        float distFromCenter;
        if (bandRows <= 1) {
            distFromCenter = 0.0f;
        } else {
            float bandCenter = (startY + endY - 1) * 0.5f;
            float halfBand = (endY - startY) * 0.5f;
            distFromCenter = fabsf(y - bandCenter) / halfBand;
        }
        // Soft falloff: cos^2 for smooth edges
        float edgeFactor = 1.0f - distFromCenter * distFromCenter;
        if (edgeFactor < 0.0f) edgeFactor = 0.0f;

        uint8_t blue = (uint8_t)(maxBlue * edgeFactor);
        if (blue > 0) {
            // Additive blend with existing pixels
            for (int x = 0; x < width_; x++) {
                RGB existing = matrix.getPixel(x, y);
                matrix.setPixel(x, y,
                               existing.r,
                               existing.g,
                               (uint8_t)min(255, (int)existing.b + (int)blue));
            }
        }
    }
}

void Audio::setRow(PixelMatrix& matrix, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (y < 0 || y >= height_) return;

    for (int x = 0; x < width_; x++) {
        matrix.setPixel(x, y, r, g, b);
    }
}
