#include "Lightning.h"
#include <Arduino.h>

Lightning::Lightning()
    : intensity_(nullptr), audioEnergy_(0.0f), audioHit_(false),
      boltPositions_(nullptr), numActiveBolts_(0) {
}

Lightning::~Lightning() {
    if (intensity_) {
        delete[] intensity_;
        intensity_ = nullptr;
    }
    if (boltPositions_) {
        delete[] boltPositions_;
        boltPositions_ = nullptr;
    }
}

bool Lightning::begin(const DeviceConfig& config) {
    width_ = config.layout.width;
    height_ = config.layout.height;
    numLeds_ = width_ * height_;
    layout_ = config.layout.type;

    // Allocate intensity array
    if (intensity_) delete[] intensity_;
    intensity_ = new uint8_t[numLeds_];
    memset(intensity_, 0, numLeds_);

    // Allocate bolt positions for random layout
    if (boltPositions_) delete[] boltPositions_;
    boltPositions_ = new uint8_t[params_.maxBoltPositions];
    memset(boltPositions_, 0, params_.maxBoltPositions);
    numActiveBolts_ = 0;

    // Reset defaults
    resetToDefaults();
    
    lastUpdateMs_ = millis();
    return true;
}

void Lightning::generate(EffectMatrix& matrix, float energy, float hit) {
    setAudioInput(energy, hit);
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
    audioHit_ = false;
    numActiveBolts_ = 0;
}

void Lightning::update() {
    uint32_t currentMs = millis();
    if (currentMs - lastUpdateMs_ < 30) return; // ~33 FPS (faster for lightning)
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

void Lightning::setAudioInput(float energy, bool hit) {
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
    if (audioHit_) {
        boltProb += params_.audioBoltBoost;
    }
    
    if (random(1000) / 1000.0f < boltProb) {
        int boltPosition;
        uint8_t boltIntensity = random(params_.boltIntensityMin, params_.boltIntensityMax + 1);
        
        // Add audio boost to bolt intensity
        if (audioEnergy_ > 0.1f) {
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
                if (random(100) < params_.branchChance) {
                    int x, y;
                    indexToCoords(boltPosition, x, y);
                    
                    // Branch in random directions
                    for (int dir = 0; dir < 4; dir++) {
                        if (random(100) < 50) { // 50% chance each direction
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
    int branchLength = random(2, 6);
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
        if (random(100) < 30) {
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
    uint8_t* newIntensity = new uint8_t[numLeds_];
    memcpy(newIntensity, intensity_, numLeds_);
    
    for (int i = 0; i < numLeds_; i++) {
        if (intensity_[i] > 50) { // Only propagate strong bolts
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
                                if (newIndex >= 0 && random(100) < 20) { // 20% propagation chance
                                    uint8_t propagatedIntensity = intensity_[i] / 3;
                                    newIntensity[newIndex] = max(newIntensity[newIndex], propagatedIntensity);
                                }
                            }
                        }
                    }
                    break;
                    
                case LINEAR_LAYOUT:
                    // Propagate along the line
                    if (i > 0 && random(100) < 30) {
                        uint8_t propagatedIntensity = intensity_[i] / 2;
                        newIntensity[i - 1] = max(newIntensity[i - 1], propagatedIntensity);
                    }
                    if (i < numLeds_ - 1 && random(100) < 30) {
                        uint8_t propagatedIntensity = intensity_[i] / 2;
                        newIntensity[i + 1] = max(newIntensity[i + 1], propagatedIntensity);
                    }
                    break;
                    
                case RANDOM_LAYOUT:
                    // Propagate to nearby positions (arc effect)
                    for (int j = max(0, i - 5); j <= min(numLeds_ - 1, i + 5); j++) {
                        if (j != i && random(100) < 15) { // 15% arc chance
                            uint8_t propagatedIntensity = intensity_[i] / 4;
                            newIntensity[j] = max(newIntensity[j], propagatedIntensity);
                        }
                    }
                    break;
            }
        }
    }
    
    memcpy(intensity_, newIntensity, numLeds_);
    delete[] newIntensity;
}

void Lightning::applyFade() {
    // Apply fade rate with audio influence
    uint8_t fadeRate = params_.baseFade;
    if (audioEnergy_ > 0.1f) {
        int8_t audioBias = (int8_t)(audioEnergy_ * params_.fadeAudioBias);
        fadeRate = constrain(fadeRate + audioBias, 50, 255);
    }
    
    // Fade all intensities
    for (int i = 0; i < numLeds_; i++) {
        if (intensity_[i] > 0) {
            intensity_[i] = max(0, intensity_[i] - (fadeRate / 10));
        }
    }
}

uint32_t Lightning::intensityToColor(uint8_t intensity) {
    if (intensity == 0) {
        return 0x000000; // Black (no lightning)
    }
    
    // Lightning color palette: yellow -> white -> electric blue
    uint8_t r, g, b;
    
    if (intensity < 85) {
        // Dark yellow to bright yellow
        r = map(intensity, 0, 84, 60, 255);
        g = map(intensity, 0, 84, 50, 200);
        b = 0;
    } else if (intensity < 170) {
        // Bright yellow to white
        r = 255;
        g = map(intensity, 85, 169, 200, 255);
        b = map(intensity, 85, 169, 0, 180);
    } else {
        // White to electric blue
        r = map(intensity, 170, 255, 255, 150);
        g = map(intensity, 170, 255, 255, 200);
        b = map(intensity, 170, 255, 180, 255);
    }
    
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
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

int Lightning::coordsToIndex(int x, int y) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) return -1;
    return y * width_ + x;
}

void Lightning::indexToCoords(int index, int& x, int& y) {
    if (index < 0 || index >= numLeds_) {
        x = y = -1;
        return;
    }
    x = index % width_;
    y = index / width_;
}