#include "Lightning.h"
#include <Arduino.h>

Lightning::Lightning() : params_() {}

bool Lightning::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;

    // Configure forces for lightning behavior (no forces - instant bolts)
    gravityForce_.setGravity(0.0f);
    dragForce_.setDrag(1.0f);  // No drag

    return true;
}

void Lightning::spawnParticles(float dt) {
    float spawnProb = params_.baseSpawnChance;
    uint8_t boltCount = 0;

    if (audio_.hasRhythm()) {
        // MUSIC MODE: Beat-synced bolt generation
        if (audio_.pulse > 0.3f) {
            float beatBoost = audio_.phaseToPulse();
            spawnProb += params_.audioSpawnBoost * audio_.pulse * beatBoost;
        }

        // Burst on beat
        if (beatHappened()) {
            boltCount = 3;
        }
    } else {
        // ORGANIC MODE: Transient-reactive
        if (audio_.pulse > params_.organicTransientMin) {
            spawnProb += params_.audioSpawnBoost * audio_.pulse;
            boltCount = 2;
        }
    }

    // Random baseline spawning
    if (random(1000) < spawnProb * 1000) {
        boltCount++;
    }

    // Spawn bolts from random positions with random directions
    for (uint8_t i = 0; i < boltCount && !pool_.isFull(); i++) {
        float x = random(width_ * 100) / 100.0f;
        float y = random(height_ * 100) / 100.0f;

        // Random direction
        float angle = random(360) * DEG_TO_RAD;
        float speed = params_.boltVelocityMin +
                     random(100) * (params_.boltVelocityMax - params_.boltVelocityMin) / 100.0f;

        float vx = cos(angle) * speed;
        float vy = sin(angle) * speed;

        uint8_t intensity = random(params_.intensityMin, params_.intensityMax + 1);

        // Add phase-modulated intensity boost when rhythm is detected
        if (audio_.hasRhythm()) {
            // Bolts are brightest on-beat, dimmer off-beat
            float phaseMod = audio_.phaseToPulse();  // 1.0 at phase=0, 0.0 at phase=0.5
            float intensityScale = 0.6f + 0.4f * phaseMod;  // 60% to 100%
            intensity = (uint8_t)(intensity * intensityScale);
        }

        pool_.spawn(x, y, vx, vy, intensity, params_.defaultLifespan, 1.0f,
                   ParticleFlags::BRANCH);  // Manual fade in updateParticle(), not auto-fade
    }
}

void Lightning::updateParticle(Particle* p, float dt) {
    // Branching logic (only branch once, when particle is young)
    if (p->hasFlag(ParticleFlags::BRANCH) && p->age > 2 && p->age < 8) {
        if (random(100) < params_.branchChance && !pool_.isFull()) {
            spawnBranch(p);
            p->clearFlag(ParticleFlags::BRANCH);  // Only branch once
        }
    }

    // Manual fast fade (faster than age-based fade)
    if (p->intensity > params_.fadeRate) {
        p->intensity -= params_.fadeRate;
    } else {
        p->intensity = 0;
    }
}

void Lightning::renderParticle(const Particle* p, PixelMatrix& matrix) {
    int x = (int)p->x;
    int y = (int)p->y;

    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        uint32_t color = particleColor(p->intensity);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        // Max blending (lightning overwrites)
        RGB existing = matrix.getPixel(x, y);
        matrix.setPixel(x, y,
                       max(existing.r, r),
                       max(existing.g, g),
                       max(existing.b, b));
    }
}

uint32_t Lightning::particleColor(uint8_t intensity) const {
    // Use Lightning palette
    return Palette::LIGHTNING.toColor(intensity);
}

void Lightning::spawnBranch(const Particle* parent) {
    uint8_t branches = min(params_.branchCount,
                          (uint8_t)(pool_.getCapacity() - pool_.getActiveCount()));

    // Calculate parent angle
    float parentAngle = atan2(parent->vy, parent->vx);

    for (uint8_t i = 0; i < branches; i++) {
        // Calculate branch angle (perpendicular to parent with variation)
        float branchAngle = parentAngle +
                           (random(200) - 100) * params_.branchAngleSpread / 100.0f;

        // Branch velocity (70% of parent speed)
        float speed = sqrt(parent->vx * parent->vx + parent->vy * parent->vy) * 0.7f;
        float vx = cos(branchAngle) * speed;
        float vy = sin(branchAngle) * speed;

        // Reduced intensity
        uint8_t intensity = parent->intensity * (100 - params_.branchIntensityLoss) / 100;

        // Branches don't branch (no BRANCH flag) and use manual fade
        pool_.spawn(parent->x, parent->y, vx, vy, intensity,
                   params_.defaultLifespan / 2, 1.0f,
                   0);  // No flags - manual fade in updateParticle()
    }
}
