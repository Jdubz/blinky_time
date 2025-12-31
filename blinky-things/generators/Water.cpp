#include "Water.h"
#include "../types/ColorPalette.h"
#include <Arduino.h>
#include "../inputs/SerialConsole.h"

// Animation and behavior constants
namespace WaterConstants {
    constexpr uint32_t FRAME_INTERVAL_MS = 50;     // ~20 FPS update rate
    constexpr float AUDIO_PRESENCE_THRESHOLD = 0.1f;  // Minimum audio energy to react
    constexpr int PROBABILITY_SCALE = 1000;        // Scale for random probability checks

    // Flow propagation rates (divisors for heat transfer)
    constexpr uint8_t MATRIX_FLOW_DIVISOR = 4;     // Downward flow rate for matrix
    constexpr uint8_t LINEAR_FLOW_DIVISOR = 6;     // Lateral flow rate for linear
    constexpr uint8_t RANDOM_FLOW_DIVISOR = 12;    // Ripple flow rate for random
    constexpr uint8_t BASE_FLOW_DIVISOR = 20;      // Base flow reduction rate
}

Water::Water()
    : depth_(nullptr), tempDepth_(nullptr), audioEnergy_(0.0f), audioHit_(0.0f) {
}

Water::~Water() {
    if (depth_) {
        delete[] depth_;
        depth_ = nullptr;
    }
    if (tempDepth_) {
        delete[] tempDepth_;
        tempDepth_ = nullptr;
    }
}

bool Water::begin(const DeviceConfig& config) {
    width_ = config.matrix.width;
    height_ = config.matrix.height;
    numLeds_ = width_ * height_;
    layout_ = config.matrix.layoutType;

    // Allocate depth array
    if (depth_) delete[] depth_;
    depth_ = new uint8_t[numLeds_];
    if (!depth_) {
        SerialConsole::logError(F("Failed to allocate depth buffer"));
        return false;
    }
    memset(depth_, 0, numLeds_);

    // Allocate temp buffer for flow propagation (avoids heap fragmentation)
    if (tempDepth_) delete[] tempDepth_;
    tempDepth_ = new uint8_t[numLeds_];
    if (!tempDepth_) {
        SerialConsole::logError(F("Failed to allocate temp depth buffer"));
        delete[] depth_;
        depth_ = nullptr;
        return false;
    }

    // Reset defaults
    resetToDefaults();

    lastUpdateMs_ = millis();
    return true;
}

void Water::generate(PixelMatrix& matrix, const AudioControl& audio) {
    setAudioInput(audio.energy, audio.pulse);
    update();

    // Convert depth values to colors and fill matrix
    for (int i = 0; i < numLeds_; i++) {
        uint32_t color = depthToColor(depth_[i]);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        int x, y;
        indexToCoords(i, x, y);
        matrix.setPixel(x, y, r, g, b);
    }
}

void Water::reset() {
    if (depth_) {
        memset(depth_, 0, numLeds_);
    }
    audioEnergy_ = 0.0f;
    audioHit_ = 0.0f;
}

void Water::update() {
    uint32_t currentMs = millis();
    if (currentMs - lastUpdateMs_ < WaterConstants::FRAME_INTERVAL_MS) return;
    lastUpdateMs_ = currentMs;

    // Choose algorithm based on layout
    switch (layout_) {
        case MATRIX_LAYOUT:
            updateMatrixWater();
            break;
        case LINEAR_LAYOUT:
            updateLinearWater();
            break;
        case RANDOM_LAYOUT:
            updateRandomWater();
            break;
    }
}

void Water::setAudioInput(float energy, float hit) {
    audioEnergy_ = energy;
    audioHit_ = hit;
}

void Water::updateMatrixWater() {
    // Generate waves from audio
    generateWaves();

    // Apply downward flow
    applyFlow();

    // Propagate flow patterns
    propagateFlow();
}

void Water::updateLinearWater() {
    // Similar to matrix but 1D wave propagation
    generateWaves();
    applyFlow();
    propagateFlow();
}

void Water::updateRandomWater() {
    // Ripple effects from wave centers
    generateWaves();
    applyFlow();
    propagateFlow();
}

