#include "FireGenerator.h"
#include "../../TotemDefaults.h"
#include <Arduino.h>

FireGenerator::FireGenerator() 
    : width_(0), height_(0), heat_(nullptr), lastUpdateMs_(0), 
      currentEnergy_(0.0f), currentHit_(0.0f) {
    restoreDefaults();
}

FireGenerator::~FireGenerator() {
    if (heat_) {
        free(heat_);
        heat_ = nullptr;
    }
}

void FireGenerator::begin(int width, int height) {
    width_ = width;
    height_ = height;
    
    if (heat_) {
        free(heat_);
        heat_ = nullptr;
    }
    
    heat_ = (float*)malloc(sizeof(float) * width * height);
    if (!heat_) {
        Serial.println(F("FireGenerator: Failed to allocate heat buffer"));
        return;
    }
    
    clearHeat();
    lastUpdateMs_ = 0;
}

void FireGenerator::restoreDefaults() {
    // Use TotemDefaults for consistent behavior
    params.baseCooling         = Defaults::BaseCooling;
    params.sparkHeatMin        = Defaults::SparkHeatMin;
    params.sparkHeatMax        = Defaults::SparkHeatMax;
    params.sparkChance         = Defaults::SparkChance;
    params.audioSparkBoost     = Defaults::AudioSparkBoost;
    params.audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
    params.coolingAudioBias    = Defaults::CoolingAudioBias;
    params.bottomRowsForSparks = Defaults::BottomRowsForSparks;
    params.transientHeatMax    = Defaults::TransientHeatMax;
}

void FireGenerator::setAudioInput(float energy, float hit) {
    currentEnergy_ = energy;
    currentHit_ = hit;
}

void FireGenerator::reset() {
    clearHeat();
    currentEnergy_ = 0.0f;
    currentHit_ = 0.0f;
    lastUpdateMs_ = 0;
}

void FireGenerator::update() {
    if (!heat_) return;
    
    unsigned long currentMs = millis();
    if (lastUpdateMs_ == 0) {
        lastUpdateMs_ = currentMs;
        return;
    }
    
    float deltaMs = currentMs - lastUpdateMs_;
    lastUpdateMs_ = currentMs;
    
    // Step 1: Cool down every cell a little
    uint8_t baseCooling = params.baseCooling;
    int8_t audioAdjustment = params.coolingAudioBias * currentEnergy_;
    baseCooling = max(0, min(255, baseCooling + audioAdjustment));
    
    float coolingAmount = (baseCooling / 255.0f) * (deltaMs / 16.67f); // Normalize to ~60fps
    
    for (int i = 0; i < width_ * height_; i++) {
        heat_[i] = max(0.0f, heat_[i] - coolingAmount);
    }
    
    // Step 2: Heat from each cell drifts 'up' and diffuses
    for (int x = 0; x < width_; x++) {
        for (int y = height_ - 1; y >= 2; y--) {
            float currentHeat = getHeatValue(x, y);
            if (currentHeat > 0.01f) {
                // Calculate how much heat moves up
                float heatToMove = currentHeat * 0.3f * (deltaMs / 16.67f);
                
                // Move heat up with some horizontal diffusion
                getHeatRef(x, y) -= heatToMove;
                
                // Distribute to cells above
                float upwardHeat = heatToMove * 0.7f;
                float sideHeat = heatToMove * 0.15f;
                
                getHeatRef(x, y - 1) += upwardHeat;
                getHeatRef(wrapX(x - 1), y - 1) += sideHeat;
                getHeatRef(wrapX(x + 1), y - 1) += sideHeat;
            }
        }
    }
    
    // Step 3: Add sparks at the bottom
    int bottomRows = min(params.bottomRowsForSparks, height_);
    
    for (int x = 0; x < width_; x++) {
        for (int y = height_ - bottomRows; y < height_; y++) {
            float sparkChance = params.sparkChance;
            
            // Audio boost for spark generation
            if (currentHit_ > 0.1f) {
                sparkChance += params.audioSparkBoost * currentHit_;
            }
            
            if (random(1000) / 1000.0f < sparkChance) {
                float sparkHeat = random(params.sparkHeatMin, params.sparkHeatMax + 1) / 255.0f;
                
                // Audio boost for spark intensity
                if (currentEnergy_ > 0.1f) {
                    float audioBoost = (params.audioHeatBoostMax / 255.0f) * currentEnergy_;
                    sparkHeat = min(1.0f, sparkHeat + audioBoost);
                }
                
                getHeatRef(x, y) = max(getHeatValue(x, y), sparkHeat);
            }
        }
    }
    
    // Step 4: Add transient heat for dramatic effect
    if (currentHit_ > 0.3f) {
        for (int x = 0; x < width_; x++) {
            for (int y = height_ - 2; y < height_; y++) {
                if (random(100) < 30) { // 30% chance
                    float transientHeat = (params.transientHeatMax / 255.0f) * currentHit_;
                    getHeatRef(x, y) = min(1.0f, getHeatValue(x, y) + transientHeat);
                }
            }
        }
    }
}

