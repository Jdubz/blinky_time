#include "Lightning.h"
#include "../types/ColorPalette.h"
#include <Arduino.h>

// Animation and behavior constants
namespace LightningConstants {
    constexpr uint32_t FRAME_INTERVAL_MS = 30;     // ~33 FPS (faster for snappy lightning)
    constexpr uint8_t BOLT_VISIBILITY_MIN = 50;    // Minimum intensity to render as visible
    constexpr float AUDIO_PRESENCE_THRESHOLD = 0.1f;  // Minimum audio energy to react
    constexpr int PROBABILITY_SCALE = 1000;        // Scale for random probability checks
    constexpr int PERCENT_SCALE = 100;             // Scale for percentage checks

    // Branching behavior
    constexpr int NUM_DIRECTIONS = 4;              // Up, right, down, left
    constexpr int BRANCH_DIRECTION_CHANCE = 50;    // 50% chance each direction
    constexpr int BRANCH_LENGTH_MIN = 2;           // Min branch length
    constexpr int BRANCH_LENGTH_MAX = 6;           // Max branch length (exclusive in random)
    constexpr int ZIGZAG_CHANCE = 30;              // 30% chance to change direction

    // Propagation chances
    constexpr int MATRIX_PROPAGATE_CHANCE = 20;    // 20% propagation to adjacent pixels
    constexpr int LINEAR_PROPAGATE_CHANCE = 30;    // 30% linear propagation
    constexpr int RANDOM_ARC_CHANCE = 15;          // 15% arc chance for random layout

    // Intensity propagation divisors
    constexpr uint8_t MATRIX_INTENSITY_DIVISOR = 3;
    constexpr uint8_t LINEAR_INTENSITY_DIVISOR = 2;
    constexpr uint8_t RANDOM_INTENSITY_DIVISOR = 4;

    // Fade constraints
    constexpr uint8_t MIN_FADE_RATE = 50;
    constexpr uint8_t MAX_FADE_RATE = 255;
    constexpr uint8_t FADE_DIVISOR = 10;
}

Lightning::Lightning()
    : intensity_(nullptr), tempIntensity_(nullptr), audioEnergy_(0.0f), audioHit_(0.0f) {
}

Lightning::~Lightning() {
    if (intensity_) {
        delete[] intensity_;
        intensity_ = nullptr;
    }
    if (tempIntensity_) {
        delete[] tempIntensity_;
        tempIntensity_ = nullptr;
    }
}

bool Lightning::begin(const DeviceConfig& config) {
    width_ = config.matrix.width;
    height_ = config.matrix.height;
    numLeds_ = width_ * height_;
    layout_ = config.matrix.layoutType;

    // Allocate intensity array
    if (intensity_) delete[] intensity_;
    intensity_ = new uint8_t[numLeds_];
    if (!intensity_) {
        Serial.println(F("ERROR: Failed to allocate intensity buffer"));
        return false;
    }
    memset(intensity_, 0, numLeds_);

    // Allocate temp buffer for bolt propagation (avoids heap fragmentation)
    if (tempIntensity_) delete[] tempIntensity_;
    tempIntensity_ = new uint8_t[numLeds_];
    if (!tempIntensity_) {
        Serial.println(F("ERROR: Failed to allocate temp intensity buffer"));
        delete[] intensity_;
        intensity_ = nullptr;
        return false;
    }

    // Reset defaults
    resetToDefaults();

    lastUpdateMs_ = millis();
    return true;
}

void Lightning::generate(PixelMatrix& matrix, const AudioControl& audio) {
    setAudioInput(audio.energy, audio.pulse);
    update();

    // Convert intensity values to colors and fill matrix
    for (int i = 0; i < numLeds_; i++) {
        uint32_t color = intensityToColor(intensity_[i]);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        int x, y;
        indexToCoords(i, x, y);
        matrix.setPixel(x, y, r, g, b);
    }
}

void Lightning::reset() {
    if (intensity_) {
        memset(intensity_, 0, numLeds_);
    }
    audioEnergy_ = 0.0f;
    audioHit_ = 0.0f;
}

void Lightning::update() {
    uint32_t currentMs = millis();
    if (currentMs - lastUpdateMs_ < LightningConstants::FRAME_INTERVAL_MS) return;
    lastUpdateMs_ = currentMs;

    // Choose algorithm based on layout
    switch (layout_) {
        case MATRIX_LAYOUT:
            updateMatrixLightning();
            break;
        case LINEAR_LAYOUT:
            updateLinearLightning();
            break;
        case RANDOM_LAYOUT:
            updateRandomLightning();
            break;
    }
}