void Water::generateWaves() {
    // Audio-reactive wave generation
    float waveProb = params_.waveChance;
    // Transient impulse boosts wave probability (audioHit_ is 0.0 or strength value)
    if (audioHit_ > 0.0f) {
        waveProb += params_.audioWaveBoost * audioHit_;  // Scale boost by transient strength
    }

    if (random(WaterConstants::PROBABILITY_SCALE) / (float)WaterConstants::PROBABILITY_SCALE < waveProb) {
        int wavePosition;
        uint8_t waveHeight = random(params_.waveHeightMin, params_.waveHeightMax + 1);

        // Add audio boost to wave height
        if (audioEnergy_ > WaterConstants::AUDIO_PRESENCE_THRESHOLD) {
            uint8_t audioBoost = (uint8_t)(audioEnergy_ * params_.audioFlowBoostMax);
            waveHeight = min(255, waveHeight + audioBoost);
        }

        // Choose wave position based on layout
        switch (layout_) {
            case MATRIX_LAYOUT:
                // Waves start from top
                wavePosition = random(width_);
                if (wavePosition < width_) {
                    depth_[wavePosition] = waveHeight;
                }
                break;

            case LINEAR_LAYOUT:
                // Waves start from beginning or end
                wavePosition = random(2) == 0 ? 0 : numLeds_ - 1;
                depth_[wavePosition] = waveHeight;
                break;

            case RANDOM_LAYOUT:
                // Random wave position
                wavePosition = random(numLeds_);
                depth_[wavePosition] = waveHeight;
                break;
        }
    }
}

void Water::propagateFlow() {
    // Use pre-allocated tempDepth_ to avoid heap fragmentation
    memcpy(tempDepth_, depth_, numLeds_);

    for (int i = 0; i < numLeds_; i++) {
        if (depth_[i] > 0) {
            int x, y;
            indexToCoords(i, x, y);

            // Flow based on layout
            switch (layout_) {
                case MATRIX_LAYOUT:
                    // Flow downward
                    if (y < height_ - 1) {
                        int belowIndex = coordsToIndex(x, y + 1);
                        uint8_t flowAmount = depth_[i] / WaterConstants::MATRIX_FLOW_DIVISOR;
                        tempDepth_[belowIndex] = min(255, tempDepth_[belowIndex] + flowAmount);
                        tempDepth_[i] = max(0, tempDepth_[i] - flowAmount);
                    }
                    break;

                case LINEAR_LAYOUT:
                    // Flow in both directions
                    if (i > 0) {
                        uint8_t flowAmount = depth_[i] / WaterConstants::LINEAR_FLOW_DIVISOR;
                        tempDepth_[i - 1] = min(255, tempDepth_[i - 1] + flowAmount);
                        tempDepth_[i] = max(0, tempDepth_[i] - flowAmount);
                    }
                    if (i < numLeds_ - 1) {
                        uint8_t flowAmount = depth_[i] / WaterConstants::LINEAR_FLOW_DIVISOR;
                        tempDepth_[i + 1] = min(255, tempDepth_[i + 1] + flowAmount);
                        tempDepth_[i] = max(0, tempDepth_[i] - flowAmount);
                    }
                    break;

                case RANDOM_LAYOUT:
                    // Ripple to nearby positions (simplified)
                    for (int j = max(0, i - 3); j <= min(numLeds_ - 1, i + 3); j++) {
                        if (j != i) {
                            uint8_t flowAmount = depth_[i] / WaterConstants::RANDOM_FLOW_DIVISOR;
                            tempDepth_[j] = min(255, tempDepth_[j] + flowAmount);
                            tempDepth_[i] = max(0, tempDepth_[i] - flowAmount);
                        }
                    }
                    break;
            }
        }
    }

    memcpy(depth_, tempDepth_, numLeds_);
}

void Water::applyFlow() {
    // Apply base flow rate with audio influence
    uint8_t flowRate = params_.baseFlow;
    if (audioEnergy_ > WaterConstants::AUDIO_PRESENCE_THRESHOLD) {
        int8_t audioBias = (int8_t)(audioEnergy_ * params_.flowAudioBias);
        flowRate = constrain(flowRate + audioBias, 0, 255);
    }

    // Reduce all depths by flow rate
    for (int i = 0; i < numLeds_; i++) {
        if (depth_[i] > 0) {
            depth_[i] = max(0, depth_[i] - (flowRate / WaterConstants::BASE_FLOW_DIVISOR));
        }
    }
}

uint32_t Water::depthToColor(uint8_t depth) {
    // Use shared palette system for consistent color handling
    return Palette::WATER.toColor(depth);
}

void Water::setParams(const WaterParams& params) {
    params_ = params;
}

void Water::resetToDefaults() {
    params_ = WaterParams();
}

void Water::setBaseFlow(uint8_t flow) {
    params_.baseFlow = flow;
}

void Water::setWaveParams(uint8_t heightMin, uint8_t heightMax, float chance) {
    params_.waveHeightMin = heightMin;
    params_.waveHeightMax = heightMax;
    params_.waveChance = chance;
}

void Water::setAudioParams(float waveBoost, uint8_t flowBoostMax, int8_t flowBias) {
    params_.audioWaveBoost = waveBoost;
    params_.audioFlowBoostMax = flowBoostMax;
    params_.flowAudioBias = flowBias;
}

// Note: coordsToIndex and indexToCoords are now inherited from Generator base class
