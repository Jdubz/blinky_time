#include "Fire.h"
#include <Arduino.h>

Fire::Fire()
    : heat_(nullptr), audioEnergy_(0.0f), audioHit_(false),
      sparkPositions_(nullptr), numActivePositions_(0) {
}

Fire::~Fire() {
    if (heat_) {
        delete[] heat_;
        heat_ = nullptr;
    }
    if (sparkPositions_) {
        delete[] sparkPositions_;
        sparkPositions_ = nullptr;
    }
}

bool Fire::begin(const DeviceConfig& config) {
    this->width_ = config.matrix.width;
    this->height_ = config.matrix.height;
    this->numLeds_ = this->width_ * this->height_;
    layout_ = config.matrix.layoutType;

    // Allocate heat array
    if (heat_) delete[] heat_;
    heat_ = new uint8_t[this->numLeds_];
    memset(heat_, 0, this->numLeds_);

    // Allocate spark positions for random layout
    if (sparkPositions_) delete[] sparkPositions_;
    sparkPositions_ = new uint8_t[params_.maxSparkPositions];
    memset(sparkPositions_, 0, params_.maxSparkPositions);
    numActivePositions_ = 0;

    this->lastUpdateMs_ = millis();
    return true;
}

void Fire::generate(PixelMatrix& matrix, float energy, float hit) {
    setAudioInput(energy, hit > 0.5f);
    update();

    // Convert heat array to PixelMatrix colors
    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            int index = coordsToIndex(x, y);
            uint32_t color = heatToColor(heat_[index]);
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;
            matrix.setPixel(x, y, r, g, b);
        }
    }
}

void Fire::update() {
    unsigned long currentMs = millis();
    if (currentMs - this->lastUpdateMs_ < 30) {  // ~33 FPS max
        return;
    }
    this->lastUpdateMs_ = currentMs;

    // Apply cooling first
    applyCooling();

    // Generate sparks based on audio input
    generateSparks();

    // Propagate heat based on layout type
    propagateHeat();
}

void Fire::reset() {
    if (heat_) {
        memset(heat_, 0, this->numLeds_);
    }
    numActivePositions_ = 0;
    audioEnergy_ = 0.0f;
    audioHit_ = false;
    this->lastUpdateMs_ = millis();
}

void Fire::setAudioInput(float energy, bool hit) {
    audioEnergy_ = energy;
    audioHit_ = hit;
}

// TODO: These methods need to be added to Fire.h header
// void Fire::setLayoutType(LayoutType layoutType) {
//     this->layout_ = layoutType;
//     // Adjust default parameters based on layout type
//     switch (this->layout_) {
//         case LINEAR_LAYOUT:
//             params_.useMaxHeatOnly = true;
//             params_.spreadDistance = 12;
//             params_.heatDecay = 0.92f;
//             break;
//         case RANDOM_LAYOUT:
//             params_.useMaxHeatOnly = false;
//             params_.spreadDistance = 8;
//             params_.heatDecay = 0.88f;
//             break;
//         case MATRIX_LAYOUT:
//         default:
//             params_.useMaxHeatOnly = false;
//             params_.spreadDistance = 6;
//             params_.heatDecay = 0.90f;
//             break;
//     }
// }

// void Fire::setOrientation(MatrixOrientation orientation) {
//     orientation_ = orientation;
// }

void Fire::setParams(const FireParams& params) {
    params_ = params;
}

void Fire::resetToDefaults() {
    params_ = FireParams();
    // TODO: Re-enable when setLayoutType is added to header
    // setLayoutType(this->layout_);  // Reapply layout-specific defaults
}

void Fire::propagateHeat() {
    switch (this->layout_) {
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

void Fire::updateMatrixFire() {
    // Traditional upward heat propagation for 2D matrices
    for (int x = 0; x < this->width_; x++) {
        for (int y = this->height_ - 1; y >= 2; y--) {
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
                if (x < this->width_ - 1) {
                    int rightIndex = coordsToIndex(x + 1, y - 1);
                    if (rightIndex >= 0) newHeat = (newHeat + heat_[rightIndex]) / 2;
                }

                heat_[currentIndex] = min(255, newHeat);
            }
        }
    }
}

void Fire::updateLinearFire() {
    // Lateral heat propagation for linear arrangements
    uint8_t* newHeat = new uint8_t[this->numLeds_];
    memcpy(newHeat, heat_, this->numLeds_);

    for (int i = 0; i < this->numLeds_; i++) {
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
                if (i + spread < this->numLeds_) {
                    if (params_.useMaxHeatOnly) {
                        newHeat[i + spread] = max(newHeat[i + spread], heatToSpread);
                    } else {
                        newHeat[i + spread] = min(255, newHeat[i + spread] + heatToSpread);
                    }
                }
            }
        }
    }

    memcpy(heat_, newHeat, this->numLeds_);
    delete[] newHeat;
}