void Lightning::setAudioInput(float energy, float hit) {
    audioEnergy_ = energy;
    audioHit_ = hit;
}

void Lightning::updateMatrixLightning() {
    // Generate bolts from audio
    generateBolts();

    // Propagate bolts with branching
    propagateBolts();

    // Apply fade
    applyFade();
}

void Lightning::updateLinearLightning() {
    // Similar to matrix but 1D bolt propagation
    generateBolts();
    propagateBolts();
    applyFade();
}

void Lightning::updateRandomLightning() {
    // Electric arcs between random points
    generateBolts();
    propagateBolts();
    applyFade();
}

void Lightning::generateBolts() {
    // Audio-reactive bolt generation
    float boltProb = params_.boltChance;
    // Transient impulse boosts bolt probability (audioHit_ is 0.0 or strength value)
    if (audioHit_ > 0.0f) {
        boltProb += params_.audioBoltBoost * audioHit_;  // Scale boost by transient strength
    }

    if (random(LightningConstants::PROBABILITY_SCALE) / (float)LightningConstants::PROBABILITY_SCALE < boltProb) {
        int boltPosition;
        uint8_t boltIntensity = random(params_.boltIntensityMin, params_.boltIntensityMax + 1);

        // Add audio boost to bolt intensity
        if (audioEnergy_ > LightningConstants::AUDIO_PRESENCE_THRESHOLD) {
            uint8_t audioBoost = (uint8_t)(audioEnergy_ * params_.audioIntensityBoostMax);
            boltIntensity = min(255, boltIntensity + audioBoost);
        }

        // Choose bolt position based on layout
        switch (layout_) {
            case MATRIX_LAYOUT:
                // Bolts can start anywhere
                boltPosition = random(numLeds_);
                intensity_[boltPosition] = boltIntensity;

                // Create branches
                if (random(LightningConstants::PERCENT_SCALE) < params_.branchChance) {
                    int x, y;
                    indexToCoords(boltPosition, x, y);

                    // Branch in random directions
                    for (int dir = 0; dir < LightningConstants::NUM_DIRECTIONS; dir++) {
                        if (random(LightningConstants::PERCENT_SCALE) < LightningConstants::BRANCH_DIRECTION_CHANCE) {
                            createBranch(boltPosition, dir, boltIntensity / 2);
                        }
                    }
                }
                break;

            case LINEAR_LAYOUT:
                // Bolts start from random position and propagate
                boltPosition = random(numLeds_);
                intensity_[boltPosition] = boltIntensity;
                break;

            case RANDOM_LAYOUT:
                // Random bolt position
                boltPosition = random(numLeds_);
                intensity_[boltPosition] = boltIntensity;
                break;
        }
    }
}

void Lightning::createBranch(int startIndex, int direction, uint8_t intensity) {
    int x, y;
    indexToCoords(startIndex, x, y);

    // Direction: 0=up, 1=right, 2=down, 3=left
    int dx = 0, dy = 0;
    switch (direction) {
        case 0: dy = -1; break; // up
        case 1: dx = 1; break;  // right
        case 2: dy = 1; break;  // down
        case 3: dx = -1; break; // left
    }

    // Create branch of random length
    int branchLength = random(LightningConstants::BRANCH_LENGTH_MIN, LightningConstants::BRANCH_LENGTH_MAX);
    for (int i = 1; i <= branchLength; i++) {
        int newX = x + dx * i;
        int newY = y + dy * i;

        if (newX >= 0 && newX < width_ && newY >= 0 && newY < height_) {
            int newIndex = coordsToIndex(newX, newY);
            if (newIndex >= 0) {
                intensity_[newIndex] = max(intensity_[newIndex], intensity / i);
            }
        } else {
            break; // Hit boundary
        }

        // Random chance to change direction (zigzag effect)
        if (random(LightningConstants::PERCENT_SCALE) < LightningConstants::ZIGZAG_CHANCE) {
            if (dx != 0) {
                dy = random(2) == 0 ? -1 : 1;
                dx = 0;
            } else {
                dx = random(2) == 0 ? -1 : 1;
                dy = 0;
            }
        }
    }
}

