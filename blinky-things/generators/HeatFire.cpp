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

    // Validate heat buffer fits
    if (numLeds_ > MAX_HEAT_CELLS) return false;

    memset(heat_, 0, sizeof(heat_));
    return true;
}

void HeatFire::generate(PixelMatrix& matrix, const AudioControl& audio) {
    audio_ = audio;

    // Delta time
    uint32_t currentMs = millis();
    float dt = (currentMs - lastUpdateMs_) / 1000.0f;
    lastUpdateMs_ = currentMs;
    dt = min(dt, 0.05f);  // Cap at 50ms to prevent huge steps after pauses

    // Advance noise time (energy + density drive speed)
    float densityNorm = min(1.0f, audio.onsetDensity / 6.0f);
    float organicSpeed = params_.noiseSpeed * (1.0f + densityNorm);
    float musicSpeed = params_.noiseSpeed * (2.0f + densityNorm);
    noiseTime_ += organicSpeed * (1.0f - audio.rhythmStrength) + musicSpeed * audio.rhythmStrength;

    // Smooth palette bias toward energy*rhythm (~0.5s time constant)
    float targetBias = audio.energy * audio.rhythmStrength;
    paletteBias_ += (targetBias - paletteBias_) * min(1.0f, 2.0f * dt);

    // Decay downbeat transient state
    downbeatSpreadMult_ = max(1.0f, downbeatSpreadMult_ - 3.0f * dt);
    downbeatColorShift_ = max(0.0f, downbeatColorShift_ - 2.0f * dt);
    downbeatCoolSuppress_ = max(0.0f, downbeatCoolSuppress_ - 2.0f * dt);
    spawnBias_ *= max(0.0f, 1.0f - dt * 3.0f);

    // Beat detection for downbeat/beatInMeasure effects
    if (beatHappened() && audio.rhythmStrength > 0.3f) {
        beatCount_++;

        if (audio.downbeat > 0.5f) {
            downbeatSpreadMult_ = 2.5f;
            downbeatColorShift_ = 1.0f;
            downbeatCoolSuppress_ = 1.0f;
        }

        // Left/right injection bias
        if (audio.beatInMeasure > 0 && audio.rhythmStrength > 0.5f) {
            spawnBias_ = (audio.beatInMeasure % 2 == 1) ? -0.25f : 0.25f;
        }
    }
    prevPhase_ = audio.phase;

    // Compute per-frame effective parameters
    // Cooling: lower when energy is high (taller flames), higher when onset density is high (jittery)
    float phasePulse = audio.phaseToPulse();
    float coolingEnergyMod = 1.0f - 0.4f * audio.energy;  // Louder = less cooling = taller
    float coolingDensityMod = 1.0f + 0.3f * densityNorm;  // More transients = more cooling = jittery
    float coolingPhaseMod = 1.0f;
    if (audio.rhythmStrength > 0.3f) {
        // Less cooling on-beat (flames surge), more off-beat (flames recede)
        coolingPhaseMod = 1.2f - 0.4f * phasePulse * audio.rhythmStrength;
    }
    float coolingDownbeatMod = 1.0f - 0.5f * downbeatCoolSuppress_;
    effectiveCooling_ = params_.baseCooling * coolingEnergyMod * coolingDensityMod *
                        coolingPhaseMod * coolingDownbeatMod;

    // Spread: wider on downbeat, modulated by wind
    float windMod = 1.0f;
    if (audio.rhythmStrength > 0.3f) {
        windMod = 0.5f + 0.5f * phasePulse + audio.pulse * params_.windDrift;
    }
    effectiveSpread_ = params_.diffusionSpread * windMod * downbeatSpreadMult_;

    // Propagate heat, inject, render
    if (layout_ == LINEAR_LAYOUT) {
        propagateHeat1D();
    } else {
        propagateHeat2D();
    }
    injectHeat(audio);

    renderHeat(matrix);
}

void HeatFire::reset() {
    memset(heat_, 0, sizeof(heat_));
    noiseTime_ = 0.0f;
    prevPhase_ = 1.0f;
    beatCount_ = 0;
    downbeatSpreadMult_ = 1.0f;
    downbeatColorShift_ = 0.0f;
    downbeatCoolSuppress_ = 0.0f;
    spawnBias_ = 0.0f;
    paletteBias_ = 0.0f;
    audio_ = AudioControl();
}

// ============================================================================
// Heat Propagation
// ============================================================================

