#include "Fire.h"
#include <Arduino.h>

// PROGMEM compatibility for non-AVR platforms (e.g., nRF52840)
#if defined(ARDUINO_ARCH_AVR)
#include <avr/pgmspace.h>
#else
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#endif
#endif

// ============================================================================
// Simplex Noise Implementation (2D)
// Based on Stefan Gustavson's simplex noise, optimized for embedded systems
// ============================================================================

namespace {
    // Permutation table (256 entries, doubled to avoid wrapping)
    // Stored in PROGMEM to save ~512 bytes of RAM
    static const uint8_t perm[512] PROGMEM = {
        151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
        8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,
        35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
        134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,
        55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,
        18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,
        250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,
        189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,
        172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,
        228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,
        107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
        138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
        // Repeat for easy wrapping
        151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
        8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,
        35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
        134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,
        55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,
        18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,
        250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,
        189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,
        172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,
        228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,
        107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
        138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
    };

    // Gradient vectors for 2D simplex noise
    static const int8_t grad2[8][2] = {
        {1,1}, {-1,1}, {1,-1}, {-1,-1},
        {1,0}, {-1,0}, {0,1}, {0,-1}
    };

    inline float dot2(const int8_t* g, float x, float y) {
        return g[0]*x + g[1]*y;
    }

    // Fast floor function
    inline int fastFloor(float x) {
        return x > 0 ? (int)x : (int)x - 1;
    }

    // 2D Simplex noise function
    // Returns value in range [-1, 1]
    float simplex2D(float x, float y) {
        const float F2 = 0.366025403f;  // (sqrt(3) - 1) / 2
        const float G2 = 0.211324865f;  // (3 - sqrt(3)) / 6

        // Skew input space to determine which simplex cell we're in
        float s = (x + y) * F2;
        int i = fastFloor(x + s);
        int j = fastFloor(y + s);

        // Unskew back to (x,y) space
        float t = (i + j) * G2;
        float X0 = i - t;
        float Y0 = j - t;
        float x0 = x - X0;
        float y0 = y - Y0;

        // Determine which simplex we're in (upper or lower triangle)
        int i1, j1;
        if (x0 > y0) { i1 = 1; j1 = 0; }  // Lower triangle
        else { i1 = 0; j1 = 1; }           // Upper triangle

        // Offsets for corners
        float x1 = x0 - i1 + G2;
        float y1 = y0 - j1 + G2;
        float x2 = x0 - 1.0f + 2.0f * G2;
        float y2 = y0 - 1.0f + 2.0f * G2;

        // Hash coordinates to get gradient indices
        // Use pgm_read_byte() to read from PROGMEM
        int ii = i & 255;
        int jj = j & 255;
        int gi0 = pgm_read_byte(&perm[ii + pgm_read_byte(&perm[jj])]) & 7;
        int gi1 = pgm_read_byte(&perm[ii + i1 + pgm_read_byte(&perm[jj + j1])]) & 7;
        int gi2 = pgm_read_byte(&perm[ii + 1 + pgm_read_byte(&perm[jj + 1])]) & 7;

        // Calculate contributions from three corners
        float n0, n1, n2;

        float t0 = 0.5f - x0*x0 - y0*y0;
        if (t0 < 0) n0 = 0.0f;
        else {
            t0 *= t0;
            n0 = t0 * t0 * dot2(grad2[gi0], x0, y0);
        }

        float t1 = 0.5f - x1*x1 - y1*y1;
        if (t1 < 0) n1 = 0.0f;
        else {
            t1 *= t1;
            n1 = t1 * t1 * dot2(grad2[gi1], x1, y1);
        }

        float t2 = 0.5f - x2*x2 - y2*y2;
        if (t2 < 0) n2 = 0.0f;
        else {
            t2 *= t2;
            n2 = t2 * t2 * dot2(grad2[gi2], x2, y2);
        }

        // Scale to [-1, 1]
        return 70.0f * (n0 + n1 + n2);
    }