void Lightning::propagateBolts() {
    // Use pre-allocated tempIntensity_ to avoid heap fragmentation
    memcpy(tempIntensity_, intensity_, numLeds_);

    for (int i = 0; i < numLeds_; i++) {
        if (intensity_[i] > LightningConstants::BOLT_VISIBILITY_MIN) { // Only propagate strong bolts
            int x, y;
            indexToCoords(i, x, y);

            // Propagate based on layout
            switch (layout_) {
                case MATRIX_LAYOUT:
                    // Propagate to adjacent pixels
                    for (int dx = -1; dx <= 1; dx++) {
                        for (int dy = -1; dy <= 1; dy++) {
                            if (dx == 0 && dy == 0) continue;

                            int newX = x + dx;
                            int newY = y + dy;

                            if (newX >= 0 && newX < width_ && newY >= 0 && newY < height_) {
                                int newIndex = coordsToIndex(newX, newY);
                                if (newIndex >= 0 && random(LightningConstants::PERCENT_SCALE) < LightningConstants::MATRIX_PROPAGATE_CHANCE) {
                                    uint8_t propagatedIntensity = intensity_[i] / LightningConstants::MATRIX_INTENSITY_DIVISOR;
                                    tempIntensity_[newIndex] = max(tempIntensity_[newIndex], propagatedIntensity);
                                }
                            }
                        }
                    }
                    break;

                case LINEAR_LAYOUT:
                    // Propagate along the line
                    if (i > 0 && random(LightningConstants::PERCENT_SCALE) < LightningConstants::LINEAR_PROPAGATE_CHANCE) {
                        uint8_t propagatedIntensity = intensity_[i] / LightningConstants::LINEAR_INTENSITY_DIVISOR;
                        tempIntensity_[i - 1] = max(tempIntensity_[i - 1], propagatedIntensity);
                    }
                    if (i < numLeds_ - 1 && random(LightningConstants::PERCENT_SCALE) < LightningConstants::LINEAR_PROPAGATE_CHANCE) {
                        uint8_t propagatedIntensity = intensity_[i] / LightningConstants::LINEAR_INTENSITY_DIVISOR;
                        tempIntensity_[i + 1] = max(tempIntensity_[i + 1], propagatedIntensity);
                    }
                    break;

                case RANDOM_LAYOUT:
                    // Propagate to nearby positions (arc effect)
                    for (int j = max(0, i - 5); j <= min(numLeds_ - 1, i + 5); j++) {
                        if (j != i && random(LightningConstants::PERCENT_SCALE) < LightningConstants::RANDOM_ARC_CHANCE) {
                            uint8_t propagatedIntensity = intensity_[i] / LightningConstants::RANDOM_INTENSITY_DIVISOR;
                            tempIntensity_[j] = max(tempIntensity_[j], propagatedIntensity);
                        }
                    }
                    break;
            }
        }
    }

    memcpy(intensity_, tempIntensity_, numLeds_);
}

void Lightning::applyFade() {
    // Apply fade rate with audio influence
    uint8_t fadeRate = params_.baseFade;
    if (audioEnergy_ > LightningConstants::AUDIO_PRESENCE_THRESHOLD) {
        int8_t audioBias = (int8_t)(audioEnergy_ * params_.fadeAudioBias);
        fadeRate = constrain(fadeRate + audioBias, LightningConstants::MIN_FADE_RATE, LightningConstants::MAX_FADE_RATE);
    }

    // Fade all intensities
    for (int i = 0; i < numLeds_; i++) {
        if (intensity_[i] > 0) {
            intensity_[i] = max(0, intensity_[i] - (fadeRate / LightningConstants::FADE_DIVISOR));
        }
    }
}

uint32_t Lightning::intensityToColor(uint8_t intensity) {
    // Use shared palette system for consistent color handling
    return Palette::LIGHTNING.toColor(intensity);
}

void Lightning::setParams(const LightningParams& params) {
    params_ = params;
}

void Lightning::resetToDefaults() {
    params_ = LightningParams();
}

void Lightning::setBaseFade(uint8_t fade) {
    params_.baseFade = fade;
}

void Lightning::setBoltParams(uint8_t intensityMin, uint8_t intensityMax, float chance) {
    params_.boltIntensityMin = intensityMin;
    params_.boltIntensityMax = intensityMax;
    params_.boltChance = chance;
}

void Lightning::setAudioParams(float boltBoost, uint8_t intensityBoostMax, int8_t fadeBias) {
    params_.audioBoltBoost = boltBoost;
    params_.audioIntensityBoostMax = intensityBoostMax;
    params_.fadeAudioBias = fadeBias;
}

// Note: coordsToIndex and indexToCoords are now inherited from Generator base class
