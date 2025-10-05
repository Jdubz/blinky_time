#include "Water.h"
#include <Arduino.h>

Water::Water()
    : depth_(nullptr), audioEnergy_(0.0f), audioHit_(false),
      wavePositions_(nullptr), numActiveWaves_(0) {
}

Water::~Water() {
    if (depth_) {
        delete[] depth_;
        depth_ = nullptr;
    }
    if (wavePositions_) {
        delete[] wavePositions_;
        wavePositions_ = nullptr;
    }
}

bool Water::begin(const DeviceConfig& config) {
    width_ = config.layout.width;
    height_ = config.layout.height;
    numLeds_ = width_ * height_;
    layout_ = config.layout.type;

    // Allocate depth array
    if (depth_) delete[] depth_;
    depth_ = new uint8_t[numLeds_];
    memset(depth_, 0, numLeds_);

    // Allocate wave positions for random layout
    if (wavePositions_) delete[] wavePositions_;
    wavePositions_ = new uint8_t[params_.maxWavePositions];
    memset(wavePositions_, 0, params_.maxWavePositions);
    numActiveWaves_ = 0;

    // Reset defaults
    resetToDefaults();
    
    lastUpdateMs_ = millis();
    return true;
}

void Water::generate(EffectMatrix& matrix, float energy, float hit) {
    setAudioInput(energy, hit);
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
    audioHit_ = false;
    numActiveWaves_ = 0;
}

void Water::update() {
    uint32_t currentMs = millis();
    if (currentMs - lastUpdateMs_ < 50) return; // 20 FPS
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

void Water::setAudioInput(float energy, bool hit) {
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
    if (audioHit_) {
        waveProb += params_.audioWaveBoost;
    }
    
    if (random(1000) / 1000.0f < waveProb) {
        int wavePosition;
        uint8_t waveHeight = random(params_.waveHeightMin, params_.waveHeightMax + 1);
        
        // Add audio boost to wave height
        if (audioEnergy_ > 0.1f) {
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
    uint8_t* newDepth = new uint8_t[numLeds_];
    memcpy(newDepth, depth_, numLeds_);
    
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
                        uint8_t flowAmount = depth_[i] / 4;
                        newDepth[belowIndex] = min(255, newDepth[belowIndex] + flowAmount);
                        newDepth[i] = max(0, newDepth[i] - flowAmount);
                    }
                    break;
                    
                case LINEAR_LAYOUT:
                    // Flow in both directions  
                    if (i > 0) {
                        uint8_t flowAmount = depth_[i] / 6;
                        newDepth[i - 1] = min(255, newDepth[i - 1] + flowAmount);
                        newDepth[i] = max(0, newDepth[i] - flowAmount);
                    }
                    if (i < numLeds_ - 1) {
                        uint8_t flowAmount = depth_[i] / 6;
                        newDepth[i + 1] = min(255, newDepth[i + 1] + flowAmount);
                        newDepth[i] = max(0, newDepth[i] - flowAmount);
                    }
                    break;
                    
                case RANDOM_LAYOUT:
                    // Ripple to nearby positions (simplified)
                    for (int j = max(0, i - 3); j <= min(numLeds_ - 1, i + 3); j++) {
                        if (j != i) {
                            uint8_t flowAmount = depth_[i] / 12;
                            newDepth[j] = min(255, newDepth[j] + flowAmount);
                            newDepth[i] = max(0, newDepth[i] - flowAmount);
                        }
                    }
                    break;
            }
        }
    }
    
    memcpy(depth_, newDepth, numLeds_);
    delete[] newDepth;
}

void Water::applyFlow() {
    // Apply base flow rate with audio influence
    uint8_t flowRate = params_.baseFlow;
    if (audioEnergy_ > 0.1f) {
        int8_t audioBias = (int8_t)(audioEnergy_ * params_.flowAudioBias);
        flowRate = constrain(flowRate + audioBias, 0, 255);
    }
    
    // Reduce all depths by flow rate
    for (int i = 0; i < numLeds_; i++) {
        if (depth_[i] > 0) {
            depth_[i] = max(0, depth_[i] - (flowRate / 20));
        }
    }
}

uint32_t Water::depthToColor(uint8_t depth) {
    if (depth == 0) {
        return 0x000000; // Black (no water)
    }
    
    // Blue color palette: deep blue -> cyan -> light blue
    uint8_t r, g, b;
    
    if (depth < 85) {
        // Deep blue to medium blue
        r = 0;
        g = 0;
        b = map(depth, 0, 84, 60, 150);
    } else if (depth < 170) {
        // Medium blue to cyan
        r = 0;
        g = map(depth, 85, 169, 0, 120);
        b = map(depth, 85, 169, 150, 255);
    } else {
        // Cyan to light blue
        r = map(depth, 170, 255, 0, 80);
        g = map(depth, 170, 255, 120, 200);
        b = 255;
    }
    
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
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

int Water::coordsToIndex(int x, int y) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) return -1;
    return y * width_ + x;
}

void Water::indexToCoords(int index, int& x, int& y) {
    if (index < 0 || index >= numLeds_) {
        x = y = -1;
        return;
    }
    x = index % width_;
    y = index / width_;
}