void HeatFire::propagateHeat2D() {
    // Pure DOOM PSX fire algorithm:
    // Each cell copies from ONE cell in the row below, with small random
    // lateral drift, and subtracts a small random value (0-1).
    //
    // NO lateral averaging — this is critical. Averaging smears heat sideways
    // and fills the entire width, creating a wall instead of distinct tongues.
    // DOOM creates tongues because each column evolves independently with only
    // tiny random lateral shifts.
    //
    // Heat values: 0-255 (not 0-36 like original DOOM).
    // Cooling: random(0, coolingMax) per cell per row. This controls flame height.

    // Scale cooling by height: on taller displays, each row needs less cooling
    // for the same visual flame height fraction. Audio modulates effectiveCooling_.
    int coolingMax = max(1, (int)(effectiveCooling_ * 600.0f / height_) + 1);

    for (int y = 0; y < height_ - 1; y++) {
        for (int x = 0; x < width_; x++) {
            // Pick ONE source cell from row below with random lateral drift (±1)
            // This is the DOOM formula: dst = src - width - rand(0..2) + 1
            int drift = random(0, 3) - 1;  // -1, 0, or +1
            int srcX = constrain(x + drift, 0, width_ - 1);
            int srcY = y + 1;

            uint8_t srcHeat = heat_[srcY * width_ + srcX];

            // Random cooling: subtract 0 or 1 (DOOM uses randIdx & 1)
            // Scale up for 0-255 range (DOOM uses 0-36)
            uint8_t cooling = (uint8_t)random(0, coolingMax);

            heat_[y * width_ + x] = (srcHeat > cooling) ? srcHeat - cooling : 0;
        }
    }
}

void HeatFire::propagateHeat1D() {
    // 1D DOOM-style: copy from neighbor with drift, subtract cooling
    int coolingMax = max(1, (int)(effectiveCooling_ * 600.0f / width_) + 1);
    uint8_t temp[MAX_HEAT_CELLS];

    for (int x = 0; x < width_; x++) {
        int drift = random(0, 3) - 1;
        int srcX = constrain(x + drift, 0, width_ - 1);
        uint8_t srcHeat = heat_[srcX];
        uint8_t cooling = (uint8_t)random(0, coolingMax);
        temp[x] = (srcHeat > cooling) ? srcHeat - cooling : 0;
    }
    memcpy(heat_, temp, width_);
}

// ============================================================================
// Audio-Driven Heat Injection
// ============================================================================

void HeatFire::injectHeat(const AudioControl& audio) {
    float phasePulse = audio.phaseToPulse();

    // Organic mode: constant base heat + energy-reactive
    float organicHeat = params_.baseHeat + params_.audioHeatBoost * audio.energy;

    // Music mode: phase-modulated injection + energy
    float phaseDepth = params_.musicBeatDepth;
    float musicHeat = params_.baseHeat * ((1.0f - phaseDepth) + phaseDepth * phasePulse)
                    + params_.audioHeatBoost * audio.energy;

    // Blend by rhythmStrength
    float heatLevel = organicHeat * (1.0f - audio.rhythmStrength)
                    + musicHeat * audio.rhythmStrength;

    // Transient burst (pulse-driven, independent of beat)
    if (audio.pulse > params_.organicTransientMin) {
        float burstStrength = (audio.pulse - params_.organicTransientMin) /
                              (1.0f - params_.organicTransientMin);
        heatLevel += params_.burstHeat * burstStrength;
    }

    // Beat burst: extra heat on every predicted beat (not just downbeat)
    if (beatHappened() && audio.rhythmStrength > 0.3f) {
        heatLevel += params_.burstHeat * audio.rhythmStrength * 0.5f;

        // Downbeat: max heat injection
        if (audio.downbeat > 0.5f) {
            heatLevel += params_.burstHeat * audio.downbeat;
        }

        // BeatInMeasure accents
        if (audio.rhythmStrength > 0.5f) {
            float accent = 0.0f;
            switch (audio.beatInMeasure) {
                case 1: accent = 1.0f; break;
                case 3: accent = 0.5f; break;
                case 2: case 4: accent = 0.25f; break;
            }
            heatLevel += params_.burstHeat * accent * audio.rhythmStrength * 0.3f;
        }
    }

    // Clamp to valid heat range
    uint8_t heatValue = (uint8_t)min(255.0f, heatLevel * 255.0f);

    if (layout_ == LINEAR_LAYOUT) {
        // 1D: inject at 2-4 source positions that drift via noise
        int numSources = 2 + (int)(audio.energy * 2.0f);
        for (int i = 0; i < numSources; i++) {
            float noisePos = SimplexNoise::noise3D_01(i * 3.7f, noiseTime_ * 0.3f, 0.0f);
            int srcX = (int)(noisePos * (width_ - 1));
            srcX = constrain(srcX + (int)(spawnBias_ * width_ * 0.25f), 0, width_ - 1);

            for (int dx = -1; dx <= 1; dx++) {
                int xx = constrain(srcX + dx, 0, width_ - 1);
                uint8_t injected = (dx == 0) ? heatValue : heatValue / 2;
                heat_[xx] = max(heat_[xx], injected);
            }
        }
    } else {
        // 2D bottom row: fully populated every frame (DOOM PSX style).
        // Each bottom cell gets a random heat value up to heatValue.
        // The randomness per cell per frame creates the flickering base.
        // Audio modulates heatValue (the max), so louder = brighter base.
        // The propagation's random cooling creates distinct tongues as heat
        // rises — some columns lose heat faster than others.
        int bottomY = height_ - 1;

        for (int x = 0; x < width_; x++) {
            // Left/right injection bias from beatInMeasure rocking
            float normalizedX = (float)x / max(1, width_ - 1) - 0.5f;
            float biasMod = 1.0f + spawnBias_ * normalizedX * 4.0f;
            biasMod = max(0.3f, biasMod);

            // Random heat with audio-driven ceiling
            uint8_t maxH = (uint8_t)min(255.0f, heatValue * biasMod);
            heat_[bottomY * width_ + x] = (uint8_t)random(maxH / 2, max((int)maxH, 1) + 1);
        }
    }
}

