#include "HeatFire.h"
#include <Arduino.h>

HeatFire::HeatFire()
    : params_(), audio_(), noiseTime_(0.0f), prevPhase_(1.0f), beatCount_(0),
      downbeatSpreadMult_(1.0f), downbeatColorShift_(0.0f),
      downbeatCoolSuppress_(0.0f), spawnBias_(0.0f),
      paletteBias_(0.0f), effectiveCooling_(0.0f), effectiveSpread_(0.0f) {
    memset(heat_, 0, sizeof(heat_));
}

bool HeatFire::begin(const DeviceConfig& config) {
    width_ = config.matrix.width;
    height_ = config.matrix.height;
    numLeds_ = width_ * height_;
    layout_ = config.matrix.layoutType;
    orientation_ = config.matrix.orientation;
    computeDimensionScales();
    lastUpdateMs_ = millis();

    if (numLeds_ > MAX_HEAT_CELLS) return false;

    memset(heat_, 0, sizeof(heat_));

    // Initialize bottom row to max heat (fire source)
    if (layout_ != LINEAR_LAYOUT) {
        int bottomY = height_ - 1;
        for (int x = 0; x < width_; x++) {
            heat_[bottomY * width_ + x] = NCOLORS - 1;
        }
    }

    return true;
}

void HeatFire::generate(PixelMatrix& matrix, const AudioControl& audio) {
    audio_ = audio;

    uint32_t currentMs = millis();
    float dt = (currentMs - lastUpdateMs_) / 1000.0f;
    lastUpdateMs_ = currentMs;
    dt = min(dt, 0.05f);

    // Advance noise time
    float densityNorm = min(1.0f, audio.onsetDensity / 6.0f);
    float organicSpeed = params_.noiseSpeed * (1.0f + densityNorm);
    float musicSpeed = params_.noiseSpeed * (2.0f + densityNorm);
    noiseTime_ += organicSpeed * (1.0f - audio.rhythmStrength) + musicSpeed * audio.rhythmStrength;

    // Smooth palette bias
    float targetBias = audio.energy * audio.rhythmStrength;
    paletteBias_ += (targetBias - paletteBias_) * min(1.0f, 2.0f * dt);

    // Decay downbeat transient state
    downbeatSpreadMult_ = max(1.0f, downbeatSpreadMult_ - 3.0f * dt);
    downbeatColorShift_ = max(0.0f, downbeatColorShift_ - 2.0f * dt);
    downbeatCoolSuppress_ = max(0.0f, downbeatCoolSuppress_ - 2.0f * dt);
    spawnBias_ *= max(0.0f, 1.0f - dt * 3.0f);

    // Beat detection
    if (beatHappened() && audio.rhythmStrength > 0.3f) {
        beatCount_++;
        if (audio.downbeat > 0.5f) {
            downbeatSpreadMult_ = 2.5f;
            downbeatColorShift_ = 1.0f;
            downbeatCoolSuppress_ = 1.0f;
        }
        if (audio.beatInMeasure > 0 && audio.rhythmStrength > 0.5f) {
            spawnBias_ = (audio.beatInMeasure % 2 == 1) ? -0.25f : 0.25f;
        }
    }
    prevPhase_ = audio.phase;

    if (layout_ == LINEAR_LAYOUT) {
        propagateHeat1D();
        injectHeat(audio);
    } else {
        propagateHeat2D();
        injectHeat(audio);
        updateFlares(audio);
    }
    renderHeat(matrix);
}

void HeatFire::reset() {
    memset(heat_, 0, sizeof(heat_));
    nflare_ = 0;
    noiseTime_ = 0.0f;
    prevPhase_ = 1.0f;
    beatCount_ = 0;
    downbeatSpreadMult_ = 1.0f;
    downbeatColorShift_ = 0.0f;
    downbeatCoolSuppress_ = 0.0f;
    spawnBias_ = 0.0f;
    paletteBias_ = 0.0f;
    audio_ = AudioControl();

    // Re-initialize bottom row
    if (layout_ != LINEAR_LAYOUT) {
        int bottomY = height_ - 1;
        for (int x = 0; x < width_; x++) {
            heat_[bottomY * width_ + x] = NCOLORS - 1;
        }
    }
}

// ============================================================================
// MatrixFireFast-style algorithm (credit: Patrick Rigney / toggledbits)
// ============================================================================

