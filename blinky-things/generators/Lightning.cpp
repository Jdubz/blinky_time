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
            boltCount = 2;  // Reduced from 3 - spawn fewer but more coherent bolts
        }
    } else {
        // ORGANIC MODE: Transient-reactive
        if (audio_.pulse > params_.organicTransientMin) {
            spawnProb += params_.audioSpawnBoost * audio_.pulse;
            boltCount = 1;  // Reduced from 2
        }
    }

    // Random baseline spawning
    if (random(1000) < spawnProb * 1000) {
        boltCount++;
    }

    // Spawn coherent lightning bolts as connected particle chains
    for (uint8_t i = 0; i < boltCount && !pool_.isFull(); i++) {
        spawnBolt();
    }
}

/**
 * Spawn a coherent lightning bolt as a connected chain of particles
 * Uses Bresenham's line algorithm to create linear bolt structure
 */
void Lightning::spawnBolt() {
    // Choose random start and end points
    float x0 = random(width_ * 100) / 100.0f;
    float y0 = random(height_ * 100) / 100.0f;
    float x1 = random(width_ * 100) / 100.0f;
    float y1 = random(height_ * 100) / 100.0f;

    // Calculate bolt intensity (brightest on beat)
    uint8_t intensity = random(params_.intensityMin, params_.intensityMax + 1);
    if (audio_.hasRhythm()) {
        float phaseMod = audio_.phaseToPulse();
        float intensityScale = 0.6f + 0.4f * phaseMod;
        intensity = (uint8_t)(intensity * intensityScale);
    }

    // Use Bresenham's line algorithm to create connected particle chain
    int dx = abs((int)x1 - (int)x0);
    int dy = abs((int)y1 - (int)y0);
    int steps = max(dx, dy);

    // Limit bolt length to prevent using entire pool
    steps = min(steps, 12);  // Max 12 particles per bolt

    if (steps == 0) return;  // Degenerate case

    float xStep = (x1 - x0) / steps;
    float yStep = (y1 - y0) / steps;

    // Spawn particles along the line with slight random jitter for organic look
    for (int step = 0; step <= steps && !pool_.isFull(); step++) {
        float x = x0 + xStep * step;
        float y = y0 + yStep * step;

        // Add small random jitter (±0.3 pixels) for organic lightning appearance
        x += (random(60) - 30) / 100.0f;
        y += (random(60) - 30) / 100.0f;

        // All particles in bolt are stationary (vx=0, vy=0) and fade together
        pool_.spawn(x, y, 0.0f, 0.0f, intensity, params_.defaultLifespan, 1.0f,
                   ParticleFlags::BRANCH);  // Can still branch
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

        // MAX BLENDING: Lightning bolts take brightest value (bolt dominance)
        // Preserves the brightest part of overlapping bolts and branches
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
    // Calculate available pool slots with underflow protection
    uint8_t available = pool_.getCapacity() > pool_.getActiveCount()
                        ? pool_.getCapacity() - pool_.getActiveCount()
                        : 0;

    // Spawn short branch lines (3-5 particles per branch)
    uint8_t branchLength = 3 + random(3);  // 3-5 particles
    uint8_t particlesNeeded = branchLength * params_.branchCount;

    if (particlesNeeded > available) {
        return;  // Not enough space for coherent branches
    }

    // Reduced intensity for branches
    uint8_t intensity = parent->intensity * (100 - params_.branchIntensityLoss) / 100;

    for (uint8_t i = 0; i < params_.branchCount; i++) {
        // Random branch direction (perpendicular-ish to main bolt)
        float branchAngle = random(360) * DEG_TO_RAD;

        // Branch extends outward from parent position
        float x0 = parent->x;
        float y0 = parent->y;

        // Calculate end point of branch
        float branchDist = branchLength;
        float x1 = x0 + cos(branchAngle) * branchDist;
        float y1 = y0 + sin(branchAngle) * branchDist;

        // Spawn connected particles along branch line
        for (uint8_t step = 0; step < branchLength && !pool_.isFull(); step++) {
            float t = (float)step / branchLength;
            float x = x0 + (x1 - x0) * t;
            float y = y0 + (y1 - y0) * t;

            // Small jitter for organic look
            x += (random(40) - 20) / 100.0f;
            y += (random(40) - 20) / 100.0f;

            // Branches are stationary and fade quickly (no BRANCH flag)
            pool_.spawn(x, y, 0.0f, 0.0f, intensity,
                       params_.defaultLifespan / 2, 1.0f,
                       0);  // No flags - branches don't branch again
        }
    }
}