// ============================================================================
// Rendering
// ============================================================================

void HeatFire::renderHeat(PixelMatrix& matrix) {
    if (layout_ == LINEAR_LAYOUT) {
        // 1D: render heat directly to LED positions
        for (int x = 0; x < width_; x++) {
            uint32_t color = heatToColor(heat_[x]);
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;
            matrix.setPixel(x, 0, r, g, b);
        }
    } else {
        // 2D: render heat buffer to matrix
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
    // Audio-driven gamma: subtle remap
    float gamma = 1.1f - 0.2f * paletteBias_;
    float normalized = powf(heat / 255.0f, gamma);
    uint8_t remapped = (uint8_t)(normalized * 255.0f);

    // Same dual-palette system as particle Fire
    struct ColorStop { uint8_t position, r, g, b; };

    // Warm palette: campfire
    static const ColorStop warm[] = {
        {0,   0,   0,   0},
        {51,  64,  0,   0},
        {102, 255, 0,   0},
        {153, 255, 128, 0},
        {204, 255, 200, 0},
        {255, 255, 255, 64}
    };

    // Hot palette: intense (stays in warm hues)
    static const ColorStop hot[] = {
        {0,   0,   0,   0},
        {51,  128, 8,   0},
        {102, 255, 80,  0},
        {153, 255, 180, 10},
        {204, 255, 230, 40},
        {255, 255, 255, 100}
    };

    const int paletteSize = 6;

    auto lookup = [&](const ColorStop* pal, uint8_t val, uint8_t& ro, uint8_t& go, uint8_t& bo) {
        int lo = 0, hi = 1;
        for (int i = 0; i < paletteSize - 1; i++) {
            if (val >= pal[i].position && val <= pal[i+1].position) {
                lo = i; hi = i + 1; break;
            }
        }
        float range = pal[hi].position - pal[lo].position;
        float t = (range > 0) ? (float)(val - pal[lo].position) / range : 0.0f;
        ro = (uint8_t)(pal[lo].r + t * (pal[hi].r - pal[lo].r));
        go = (uint8_t)(pal[lo].g + t * (pal[hi].g - pal[lo].g));
        bo = (uint8_t)(pal[lo].b + t * (pal[hi].b - pal[lo].b));
    };

    uint8_t wr, wg, wb, hr, hg, hb;
    lookup(warm, remapped, wr, wg, wb);
    lookup(hot,  remapped, hr, hg, hb);

    // Blend warm/hot (dead zone at 0.4 so warm is default)
    float blend = constrain((paletteBias_ - 0.4f) / 0.5f, 0.0f, 1.0f);
    uint8_t r = (uint8_t)(wr + blend * (hr - wr));
    uint8_t g = (uint8_t)(wg + blend * (hg - wg));
    uint8_t b = (uint8_t)(wb + blend * (hb - wb));

    // Downbeat color temperature shift
    if (downbeatColorShift_ > 0.0f) {
        float s = downbeatColorShift_;
        r = (uint8_t)min(255, (int)r + (int)(s * 40));
        g = (uint8_t)min(255, (int)g + (int)(s * 50));
        b = (uint8_t)min(255, (int)b + (int)(s * 80));
    }

    // Master brightness scale — heat buffer fills every pixel so
    // full-brightness RGB is much brighter than sparse particles
    float br = params_.brightness;
    r = (uint8_t)(r * br);
    g = (uint8_t)(g * br);
    b = (uint8_t)(b * br);

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