void HeatFire::propagateHeat2D() {
    // Step 1: Shift heat UP one row and subtract 1 (simple decay).
    // Process from top to bottom so we read unmodified data below.
    for (int y = 0; y < height_ - 1; y++) {
        for (int x = 0; x < width_; x++) {
            uint8_t below = heat_[(y + 1) * width_ + x];
            heat_[y * width_ + x] = (below > 0) ? below - 1 : 0;
        }
    }

    // Step 2: Bottom row — if cell has heat, randomize to medium-high value.
    // This maintains the fire base with natural flickering.
    int bottomY = height_ - 1;
    for (int x = 0; x < width_; x++) {
        uint8_t h = heat_[bottomY * width_ + x];
        if (h > 0) {
            heat_[bottomY * width_ + x] = (uint8_t)random(NCOLORS - 6, NCOLORS - 1);
        }
    }
}

void HeatFire::propagateHeat1D() {
    // 1D: shift and decay, with random source positions
    uint8_t temp[MAX_HEAT_CELLS];
    for (int x = 0; x < width_; x++) {
        int drift = random(0, 3) - 1;
        int srcX = constrain(x + drift, 0, width_ - 1);
        uint8_t srcHeat = heat_[srcX];
        temp[x] = (srcHeat > 0) ? srcHeat - 1 : 0;
    }
    memcpy(heat_, temp, width_);
}

// ============================================================================
// Flare system — this is what creates the flame tongues
// ============================================================================

void HeatFire::updateFlares(const AudioControl& audio) {
    // Update existing flares: re-apply glow at reduced intensity, remove dead ones
    int i = 0;
    while (i < nflare_) {
        int x = flares_[i] & 0xFF;
        int y = (flares_[i] >> 8) & 0xFF;
        int z = (flares_[i] >> 16) & 0xFF;

        applyGlow(x, y, z);

        if (z > 1) {
            flares_[i] = (flares_[i] & 0xFFFF) | ((z - 1) << 16);
            i++;
        } else {
            // Flare is dead — remove by shifting
            for (int j = i + 1; j < nflare_; j++) {
                flares_[j - 1] = flares_[j];
            }
            nflare_--;
        }
    }

    // Spawn new flares — audio controls frequency and intensity
    float phasePulse = audio.phaseToPulse();

    // Flare chance: organic vs music blend
    float organicChance = params_.baseHeat * 40.0f;  // ~20-40% base
    float musicChance = params_.baseHeat * 30.0f * ((1.0f - params_.musicBeatDepth) + params_.musicBeatDepth * phasePulse)
                      + params_.audioHeatBoost * audio.energy * 30.0f;
    float flareChance = organicChance * (1.0f - audio.rhythmStrength)
                      + musicChance * audio.rhythmStrength;

    // Transient burst: extra flares
    if (audio.pulse > params_.organicTransientMin) {
        float burstStrength = (audio.pulse - params_.organicTransientMin) /
                              (1.0f - params_.organicTransientMin);
        flareChance += params_.burstHeat * burstStrength * 60.0f;
    }

    // Beat burst
    if (beatHappened() && audio.rhythmStrength > 0.3f) {
        flareChance += params_.burstHeat * audio.rhythmStrength * 40.0f;

        // Downbeat: max flare burst
        if (audio.downbeat > 0.5f) {
            flareChance += params_.burstHeat * audio.downbeat * 50.0f;
        }
    }

    // Max simultaneous flares scales with display width
    int maxFlares = min((int)MAX_FLARES, max(4, width_ / 4));

    if (nflare_ < maxFlares && random(1, 101) <= (int)flareChance) {
        // Spawn position: random x with beat-rocking bias
        int x = random(0, width_);
        float normalizedX = (float)x / max(1, width_ - 1) - 0.5f;
        float biasMod = 1.0f + spawnBias_ * normalizedX * 4.0f;
        if (biasMod > 0.2f) {
            int flareRows = max(1, height_ / 5);  // Bottom ~20% for flare sources
            int y = height_ - 1 - random(0, flareRows);
            int z = NCOLORS - 1;  // Start at max intensity

            flares_[nflare_++] = (z << 16) | (y << 8) | (x & 0xFF);
            applyGlow(x, y, z);
        }
    }
}

