#include "Water.h"
#include <Arduino.h>

Water::Water() : params_(), noiseTime_(0.0f) {}

bool Water::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;

    // Configure forces for water behavior
    gravityForce_.setGravity(params_.gravity);
    windForce_.setWind(params_.windBase, params_.windVariation);
    dragForce_.setDrag(params_.drag);

    noiseTime_ = 0.0f;

    return true;
}

void Water::generate(PixelMatrix& matrix, const AudioControl& audio) {
    // Advance noise animation time
    // Music mode: wave-like pulsing synced to beat
    // Ambient mode: slow, gentle ocean swell
    float timeSpeed = audio.hasRhythm() ?
        0.03f + 0.02f * audio.energy :   // Music: 0.03-0.05 (wave-like)
        0.012f + 0.008f * audio.energy;  // Ambient: 0.012-0.02 (gentle swell)
    noiseTime_ += timeSpeed;

    // Render noise background first (tropical sea underlayer)
    renderNoiseBackground(matrix);

    // Run particle system (spawns, updates, renders particles)
    ParticleGenerator::generate(matrix, audio);
}

void Water::reset() {
    ParticleGenerator::reset();
    noiseTime_ = 0.0f;
}

void Water::spawnParticles(float dt) {
    float spawnProb = params_.baseSpawnChance;
    uint8_t dropCount = 0;

    if (audio_.hasRhythm()) {
        // MUSIC MODE: Dancey, wave-like spawning synced to beat
        // Creates rhythmic "rain" that pulses with the music
        float phasePulse = audio_.phaseToPulse();  // 1.0 at beat, fades to 0
        float phaseWave = 0.4f + 0.6f * phasePulse;  // Range 0.4-1.0

        // Modulate spawn rate with beat phase for wave effect
        spawnProb *= phaseWave;
        spawnProb += params_.audioSpawnBoost * audio_.pulse * phasePulse;

        // Wave burst on beat
        if (beatHappened()) {
            // Spawn a "wave" of drops across the width
            uint8_t waveDrops = 3 + (uint8_t)(5 * audio_.rhythmStrength);
            dropCount = (uint8_t)(waveDrops * (0.5f + 0.5f * audio_.energy));
        }
    } else {
        // AMBIENT MODE: Gentle, steady rainfall with subtle variations
        // Calm, meditative water flow
        float smoothEnergy = 0.4f + 0.3f * audio_.energy;  // Range 0.4-0.7
        spawnProb *= smoothEnergy;

        // Occasional gentle bursts on transients (like a gust of wind)
        if (audio_.pulse > params_.organicTransientMin) {
            float transientStrength = (audio_.pulse - params_.organicTransientMin) /
                                     (1.0f - params_.organicTransientMin);
            dropCount = (uint8_t)(2 * transientStrength);
        }
    }

    // Random baseline spawning (always some gentle rain)
    if (random(1000) < spawnProb * 1000) {
        dropCount++;
    }

    // Spawn drops from top (respect maxParticles limit)
    for (uint8_t i = 0; i < dropCount && pool_.getActiveCount() < params_.maxParticles; i++) {
        float x = random(width_ * 100) / 100.0f;
        float y = 0;  // Top of screen

        // Downward velocity with horizontal spread
        // Music mode: faster drops for more energy
        // Ambient mode: slower, more peaceful
        float velocityMult = audio_.hasRhythm() ? (1.0f + 0.2f * audio_.pulse) : 0.7f;
        float vy = (params_.dropVelocityMin +
                   random(100) * (params_.dropVelocityMax - params_.dropVelocityMin) / 100.0f) * velocityMult;
        float vx = (random(200) - 100) * params_.dropSpread / 100.0f;

        uint8_t intensity = random(params_.intensityMin, params_.intensityMax + 1);

        pool_.spawn(x, y, vx, vy, intensity, params_.defaultLifespan, 1.0f,
                   ParticleFlags::GRAVITY | ParticleFlags::WIND |
                   ParticleFlags::FADE | ParticleFlags::SPLASH);
    }
}

void Water::updateParticle(Particle* p, float dt) {
    // Check for splash on bottom collision
    if (p->hasFlag(ParticleFlags::SPLASH) && p->y >= height_ - 1) {
        spawnSplash(p->x, p->y, p->intensity);
        pool_.kill(p);
    }
}

