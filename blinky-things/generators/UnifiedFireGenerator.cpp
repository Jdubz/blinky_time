#include "UnifiedFireGenerator.h"
#include <Arduino.h>

UnifiedFireGenerator::UnifiedFireGenerator() 
    : width_(0), height_(0), numLeds_(0), heat_(nullptr), lastUpdateMs_(0),
      layoutType_(LAYOUT_MATRIX), orientation_(HORIZONTAL), 
      audioEnergy_(0.0f), audioHit_(false),
      sparkPositions_(nullptr), numActivePositions_(0) {
}

UnifiedFireGenerator::~UnifiedFireGenerator() {
    if (heat_) {
        delete[] heat_;
        heat_ = nullptr;
    }
    if (sparkPositions_) {
        delete[] sparkPositions_;
        sparkPositions_ = nullptr;
    }
}

bool UnifiedFireGenerator::begin(int width, int height) {
    // Default to matrix layout for backward compatibility
    return begin(width, height, LAYOUT_MATRIX);
}

bool UnifiedFireGenerator::begin(int width, int height, LayoutType layoutType) {
    width_ = width;
    height_ = height;
    numLeds_ = width * height;
    layoutType_ = layoutType;
    
    // Allocate heat array
    if (heat_) delete[] heat_;
    heat_ = new uint8_t[numLeds_];
    memset(heat_, 0, numLeds_);
    
    // Allocate spark positions for random layout
    if (sparkPositions_) delete[] sparkPositions_;
    sparkPositions_ = new uint8_t[params_.maxSparkPositions];
    memset(sparkPositions_, 0, params_.maxSparkPositions);
    numActivePositions_ = 0;
    
    lastUpdateMs_ = millis();
    return true;
}

void UnifiedFireGenerator::update() {
    unsigned long currentMs = millis();
    if (currentMs - lastUpdateMs_ < 30) {  // ~33 FPS max
        return;
    }
    lastUpdateMs_ = currentMs;
    
    // Apply cooling first
    applyCooling();
    
    // Generate sparks based on audio input
    generateSparks();
    
    // Propagate heat based on layout type
    propagateHeat();
}

void UnifiedFireGenerator::generate(EffectMatrix* matrix) {
    if (!matrix || !heat_) return;
    
    for (int i = 0; i < numLeds_; i++) {
        uint32_t color = heatToColor(heat_[i]);
        int x, y;
        indexToCoords(i, x, y);
        matrix->setPixel(x, y, color);
    }
}

void UnifiedFireGenerator::setAudioInput(float energy, bool hit) {
    audioEnergy_ = energy;
    audioHit_ = hit;
}

void UnifiedFireGenerator::setLayoutType(LayoutType layoutType) {
    layoutType_ = layoutType;
    
    // Adjust default parameters based on layout type
    switch (layoutType_) {
        case LINEAR_LAYOUT:
            params_.useMaxHeatOnly = true;   // Use max heat instead of additive
            params_.spreadDistance = 12;     // Wider spread for linear
            params_.heatDecay = 0.92f;      // Slower decay
            break;
        case RANDOM_LAYOUT:
            params_.useMaxHeatOnly = false;  // Allow additive heat
            params_.spreadDistance = 8;      // Moderate spread
            params_.heatDecay = 0.88f;      // Faster decay for randomness
            break;
        case MATRIX_LAYOUT:
        default:
            params_.useMaxHeatOnly = false;  // Traditional additive
            params_.spreadDistance = 6;      // Upward focused
            params_.heatDecay = 0.90f;      // Standard decay
            break;
    }
}

void UnifiedFireGenerator::setOrientation(MatrixOrientation orientation) {
    orientation_ = orientation;
}

void UnifiedFireGenerator::setParams(const UnifiedFireParams& params) {
    params_ = params;
}

void UnifiedFireGenerator::resetToDefaults() {
    params_ = UnifiedFireParams();
    setLayoutType(layoutType_);  // Reapply layout-specific defaults
}

void UnifiedFireGenerator::propagateHeat() {
    switch (layoutType_) {
        case MATRIX_LAYOUT:
            updateMatrixFire();
            break;
        case LINEAR_LAYOUT:
            updateLinearFire();
            break;
        case RANDOM_LAYOUT:
            updateRandomFire();
            break;
    }
}