void Fire::updateRandomFire() {
    // Omnidirectional heat propagation for random/scattered layouts
    uint8_t* newHeat = new uint8_t[this->numLeds_];
    memcpy(newHeat, heat_, this->numLeds_);

    for (int i = 0; i < this->numLeds_; i++) {
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

    memcpy(heat_, newHeat, this->numLeds_);
    delete[] newHeat;
}

void Fire::generateSparks() {
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
        switch (this->layout_) {
            case MATRIX_LAYOUT:
                // Generate sparks in bottom rows only
                sparkPosition = random(this->width_ * params_.bottomRowsForSparks);
                break;
            case LINEAR_LAYOUT:
                // Generate sparks anywhere along the string
                sparkPosition = random(this->numLeds_);
                break;
            case RANDOM_LAYOUT:
                // Track multiple spark positions
                if (numActivePositions_ < params_.maxSparkPositions) {
                    sparkPosition = random(this->numLeds_);
                    sparkPositions_[numActivePositions_++] = sparkPosition;
                } else {
                    // Replace oldest spark
                    sparkPosition = random(this->numLeds_);
                    sparkPositions_[random(params_.maxSparkPositions)] = sparkPosition;
                }
                break;
        }

        if (sparkPosition < this->numLeds_) {
            if (params_.useMaxHeatOnly) {
                heat_[sparkPosition] = max(heat_[sparkPosition], sparkHeat);
            } else {
                heat_[sparkPosition] = min(255, heat_[sparkPosition] + sparkHeat);
            }
        }
    }
}

void Fire::applyCooling() {
    uint8_t cooling = params_.baseCooling;

    // Adjust cooling based on audio input
    if (audioEnergy_ > 0.1f) {
        cooling = max(0, cooling + params_.coolingAudioBias);
    }

    for (int i = 0; i < this->numLeds_; i++) {
        uint8_t coolAmount = random(0, cooling + 1);
        heat_[i] = (heat_[i] > coolAmount) ? heat_[i] - coolAmount : 0;
    }
}

uint32_t Fire::heatToColor(uint8_t heat) {
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

int Fire::coordsToIndex(int x, int y) {
    if (x < 0 || x >= this->width_ || y < 0 || y >= this->height_) {
        return -1;
    }

    // Standard row-major order
    // TODO: Add orientation support when orientation_ field is added to header
    return y * this->width_ + x;
}

void Fire::indexToCoords(int index, int& x, int& y) {
    if (index < 0 || index >= this->numLeds_) {
        x = y = -1;
        return;
    }

    // Standard row-major order
    // TODO: Add orientation support when orientation_ field is added to header
    x = index % this->width_;
    y = index / this->width_;
}

// Implement parameter setters
void Fire::setBaseCooling(uint8_t cooling) {
    params_.baseCooling = cooling;
}

void Fire::setSparkParams(uint8_t heatMin, uint8_t heatMax, float chance) {
    params_.sparkHeatMin = heatMin;
    params_.sparkHeatMax = heatMax;
    params_.sparkChance = chance;
}

void Fire::setAudioParams(float sparkBoost, uint8_t heatBoostMax, int8_t coolingBias) {
    params_.audioSparkBoost = sparkBoost;
    params_.audioHeatBoostMax = heatBoostMax;
    params_.coolingAudioBias = coolingBias;
}

// Factory function - Disabled until setLayoutType/setOrientation are added to header
// Fire* createFireGenerator(const DeviceConfig& config) {
//     Fire* generator = new Fire();
//     // Configure layout type from device config
//     generator->setLayoutType(config.matrix.layoutType);
//     generator->setOrientation(config.matrix.orientation);
//     return generator;
// }