void HeatFire::applyGlow(int x, int y, int z) {
    // Radial glow around flare position — this creates the tongue shapes.
    // Heat radiates outward, falling off with distance.
    int radius = z * 10 / FLARE_DECAY + 1;
    for (int dy = -radius; dy < radius; dy++) {
        for (int dx = -radius; dx < radius; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < width_ && py >= 0 && py < height_) {
                // Distance-based falloff
                int dist = isqrt(dx * dx + dy * dy);
                int decayed = (FLARE_DECAY * dist + 5) / 10;
                uint8_t n = (z > decayed) ? z - decayed : 0;
                int idx = py * width_ + px;
                if (n > heat_[idx]) {
                    heat_[idx] = n;
                }
            }
        }
    }
}

uint32_t HeatFire::isqrt(uint32_t n) {
    if (n < 2) return n;
    uint32_t small = isqrt(n >> 2) << 1;
    uint32_t large = small + 1;
    return (large * large > n) ? small : large;
}

// ============================================================================
// Heat injection (bottom row maintenance + audio)
// ============================================================================

void HeatFire::injectHeat(const AudioControl& audio) {
    if (layout_ == LINEAR_LAYOUT) {
        // 1D: inject at 2-4 source positions
        float heatLevel = params_.baseHeat + params_.audioHeatBoost * audio.energy;
        int numSources = 2 + (int)(audio.energy * 2.0f);
        for (int i = 0; i < numSources; i++) {
            float noisePos = SimplexNoise::noise3D_01(i * 3.7f, noiseTime_ * 0.3f, 0.0f);
            int srcX = (int)(noisePos * (width_ - 1));
            srcX = constrain(srcX + (int)(spawnBias_ * width_ * 0.25f), 0, width_ - 1);
            for (int dx = -1; dx <= 1; dx++) {
                int xx = constrain(srcX + dx, 0, width_ - 1);
                uint8_t val = (dx == 0) ? NCOLORS - 1 : NCOLORS - 3;
                if (val > heat_[xx]) heat_[xx] = val;
            }
        }
    }
    // 2D injection is handled by the flare system + bottom row maintenance in propagateHeat2D
}

// ============================================================================
// Rendering
// ============================================================================

void HeatFire::renderHeat(PixelMatrix& matrix) {
    if (layout_ == LINEAR_LAYOUT) {
        for (int x = 0; x < width_; x++) {
            uint32_t color = heatToColor(heat_[x]);
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;
            matrix.setPixel(x, 0, r, g, b);
        }
    } else {
        for (int y = 0; y < height_; y++) {
            for (int x = 0; x < width_; x++) {
                uint32_t color = heatToColor(heat_[y * width_ + x]);
                uint8_t r = (color >> 16) & 0xFF;
                uint8_t g = (color >> 8) & 0xFF;
                uint8_t b = color & 0xFF;
                matrix.setPixel(x, y, r, g, b);
            }
        }
    }
}

uint32_t HeatFire::heatToColor(uint8_t heat) const {
    if (heat == 0) return 0;  // Black — most cells should be black

    // Map heat (0-NCOLORS) through fire color palette
    // Use a fixed 11-stop palette (matching MatrixFireFast) for authentic fire look
    static const uint32_t fireColors[] = {
        0x000000,  // 0: black
        0x100000,  // 1: very dark red
        0x300000,  // 2: dark red
        0x600000,  // 3: medium red
        0x800000,  // 4: red
        0xA00000,  // 5: bright red
        0xC02000,  // 6: red-orange
        0xC04000,  // 7: orange
        0xC06000,  // 8: yellow-orange
        0xC08000,  // 9: yellow
        0x807080   // 10: white/hot
    };

    uint8_t idx = min((int)heat, NCOLORS - 1);
    uint32_t baseColor = fireColors[idx];
    uint8_t r = (baseColor >> 16) & 0xFF;
    uint8_t g = (baseColor >> 8) & 0xFF;
    uint8_t b = baseColor & 0xFF;

    // Downbeat color temperature shift
    if (downbeatColorShift_ > 0.0f) {
        float s = downbeatColorShift_;
        r = (uint8_t)min(255, (int)r + (int)(s * 30));
        g = (uint8_t)min(255, (int)g + (int)(s * 40));
        b = (uint8_t)min(255, (int)b + (int)(s * 60));
    }

    // Master brightness scale
    float br = params_.brightness;
    r = (uint8_t)(r * br);
    g = (uint8_t)(g * br);
    b = (uint8_t)(b * br);

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
