#include "WaterParticle.h"
#include <Arduino.h>

WaterParticle::WaterParticle() : params_() {}

bool WaterParticle::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;

    // Configure forces for water behavior
    gravityForce_.setGravity(params_.gravity);
    windForce_.setWind(params_.windBase, params_.windVariation);
    dragForce_.setDrag(params_.drag);

    return true;
}

void WaterParticle::spawnParticles(float dt) {
    float spawnProb = params_.baseSpawnChance;
    uint8_t dropCount = 0;

    if (audio_.hasRhythm()) {
        // MUSIC MODE: Beat-synced wave generation
        if (audio_.pulse > 0.3f) {
            // Boost probability near on-beat moments (phase near 0 or 1)
            float beatBoost = audio_.phaseToPulse();  // 1.0 at phase=0, 0.0 at phase=0.5
            spawnProb += params_.audioSpawnBoost * audio_.pulse * beatBoost;
        }

        // Burst on beat
        if (beatHappened()) {
            dropCount = 4;  // Wave on beat
        }
    } else {
        // ORGANIC MODE: Transient-reactive with threshold
        if (audio_.pulse > params_.organicTransientMin) {
            spawnProb += params_.audioSpawnBoost * audio_.pulse;
            dropCount = 2;
        }
    }

    // Random baseline spawning
    if (random(1000) < spawnProb * 1000) {
        dropCount++;
    }

    // Spawn drops from top
    for (uint8_t i = 0; i < dropCount && !pool_.isFull(); i++) {
        float x = random(width_ * 100) / 100.0f;
        float y = 0;  // Top of screen

        // Downward velocity with horizontal spread
        float vy = params_.dropVelocityMin +
                  random(100) * (params_.dropVelocityMax - params_.dropVelocityMin) / 100.0f;
        float vx = (random(200) - 100) * params_.dropSpread / 100.0f;

        uint8_t intensity = random(params_.intensityMin, params_.intensityMax + 1);

        pool_.spawn(x, y, vx, vy, intensity, params_.defaultLifespan, 1.0f,
                   ParticleFlags::GRAVITY | ParticleFlags::WIND |
                   ParticleFlags::FADE | ParticleFlags::SPLASH);
    }
}

void WaterParticle::updateParticle(Particle* p, float dt) {
    // Check for splash on bottom collision
    if (p->hasFlag(ParticleFlags::SPLASH) && p->y >= height_ - 1) {
        spawnSplash(p->x, p->y, p->intensity);
        pool_.kill(p);
    }
}

void WaterParticle::renderParticle(const Particle* p, PixelMatrix& matrix) {
    int x = (int)p->x;
    int y = (int)p->y;

    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        uint32_t color = particleColor(p->intensity);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        // Additive blending (cast to int to prevent overflow)
        RGB existing = matrix.getPixel(x, y);
        matrix.setPixel(x, y,
                       min(255, (int)existing.r + r),
                       min(255, (int)existing.g + g),
                       min(255, (int)existing.b + b));
    }
}

uint32_t WaterParticle::particleColor(uint8_t intensity) const {
    // Use Water palette
    return Palette::WATER.toColor(intensity);
}

void WaterParticle::spawnSplash(float x, float y, uint8_t parentIntensity) {
    uint8_t splashCount = min(params_.splashParticles,
                             (uint8_t)(pool_.getCapacity() - pool_.getActiveCount()));

    for (uint8_t i = 0; i < splashCount; i++) {
        // Radial splash pattern
        float angle = (i * TWO_PI / splashCount) + random(100) * 0.01f;
        float speed = params_.splashVelocityMin +
                     random(100) * (params_.splashVelocityMax - params_.splashVelocityMin) / 100.0f;

        float vx = cos(angle) * speed;
        float vy = sin(angle) * speed - 1.0f;  // Slight upward component

        uint8_t intensity = min(255, parentIntensity * params_.splashIntensity / 255);

        pool_.spawn(x, y, vx, vy, intensity, 30, 0.5f,  // Light, short-lived
                   ParticleFlags::GRAVITY | ParticleFlags::FADE);
    }
}