void UnifiedFireGenerator::updateMatrixFire() {
    // Traditional upward heat propagation for 2D matrices
    for (int x = 0; x < width_; x++) {
        for (int y = height_ - 1; y >= 2; y--) {
            int currentIndex = coordsToIndex(x, y);
            int belowIndex = coordsToIndex(x, y - 1);
            int below2Index = coordsToIndex(x, y - 2);
            
            if (currentIndex >= 0 && belowIndex >= 0 && below2Index >= 0) {
                uint16_t newHeat = (heat_[belowIndex] + heat_[below2Index] * 2) / 3;
                
                // Add horizontal spread
                if (x > 0) {
                    int leftIndex = coordsToIndex(x - 1, y - 1);
                    if (leftIndex >= 0) newHeat = (newHeat + heat_[leftIndex]) / 2;
                }
                if (x < width_ - 1) {
                    int rightIndex = coordsToIndex(x + 1, y - 1);
                    if (rightIndex >= 0) newHeat = (newHeat + heat_[rightIndex]) / 2;
                }
                
                heat_[currentIndex] = min(255, newHeat);
            }
        }
    }
}

void UnifiedFireGenerator::updateLinearFire() {
    // Lateral heat propagation for linear arrangements
    uint8_t* newHeat = new uint8_t[numLeds_];
    memcpy(newHeat, heat_, numLeds_);
    
    for (int i = 0; i < numLeds_; i++) {
        if (heat_[i] > 0) {
            uint16_t spreadHeat = heat_[i] * params_.heatDecay;
            
            // Spread heat laterally
            for (int spread = 1; spread <= params_.spreadDistance; spread++) {
                float falloff = 1.0f / (spread + 1);
                uint8_t heatToSpread = spreadHeat * falloff;
                
                // Spread left
                if (i - spread >= 0) {
                    if (params_.useMaxHeatOnly) {
                        newHeat[i - spread] = max(newHeat[i - spread], heatToSpread);
                    } else {
                        newHeat[i - spread] = min(255, newHeat[i - spread] + heatToSpread);
                    }
                }
                
                // Spread right
                if (i + spread < numLeds_) {
                    if (params_.useMaxHeatOnly) {
                        newHeat[i + spread] = max(newHeat[i + spread], heatToSpread);
                    } else {
                        newHeat[i + spread] = min(255, newHeat[i + spread] + heatToSpread);
                    }
                }
            }
        }
    }
    
    memcpy(heat_, newHeat, numLeds_);
    delete[] newHeat;
}

void UnifiedFireGenerator::updateRandomFire() {
    // Omnidirectional heat propagation for random/scattered layouts
    uint8_t* newHeat = new uint8_t[numLeds_];
    memcpy(newHeat, heat_, numLeds_);
    
    for (int i = 0; i < numLeds_; i++) {
        if (heat_[i] > 0) {
            int x, y;
            indexToCoords(i, x, y);
            uint16_t spreadHeat = heat_[i] * params_.heatDecay;
            
            // Spread heat in all directions
            for (int dx = -params_.spreadDistance; dx <= params_.spreadDistance; dx++) {
                for (int dy = -params_.spreadDistance; dy <= params_.spreadDistance; dy++) {
                    if (dx == 0 && dy == 0) continue;
                    
                    int targetX = x + dx;
                    int targetY = y + dy;
                    int targetIndex = coordsToIndex(targetX, targetY);
                    
                    if (targetIndex >= 0) {
                        float distance = sqrt(dx*dx + dy*dy);
                        float falloff = 1.0f / (distance + 1);
                        uint8_t heatToSpread = spreadHeat * falloff;
                        
                        newHeat[targetIndex] = min(255, newHeat[targetIndex] + heatToSpread);
                    }
                }
            }
        }
    }
    
    memcpy(heat_, newHeat, numLeds_);
    delete[] newHeat;
}