    // Fractional Brownian Motion - layered simplex noise for more organic look
    float fbmSimplex2D(float x, float y, int octaves, float persistence = 0.5f) {
        float total = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float maxValue = 0.0f;

        for (int i = 0; i < octaves; i++) {
            total += simplex2D(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }

        return total / maxValue;  // Normalize to [-1, 1]
    }
}

// ============================================================================
// Fire Implementation
// ============================================================================

Fire::Fire()
    : heat_(nullptr), tempHeat_(nullptr), audioEnergy_(0.0f), audioHit_(0.0f),
      lastBurstMs_(0), inSuppression_(false),
      emberNoisePhase_(0.0f), sparkPositions_(nullptr), numActivePositions_(0) {
}

Fire::~Fire() {
    if (heat_) {
        delete[] heat_;
        heat_ = nullptr;
    }
    if (tempHeat_) {
        delete[] tempHeat_;
        tempHeat_ = nullptr;
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
    if (!heat_) {
        Serial.println(F("ERROR: Failed to allocate heat buffer"));
        return false;
    }
    memset(heat_, 0, this->numLeds_);

    // Allocate spark positions for random layout
    if (sparkPositions_) delete[] sparkPositions_;
    sparkPositions_ = new uint8_t[params_.maxSparkPositions];
    if (!sparkPositions_) {
        Serial.println(F("ERROR: Failed to allocate spark positions"));
        delete[] heat_;
        heat_ = nullptr;
        return false;
    }
    memset(sparkPositions_, 0, params_.maxSparkPositions);
    numActivePositions_ = 0;

    // Allocate temp buffer for heat propagation (avoids heap fragmentation)
    if (tempHeat_) delete[] tempHeat_;
    tempHeat_ = new uint8_t[this->numLeds_];
    if (!tempHeat_) {
        Serial.println(F("ERROR: Failed to allocate temp heat buffer"));
        delete[] heat_;
        delete[] sparkPositions_;
        heat_ = nullptr;
        sparkPositions_ = nullptr;
        return false;
    }

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

void Fire::setAudioInput(const float energy, const float hit) {
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
    // Heat propagation direction depends on orientation
    // VERTICAL: y=0 is physical top, so fire rises from high y to low y
    // HORIZONTAL: y=0 is physical bottom, so fire rises from low y to high y

    // Heat propagation weights - weighted average of nearby cells
    // Overflow safety: heat_ values are uint8_t (0-255), weights are small constants
    // Max calculation: (255 * 1) + (255 * 2) = 765, well within uint16_t range (65535)
    // After division and averaging: result stays <= 255, safe to cast back to uint8_t
    constexpr uint16_t HEAT_WEIGHT_NEAR = 1;  // Weight for adjacent cell
    constexpr uint16_t HEAT_WEIGHT_FAR = 2;   // Weight for cell 2 away (stronger influence)
    constexpr uint16_t HEAT_WEIGHT_TOTAL = HEAT_WEIGHT_NEAR + HEAT_WEIGHT_FAR;

    if (orientation_ == VERTICAL) {
        // Fire rises from bottom (high y) to top (low y)
        for (int x = 0; x < this->width_; x++) {
            for (int y = 0; y < this->height_ - 2; y++) {
                int currentIndex = coordsToIndex(x, y);
                int belowIndex = coordsToIndex(x, y + 1);
                int below2Index = coordsToIndex(x, y + 2);

                if (currentIndex >= 0 && belowIndex >= 0 && below2Index >= 0) {
                    // FIX: Use uint16_t arithmetic to prevent overflow
                    uint16_t newHeat = ((uint16_t)heat_[belowIndex] * HEAT_WEIGHT_NEAR +
                                        (uint16_t)heat_[below2Index] * HEAT_WEIGHT_FAR) / HEAT_WEIGHT_TOTAL;

                    // Add horizontal spread (prevent overflow with uint16_t)
                    if (x > 0) {
                        int leftIndex = coordsToIndex(x - 1, y + 1);
                        if (leftIndex >= 0) {
                            newHeat = ((uint16_t)newHeat + (uint16_t)heat_[leftIndex]) / 2;
                        }
                    }
                    if (x < this->width_ - 1) {
                        int rightIndex = coordsToIndex(x + 1, y + 1);
                        if (rightIndex >= 0) {
                            newHeat = ((uint16_t)newHeat + (uint16_t)heat_[rightIndex]) / 2;
                        }
                    }

                    heat_[currentIndex] = min(255, newHeat);
                }
            }
        }
    } else {
        // HORIZONTAL: Traditional upward heat propagation (low y to high y)
        for (int x = 0; x < this->width_; x++) {
            for (int y = this->height_ - 1; y >= 2; y--) {
                int currentIndex = coordsToIndex(x, y);
                int belowIndex = coordsToIndex(x, y - 1);
                int below2Index = coordsToIndex(x, y - 2);

                if (currentIndex >= 0 && belowIndex >= 0 && below2Index >= 0) {
                    // FIX: Use uint16_t arithmetic to prevent overflow
                    uint16_t newHeat = ((uint16_t)heat_[belowIndex] * HEAT_WEIGHT_NEAR +
                                        (uint16_t)heat_[below2Index] * HEAT_WEIGHT_FAR) / HEAT_WEIGHT_TOTAL;

                    // Add horizontal spread (prevent overflow with uint16_t)
                    if (x > 0) {
                        int leftIndex = coordsToIndex(x - 1, y - 1);
                        if (leftIndex >= 0) {
                            newHeat = ((uint16_t)newHeat + (uint16_t)heat_[leftIndex]) / 2;
                        }
                    }
                    if (x < this->width_ - 1) {
                        int rightIndex = coordsToIndex(x + 1, y - 1);
                        if (rightIndex >= 0) {
                            newHeat = ((uint16_t)newHeat + (uint16_t)heat_[rightIndex]) / 2;
                        }
                    }

                    heat_[currentIndex] = min(255, newHeat);
                }
            }
        }
    }
}

void Fire::updateLinearFire() {
    // Lateral heat propagation for linear arrangements
    // Use pre-allocated tempHeat_ to avoid heap fragmentation
    memset(tempHeat_, 0, this->numLeds_);  // Start fresh - heat must spread or decay

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
            tempHeat_[i] = max(tempHeat_[i], decayedHeat);

            // Smaller spread for tighter sparks (only spread high heat)
            if (heat_[i] > 60) {
                int spreadDist = min((int)params_.spreadDistance, 4);  // Tighter spread
                for (int spread = 1; spread <= spreadDist; spread++) {
                    float falloff = 1.0f / (spread + 2);  // Steeper falloff
                    uint8_t heatToSpread = decayedHeat * falloff;

                    if (i - spread >= 0) {
                        tempHeat_[i - spread] = max(tempHeat_[i - spread], heatToSpread);
                    }
                    if (i + spread < this->numLeds_) {
                        tempHeat_[i + spread] = max(tempHeat_[i + spread], heatToSpread);
                    }
                }
            }
        }
    }

    memcpy(heat_, tempHeat_, this->numLeds_);
}

void Fire::updateRandomFire() {
    // Omnidirectional heat propagation for random/scattered layouts
    // Use pre-allocated tempHeat_ to avoid heap fragmentation
    memcpy(tempHeat_, heat_, this->numLeds_);

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

                        tempHeat_[targetIndex] = min(255, tempHeat_[targetIndex] + heatToSpread);
                    }
                }
            }
        }
    }

    memcpy(heat_, tempHeat_, this->numLeds_);
}