void Water::renderParticle(const Particle* p, PixelMatrix& matrix) {
    int x = (int)p->x;
    int y = (int)p->y;

    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        uint32_t color = particleColor(p->intensity);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        // MAX BLENDING: Drops take brightest value to pop against dark background
        // Creates distinct, visible droplets on the sea surface
        RGB existing = matrix.getPixel(x, y);
        matrix.setPixel(x, y,
                       max(existing.r, r),
                       max(existing.g, g),
                       max(existing.b, b));
    }
}

uint32_t Water::particleColor(uint8_t intensity) const {
    // White/light cyan drops for visibility against blue background
    // Higher intensity = whiter (like foam/light reflection on water)
    uint8_t white = intensity;  // Base brightness
    uint8_t blue = min(255, intensity + 40);  // Slight blue tint
    uint8_t green = intensity * 3 / 4;  // Less green than blue
    return ((uint32_t)white << 16) | ((uint32_t)green << 8) | blue;
}

void Water::spawnSplash(float x, float y, uint8_t parentIntensity) {
    // Calculate available slots (respect maxParticles limit)
    uint8_t available = params_.maxParticles > pool_.getActiveCount()
                        ? params_.maxParticles - pool_.getActiveCount()
                        : 0;
    uint8_t splashCount = min(params_.splashParticles, available);

    for (uint8_t i = 0; i < splashCount; i++) {
        // Radial splash pattern
        float angle = (i * TWO_PI / splashCount) + random(100) * 0.01f;
        float speed = params_.splashVelocityMin +
                     random(100) * (params_.splashVelocityMax - params_.splashVelocityMin) / 100.0f;

        float vx = cos(angle) * speed;
        float vy = sin(angle) * speed - 1.0f;  // Slight upward component

        uint8_t intensity = min(255, (uint32_t)parentIntensity * params_.splashIntensity / 255);

        pool_.spawn(x, y, vx, vy, intensity, 30, 0.5f,  // Light, short-lived
                   ParticleFlags::GRAVITY | ParticleFlags::FADE);
    }
}

void Water::renderNoiseBackground(PixelMatrix& matrix) {
    // Tropical sea noise background - blue/green/cyan gradients
    // Simulates shallow tropical water with light playing on the surface

    // Noise scales for wave-like movement
    const float noiseScale = 0.12f;     // Spatial frequency
    const float timeScale = noiseTime_; // Animated movement

    // In music mode, add wave-like brightness pulsing
    float waveBrightness = 1.0f;
    if (audio_.hasRhythm()) {
        // Gentle wave pulse on beat (range 0.7-1.0)
        float phasePulse = audio_.phaseToPulse();
        waveBrightness = 0.7f + 0.3f * phasePulse;
    }

    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            // Sample multiple noise layers for complex water surface
            float nx = x * noiseScale;
            float ny = y * noiseScale;

            // Primary wave layer (slow, large waves)
            float wave1 = SimplexNoise::noise3D_01(nx, ny, timeScale);

            // Secondary ripple layer (faster, smaller)
            float wave2 = SimplexNoise::noise3D_01(nx * 2.5f, ny * 2.5f, timeScale * 1.5f);

            // Combine waves with different weights
            float noiseVal = wave1 * 0.6f + wave2 * 0.4f;

            // Apply wave brightness modulation
            float intensity = noiseVal * waveBrightness;

            // Moderate sea background - visible but drops pop against it
            intensity *= 0.18f;

            // Clamp and convert to 0-255 range
            intensity = constrain(intensity, 0.0f, 1.0f);
            uint8_t level = (uint8_t)(intensity * 255.0f);

            // Tropical sea color palette: deep blue -> turquoise -> cyan
            // Use second noise sample for color variation
            float colorNoise = SimplexNoise::noise3D_01(nx * 0.5f, ny * 0.5f, timeScale * 0.3f);

            uint8_t r, g, b;
            if (colorNoise < 0.4f) {
                // Deep blue-green (40% of surface)
                r = (uint8_t)(level * 0.05f);
                g = (uint8_t)(level * 0.4f);
                b = level;
            } else if (colorNoise < 0.7f) {
                // Turquoise/teal (30% of surface)
                r = (uint8_t)(level * 0.1f);
                g = (uint8_t)(level * 0.6f);
                b = (uint8_t)(level * 0.8f);
            } else {
                // Bright cyan highlights (30% of surface)
                r = (uint8_t)(level * 0.15f);
                g = (uint8_t)(level * 0.7f);
                b = (uint8_t)(level * 0.65f);
            }

            // Set pixel (first layer, direct set)
            matrix.setPixel(x, y, r, g, b);
        }
    }
}
