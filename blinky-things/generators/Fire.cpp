#include "Fire.h"
#include <Arduino.h>

Fire::Fire() : heat_(nullptr), params_(), beatCount_(0) {}

Fire::~Fire() {
    if (heat_) {
        delete[] heat_;
        heat_ = nullptr;
    }
}

bool Fire::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;

    // Allocate heat buffer (same as current Fire)
    heat_ = new uint8_t[numLeds_];
    if (!heat_) return false;
    memset(heat_, 0, numLeds_);

    // Configure forces for fire behavior
    gravityForce_.setGravity(params_.gravity);
    windForce_.setWind(params_.windBase, params_.windVariation);
    dragForce_.setDrag(params_.drag);

    beatCount_ = 0;

    return true;
}

void Fire::generate(PixelMatrix& matrix, const AudioControl& audio) {
    // First, apply cooling to heat buffer
    applyCooling();

    // Run particle system (spawns, updates, renders particles)
    ParticleGenerator::generate(matrix, audio);

    // Blend heat buffer with particle rendering
    blendHeatToMatrix(matrix);
}

void Fire::reset() {
    ParticleGenerator::reset();
    if (heat_) {
        memset(heat_, 0, numLeds_);
    }
    beatCount_ = 0;
}

void Fire::spawnParticles(float dt) {
    float spawnProb = params_.baseSpawnChance;
    uint8_t sparkCount = 0;

    if (audio_.hasRhythm()) {
        // MUSIC MODE: Beat-synced spawning
        float phaseMod = audio_.phaseToPulse();
        spawnProb += params_.audioSpawnBoost * audio_.pulse * phaseMod;

        // Detect beat and track count
        if (beatHappened()) {
            beatCount_++;
            // Stronger bursts on downbeats (every 4 beats)
            uint8_t baseSparks = (beatCount_ % 4 == 0) ? params_.burstSparks * 2 : params_.burstSparks;
            // Scale by rhythm confidence
            sparkCount = (uint8_t)(baseSparks * (0.5f + 0.5f * audio_.rhythmStrength));
        }
    } else {
        // ORGANIC MODE: Transient-reactive
        if (audio_.pulse > params_.organicTransientMin) {
            spawnProb += params_.audioSpawnBoost * audio_.pulse;
            sparkCount = params_.burstSparks / 2;
        }
    }

    // Random baseline spawning
    if (random(1000) < spawnProb * 1000) {
        sparkCount++;
    }

    // Spawn sparks from bottom row
    for (uint8_t i = 0; i < sparkCount && !pool_.isFull(); i++) {
        float x = random(width_ * 100) / 100.0f;
        float y = height_ - 1;  // Bottom of screen

        // Upward velocity with horizontal spread
        float vy = -(params_.sparkVelocityMin +
                    random(100) * (params_.sparkVelocityMax - params_.sparkVelocityMin) / 100.0f);
        float vx = (random(200) - 100) * params_.sparkSpread / 100.0f;

        uint8_t intensity = random(params_.intensityMin, params_.intensityMax + 1);

        pool_.spawn(x, y, vx, vy, intensity, params_.defaultLifespan, 1.0f,
                   ParticleFlags::GRAVITY | ParticleFlags::WIND |
                   ParticleFlags::FADE | ParticleFlags::EMIT_TRAIL);
    }
}

void Fire::updateParticle(Particle* p, float dt) {
    // Emit heat trail if flagged
    if (p->hasFlag(ParticleFlags::EMIT_TRAIL)) {
        int x = (int)p->x;
        int y = (int)p->y;
        int index = coordsToIndex(x, y);

        if (index >= 0 && index < numLeds_) {
            uint8_t trailHeat = p->intensity * params_.trailHeatFactor / 100;
            heat_[index] = min(255, heat_[index] + trailHeat);
        }
    }
}

void Fire::renderParticle(const Particle* p, PixelMatrix& matrix) {
    // Render particle as bright point
    int x = (int)p->x;
    int y = (int)p->y;

    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        uint32_t color = particleColor(p->intensity);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        // Additive blending with existing heat (cast to int to prevent overflow)
        RGB existing = matrix.getPixel(x, y);
        matrix.setPixel(x, y,
                       min(255, (int)existing.r + r),
                       min(255, (int)existing.g + g),
                       min(255, (int)existing.b + b));
    }
}

uint32_t Fire::particleColor(uint8_t intensity) const {
    // Fire palette: black -> red -> orange -> yellow
    if (intensity < 85) {
        // Black to red (cast before multiply to prevent overflow)
        return (((uint32_t)intensity * 3) << 16);
    } else if (intensity < 170) {
        // Red to orange (cast before multiply to prevent overflow)
        uint8_t green = (uint8_t)(((uint32_t)(intensity - 85)) * 3);
        return (0xFF0000 | ((uint32_t)green << 8));
    } else {
        // Orange to yellow
        return 0xFFFF00;
    }
}

void Fire::applyCooling() {
    uint8_t cooling = params_.trailDecay;

    // Audio-reactive cooling (breathing effect in music mode)
    if (audio_.hasRhythm()) {
        float breathe = -cos(audio_.phase * TWO_PI);
        int8_t coolingMod = (int8_t)(breathe * 15.0f);
        cooling = max(0, min(255, (int)cooling + coolingMod));
    }

    // Apply cooling to all heat values
    for (int i = 0; i < numLeds_; i++) {
        uint8_t coolAmount = random(0, cooling + 1);
        heat_[i] = (heat_[i] > coolAmount) ? heat_[i] - coolAmount : 0;
    }
}

void Fire::blendHeatToMatrix(PixelMatrix& matrix) {
    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            int index = coordsToIndex(x, y);
            if (index >= 0) {
                uint32_t color = particleColor(heat_[index]);
                uint8_t r = (color >> 16) & 0xFF;
                uint8_t g = (color >> 8) & 0xFF;
                uint8_t b = color & 0xFF;

                // Max blend with particle rendering
                RGB existing = matrix.getPixel(x, y);
                matrix.setPixel(x, y,
                               max(existing.r, r),
                               max(existing.g, g),
                               max(existing.b, b));
            }
        }
    }
}