void UnifiedFireGenerator::generateSparks() {
    // Base spark chance modified by audio
    float sparkChance = params_.sparkChance;
    if (audioHit_) {
        sparkChance += params_.audioSparkBoost;
    }
    
    if (random(1000) < sparkChance * 1000) {
        uint8_t sparkHeat = random(params_.sparkHeatMin, params_.sparkHeatMax + 1);
        
        // Add audio boost to spark heat
        if (audioEnergy_ > 0.1f) {
            sparkHeat = min(255, sparkHeat + (audioEnergy_ * params_.audioHeatBoostMax));
        }
        
        int sparkPosition;
        switch (layoutType_) {
            case MATRIX_LAYOUT:
                // Generate sparks in bottom rows only
                sparkPosition = random(width_ * params_.bottomRowsForSparks);
                break;
            case LINEAR_LAYOUT:
                // Generate sparks anywhere along the string
                sparkPosition = random(numLeds_);
                break;
            case RANDOM_LAYOUT:
                // Track multiple spark positions
                if (numActivePositions_ < params_.maxSparkPositions) {
                    sparkPosition = random(numLeds_);
                    sparkPositions_[numActivePositions_++] = sparkPosition;
                } else {
                    // Replace oldest spark
                    sparkPosition = random(numLeds_);
                    sparkPositions_[random(params_.maxSparkPositions)] = sparkPosition;
                }
                break;
        }
        
        if (sparkPosition < numLeds_) {
            if (params_.useMaxHeatOnly) {
                heat_[sparkPosition] = max(heat_[sparkPosition], sparkHeat);
            } else {
                heat_[sparkPosition] = min(255, heat_[sparkPosition] + sparkHeat);
            }
        }
    }
}

void UnifiedFireGenerator::applyCooling() {
    uint8_t cooling = params_.baseCooling;
    
    // Adjust cooling based on audio input
    if (audioEnergy_ > 0.1f) {
        cooling = max(0, cooling + params_.coolingAudioBias);
    }
    
    for (int i = 0; i < numLeds_; i++) {
        uint8_t coolAmount = random(0, cooling + 1);
        heat_[i] = (heat_[i] > coolAmount) ? heat_[i] - coolAmount : 0;
    }
}

uint32_t UnifiedFireGenerator::heatToColor(uint8_t heat) {
    // Fire color palette: black -> red -> orange -> yellow -> white
    if (heat < 85) {
        // Black to red
        return ((uint32_t)(heat * 3) << 16);
    } else if (heat < 170) {
        // Red to orange/yellow
        uint8_t green = (heat - 85) * 3;
        return (0xFF0000 | ((uint32_t)green << 8));
    } else {
        // Orange/yellow to white
        uint8_t blue = (heat - 170) * 3;
        return (0xFFFF00 | blue);
    }
}

int UnifiedFireGenerator::coordsToIndex(int x, int y) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        return -1;
    }
    
    // Handle different orientations and wiring patterns
    switch (orientation_) {
        case VERTICAL:
            // Zigzag pattern for vertical orientation
            if (x % 2 == 0) {
                // Even columns: top to bottom
                return x * height_ + y;
            } else {
                // Odd columns: bottom to top
                return x * height_ + (height_ - 1 - y);
            }
        case HORIZONTAL:
        default:
            // Standard row-major order
            return y * width_ + x;
    }
}

void UnifiedFireGenerator::indexToCoords(int index, int& x, int& y) {
    if (index < 0 || index >= numLeds_) {
        x = y = -1;
        return;
    }
    
    switch (orientation_) {
        case VERTICAL:
            // Reverse of zigzag pattern
            x = index / height_;
            if (x % 2 == 0) {
                // Even columns: top to bottom
                y = index % height_;
            } else {
                // Odd columns: bottom to top
                y = height_ - 1 - (index % height_);
            }
            break;
        case HORIZONTAL:
        default:
            // Standard row-major order
            x = index % width_;
            y = index / width_;
            break;
    }
}

// Implement parameter setters
void UnifiedFireGenerator::setBaseCooling(uint8_t cooling) {
    params_.baseCooling = cooling;
}

void UnifiedFireGenerator::setSparkParams(uint8_t heatMin, uint8_t heatMax, float chance) {
    params_.sparkHeatMin = heatMin;
    params_.sparkHeatMax = heatMax;
    params_.sparkChance = chance;
}

void UnifiedFireGenerator::setAudioParams(float sparkBoost, uint8_t heatBoostMax, int8_t coolingBias) {
    params_.audioSparkBoost = sparkBoost;
    params_.audioHeatBoostMax = heatBoostMax;
    params_.coolingAudioBias = coolingBias;
}

// Factory function
UnifiedFireGenerator* createFireGenerator(const DeviceConfig& config) {
    UnifiedFireGenerator* generator = new UnifiedFireGenerator();
    
    // Configure layout type from device config
    generator->setLayoutType(config.layoutType);
    generator->setOrientation(config.matrix.orientation);
    
    return generator;
}