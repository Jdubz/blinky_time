#include "Fire.h"
#include <Arduino.h>

Fire::Fire()
    : heat_(nullptr), audioEnergy_(0.0f), audioHit_(0.0f),
      prevHit_(0.0f), lastBurstMs_(0), inSuppression_(false),
      emberNoisePhase_(0.0f), sparkPositions_(nullptr), numActivePositions_(0) {
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
    orientation_ = config.matrix.orientation;

    // Apply fire parameters from device config
    params_.baseCooling = config.fireDefaults.baseCooling;
    params_.sparkHeatMin = config.fireDefaults.sparkHeatMin;
    params_.sparkHeatMax = config.fireDefaults.sparkHeatMax;
    params_.sparkChance = config.fireDefaults.sparkChance;
    params_.audioSparkBoost = config.fireDefaults.audioSparkBoost;
    params_.audioHeatBoostMax = config.fireDefaults.audioHeatBoostMax;
    params_.coolingAudioBias = config.fireDefaults.coolingAudioBias;
    params_.bottomRowsForSparks = config.fireDefaults.bottomRowsForSparks;
    params_.transientHeatMax = config.fireDefaults.transientHeatMax;

    // Apply layout-specific defaults
    if (layout_ == LINEAR_LAYOUT) {
        params_.useMaxHeatOnly = true;
        params_.spreadDistance = 12;      // Wide spread for visible tails
        params_.heatDecay = 0.94f;        // Very slow decay for long ember trails
    }

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
    setAudioInput(energy, hit);
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

    // Apply subtle ember glow (noise floor)
    applyEmbers();
}

void Fire::reset() {
    if (heat_) {
        memset(heat_, 0, this->numLeds_);
    }
    numActivePositions_ = 0;
    audioEnergy_ = 0.0f;
    audioHit_ = 0.0f;
    this->lastUpdateMs_ = millis();
}

void Fire::setAudioInput(float energy, float hit) {
    audioEnergy_ = energy;
    audioHit_ = hit;
}

void Fire::setLayoutType(LayoutType layoutType) {
    this->layout_ = layoutType;
    // Adjust default parameters based on layout type
    switch (this->layout_) {
        case LINEAR_LAYOUT:
            params_.useMaxHeatOnly = true;
            params_.spreadDistance = 12;
            params_.heatDecay = 0.92f;
            break;
        case RANDOM_LAYOUT:
            params_.useMaxHeatOnly = false;
            params_.spreadDistance = 8;
            params_.heatDecay = 0.88f;
            break;
        case MATRIX_LAYOUT:
        default:
            params_.useMaxHeatOnly = false;
            params_.spreadDistance = 6;
            params_.heatDecay = 0.90f;
            break;
    }
}

void Fire::setOrientation(MatrixOrientation orientation) {
    orientation_ = orientation;
}

void Fire::setParams(const FireParams& params) {
    params_ = params;
}