void Fire::generateSparks() {
    uint32_t now = millis();
    int numSparks = 0;
    uint8_t sparkHeat = params_.sparkHeatMax;

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

    // BURST: Percussion impulse triggers burst (only if not suppressed)
    // audioHit_ is 0.0 normally, non-zero when percussion detected (kick/snare/hihat)
    if (audioHit_ > 0.0f && !inSuppression_) {
        float strength = audioHit_;  // Use percussion strength (0.0-1.0+)
        numSparks += params_.burstSparks;
        // Scale heat by strength: weak percussion = less intense, strong = max
        sparkHeat = params_.sparkHeatMin +
            (uint8_t)(strength * (255 - params_.sparkHeatMin));
        lastBurstMs_ = now;
        inSuppression_ = true;  // Suppress further bursts briefly
    }

    // Generate the sparks
    for (int s = 0; s < numSparks; s++) {
        int sparkPosition;
        switch (this->layout_) {
            case MATRIX_LAYOUT: {
                // Generate spark at random x position in bottom row(s)
                int x = random(this->width_);
                int y;
                if (orientation_ == VERTICAL) {
                    // VERTICAL: physical bottom is high y values
                    y = this->height_ - 1 - random(params_.bottomRowsForSparks);
                } else {
                    // HORIZONTAL: physical bottom is low y values
                    y = random(params_.bottomRowsForSparks);
                }
                sparkPosition = coordsToIndex(x, y);
                break;
            }
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

    // Noise scale controls the "size" of ember patches
    // Smaller values = larger patches, larger values = more detail
    const float noiseScale = 0.25f;

    // Apply simplex noise-based ember glow to all LEDs
    for (int i = 0; i < this->numLeds_; i++) {
        // Convert linear index to 2D coordinates for better spatial coherence
        int x, y;
        indexToCoords(i, x, y);

        // Use 2D simplex noise with time dimension for animation
        // FBM (Fractional Brownian Motion) with 2 octaves for organic look
        float noise = fbmSimplex2D(
            x * noiseScale,
            y * noiseScale + emberNoisePhase_,
            2,      // octaves
            0.5f    // persistence
        );

        // Normalize from [-1, 1] to [0, 1]
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
void Fire::setBaseCooling(const uint8_t cooling) {
    params_.baseCooling = cooling;
}

void Fire::setSparkParams(const uint8_t heatMin, const uint8_t heatMax, const float chance) {
    params_.sparkHeatMin = heatMin;
    params_.sparkHeatMax = heatMax;
    params_.sparkChance = chance;
}

void Fire::setAudioParams(const float sparkBoost, const uint8_t heatBoostMax, const int8_t coolingBias) {
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