void FireGenerator::generate(EffectMatrix* matrix) {
    if (!heat_ || !matrix) return;
    
    // Convert heat values to colors and fill the matrix
    for (int x = 0; x < width_; x++) {
        for (int y = 0; y < height_; y++) {
            float heat = getHeatValue(x, y);
            RGB color = heatToColor(heat);
            matrix->setPixel(x, y, color);
        }
    }
}

void FireGenerator::generate(EffectMatrix& matrix, float energy, float hit) {
    // Set audio input and generate
    setAudioInput(energy, hit);
    generate(&matrix);
}

// Helper functions (same as original FireVisualEffect)
float& FireGenerator::getHeatRef(int x, int y) {
    x = wrapX(x);
    y = max(0, min(height_ - 1, y));
    return heat_[y * width_ + x];
}

float FireGenerator::getHeatValue(int x, int y) const {
    if (!heat_) return 0.0f;
    x = wrapX(x);
    y = max(0, min(height_ - 1, y));
    return heat_[y * width_ + x];
}

RGB FireGenerator::heatToColor(float heat) const {
    // Convert heat (0.0-1.0) to fire colors (black -> red -> orange -> yellow -> white)
    heat = max(0.0f, min(1.0f, heat));
    
    if (heat < 0.25f) {
        // Black to red
        float t = heat * 4.0f;
        return RGB{(uint8_t)(255 * t), 0, 0};
    } else if (heat < 0.5f) {
        // Red to orange
        float t = (heat - 0.25f) * 4.0f;
        return RGB{255, (uint8_t)(128 * t), 0};
    } else if (heat < 0.75f) {
        // Orange to yellow
        float t = (heat - 0.5f) * 4.0f;
        return RGB{255, (uint8_t)(128 + 127 * t), 0};
    } else {
        // Yellow to white
        float t = (heat - 0.75f) * 4.0f;
        return RGB{255, 255, (uint8_t)(255 * t)};
    }
}

int FireGenerator::wrapX(int x) const {
    while (x < 0) x += width_;
    while (x >= width_) x -= width_;
    return x;
}

int FireGenerator::wrapY(int y) const {
    while (y < 0) y += height_;
    while (y >= height_) y -= height_;
    return y;
}

// Testing helpers
void FireGenerator::setHeat(int x, int y, float heat) {
    if (heat_ && x >= 0 && x < width_ && y >= 0 && y < height_) {
        getHeatRef(x, y) = max(0.0f, min(1.0f, heat));
    }
}

float FireGenerator::getHeat(int x, int y) const {
    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        return getHeatValue(x, y);
    }
    return 0.0f;
}

void FireGenerator::clearHeat() {
    if (heat_) {
        for (int i = 0; i < width_ * height_; i++) {
            heat_[i] = 0.0f;
        }
    }
}