void Fire::resetToDefaults() {
    params_ = FireParams();
    setLayoutType(this->layout_);  // Reapply layout-specific defaults
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
    memset(newHeat, 0, this->numLeds_);  // Start fresh - heat must spread or decay

    for (int i = 0; i < this->numLeds_; i++) {
        if (heat_[i] > 0) {
            // Long tail decay: high heat decays fast, low heat persists ~1 second
            // Using quartic (t^4) for very slow tail fade
            // At heat=255: decayRate = 0.88 (fast drop)
            // At heat=30: decayRate ~0.99 (very slow, ~1 sec to fade)
            float t = heat_[i] / 255.0f;
            float t4 = t * t * t * t;  // Quartic for dramatic tail
            float decayRate = 0.88f + 0.12f * (1.0f - t4);
            uint8_t decayedHeat = (uint8_t)(heat_[i] * decayRate);

            // Source pixel keeps decayed heat
            newHeat[i] = max(newHeat[i], decayedHeat);

            // Smaller spread for tighter sparks (only spread high heat)
            if (heat_[i] > 60) {
                int spreadDist = min((int)params_.spreadDistance, 4);  // Tighter spread
                for (int spread = 1; spread <= spreadDist; spread++) {
                    float falloff = 1.0f / (spread + 2);  // Steeper falloff
                    uint8_t heatToSpread = decayedHeat * falloff;

                    if (i - spread >= 0) {
                        newHeat[i - spread] = max(newHeat[i - spread], heatToSpread);
                    }
                    if (i + spread < this->numLeds_) {
                        newHeat[i + spread] = max(newHeat[i + spread], heatToSpread);
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
    uint32_t now = millis();
    int numSparks = 0;
    uint8_t sparkHeat = params_.sparkHeatMax;

    // Detect hit EDGE (rising edge - hit going from low to high)
    bool hitEdge = (audioHit_ > 0.5f && prevHit_ < 0.3f);
    prevHit_ = audioHit_;

    // Update suppression state
    if (inSuppression_ && (now - lastBurstMs_ > params_.suppressionMs)) {
        inSuppression_ = false;
    }

    // BASELINE: Generate sparks scaled by audio energy
    // Higher energy = more sparks and hotter sparks
    float energyBoost = audioEnergy_ * params_.audioSparkBoost;  // 0-1 scaled by boost
    float effectiveChance = params_.sparkChance + energyBoost;

    if (random(100) < (int)(effectiveChance * 100)) {
        numSparks = 1;
        // Heat scales with energy: low energy = min heat, high energy = max heat
        uint8_t energyHeat = params_.sparkHeatMin +
            (uint8_t)(audioEnergy_ * (params_.sparkHeatMax - params_.sparkHeatMin));
        sparkHeat = max(energyHeat, params_.sparkHeatMin);
    }

    // BURST: Add extra sparks on hit edge (only if not suppressed)
    if (hitEdge && !inSuppression_) {
        numSparks += params_.burstSparks;
        sparkHeat = 255;  // Max heat for punch
        lastBurstMs_ = now;
        inSuppression_ = true;  // Suppress further bursts briefly
    }

    // Generate the sparks
    for (int s = 0; s < numSparks; s++) {
        int sparkPosition;
        switch (this->layout_) {
            case MATRIX_LAYOUT:
                sparkPosition = random(this->width_ * params_.bottomRowsForSparks);
                break;
            case LINEAR_LAYOUT:
                sparkPosition = random(this->numLeds_);
                break;
            case RANDOM_LAYOUT:
                if (numActivePositions_ < params_.maxSparkPositions) {
                    sparkPosition = random(this->numLeds_);
                    sparkPositions_[numActivePositions_++] = sparkPosition;
                } else {
                    sparkPosition = random(this->numLeds_);
                    sparkPositions_[random(params_.maxSparkPositions)] = sparkPosition;
                }
                break;
            default:
                sparkPosition = random(this->numLeds_);
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

    // Reduce cooling when audio is present (flames persist longer with sound)
    if (audioEnergy_ > 0.05f) {
        int8_t reduction = (int8_t)(params_.coolingAudioBias * audioEnergy_ * 2.0f);
        cooling = max(0, (int)cooling + reduction);
    }

    for (int i = 0; i < this->numLeds_; i++) {
        uint8_t coolAmount = random(0, cooling + 1);
        heat_[i] = (heat_[i] > coolAmount) ? heat_[i] - coolAmount : 0;
    }
}

void Fire::applyEmbers() {
    // Advance noise phase very slowly
    emberNoisePhase_ += params_.emberNoiseSpeed * 30.0f;  // 30ms frame time

    // Ember brightness pulses directly with mic level (no transient influence)
    // Base brightness + audio-reactive component
    float micPulse = 0.3f + (audioEnergy_ * 2.0f);  // 0.3 base, scales up with mic
    micPulse = min(1.0f, micPulse);

    // Apply noise-based ember glow to all LEDs
    for (int i = 0; i < this->numLeds_; i++) {
        // Multi-octave noise using overlapping sine waves
        // Creates organic, slowly shifting patterns
        float pos = (float)i;
        float noise =
            sin(pos * 0.15f + emberNoisePhase_) * 0.4f +
            sin(pos * 0.37f + emberNoisePhase_ * 1.3f) * 0.35f +
            sin(pos * 0.71f + emberNoisePhase_ * 0.7f) * 0.25f;

        // Normalize to 0-1 range
        noise = (noise + 1.0f) * 0.5f;

        // Apply threshold so only some areas glow (sparse embers)
        if (noise > 0.55f) {
            float intensity = (noise - 0.55f) / 0.45f;  // 0-1 above threshold
            uint8_t emberHeat = (uint8_t)(intensity * micPulse * params_.emberHeatMax);

            // Only apply if ember is brighter than current heat
            if (emberHeat > heat_[i]) {
                heat_[i] = emberHeat;
            }
        }
    }
}

uint32_t Fire::heatToColor(uint8_t heat) {
    // Fire color palette: black -> red -> orange -> yellow (NO white)
    if (heat < 85) {
        // Black to red
        return ((uint32_t)(heat * 3) << 16);
    } else if (heat < 170) {
        // Red to orange
        uint8_t green = (heat - 85) * 3;
        return (0xFF0000 | ((uint32_t)green << 8));
    } else {
        // Orange to bright yellow (cap at full yellow, no blue/white)
        return 0xFFFF00;
    }
}

int Fire::coordsToIndex(int x, int y) {
    if (x < 0 || x >= this->width_ || y < 0 || y >= this->height_) {
        return -1;
    }

    // Handle different orientations and wiring patterns
    switch (orientation_) {
        case VERTICAL:
            // Zigzag pattern for vertical orientation
            if (x % 2 == 0) {
                // Even columns: top to bottom
                return x * this->height_ + y;
            } else {
                // Odd columns: bottom to top
                return x * this->height_ + (this->height_ - 1 - y);
            }
        case HORIZONTAL:
        default:
            // Standard row-major order
            return y * this->width_ + x;
    }
}

void Fire::indexToCoords(int index, int& x, int& y) {
    if (index < 0 || index >= this->numLeds_) {
        x = y = -1;
        return;
    }

    switch (orientation_) {
        case VERTICAL:
            // Reverse of zigzag pattern
            x = index / this->height_;
            if (x % 2 == 0) {
                // Even columns: top to bottom
                y = index % this->height_;
            } else {
                // Odd columns: bottom to top
                y = this->height_ - 1 - (index % this->height_);
            }
            break;
        case HORIZONTAL:
        default:
            // Standard row-major order
            x = index % this->width_;
            y = index / this->width_;
            break;
    }
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
