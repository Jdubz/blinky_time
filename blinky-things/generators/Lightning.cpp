#include "Lightning.h"
#include <Arduino.h>

Lightning::Lightning() : params_(), noiseTime_(0.0f) {}

bool Lightning::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;

    // Configure forces for lightning behavior (no forces - instant bolts)
    gravityForce_.setGravity(0.0f);
    dragForce_.setDrag(1.0f);  // No drag

    noiseTime_ = 0.0f;

    return true;
}

void Lightning::generate(PixelMatrix& matrix, const AudioControl& audio) {
    // Advance noise animation time
    // Music mode: dramatic, pulsing storm clouds synced to beat
    // Ambient mode: slow, ominous rolling clouds
    float timeSpeed = audio.hasRhythm() ?
        0.025f + 0.02f * audio.energy :  // Music: 0.025-0.045 (dramatic)
        0.01f + 0.005f * audio.energy;   // Ambient: 0.01-0.015 (ominous)
    noiseTime_ += timeSpeed;

    // Render storm sky noise background first
    renderNoiseBackground(matrix);

    // Run particle system (spawns, updates, renders particles)
    ParticleGenerator::generate(matrix, audio);
}

void Lightning::reset() {
    ParticleGenerator::reset();
    noiseTime_ = 0.0f;
}

void Lightning::spawnParticles(float dt) {
    float spawnProb = params_.baseSpawnChance;
    uint8_t boltCount = 0;

    if (audio_.hasRhythm()) {
        // MUSIC MODE: Dramatic, pulsating lightning synced to beat
        // Lightning strikes on beats with intensity variation
        float phasePulse = audio_.phaseToPulse();  // 1.0 at beat, fades to 0

        // Build tension between beats (reduce random spawns)
        // Then release with powerful strikes on beat
        spawnProb *= (0.3f + 0.7f * phasePulse);
        spawnProb += params_.audioSpawnBoost * audio_.pulse * phasePulse;

        // Dramatic bolt burst on beat
        if (beatHappened()) {
            // More bolts with higher rhythm confidence
            uint8_t baseBolts = 2 + (uint8_t)(2 * audio_.rhythmStrength);
            boltCount = (uint8_t)(baseBolts * (0.5f + 0.5f * audio_.energy));
        }
    } else {
        // AMBIENT MODE: Slow, ominous storm with occasional strikes
        // Creates atmosphere with long pauses between strikes
        float smoothEnergy = 0.2f + 0.3f * audio_.energy;  // Range 0.2-0.5 (less frequent)
        spawnProb *= smoothEnergy;

        // Occasional strikes on transients (dramatic but rare)
        if (audio_.pulse > params_.organicTransientMin) {
            float transientStrength = (audio_.pulse - params_.organicTransientMin) /
                                     (1.0f - params_.organicTransientMin);
            if (transientStrength > 0.5f) {
                boltCount = 1;  // Single dramatic strike
            }
        }
    }

    // Random baseline spawning (occasional random strikes)
    if (random(1000) < spawnProb * 1000) {
        boltCount++;
    }

    // Spawn coherent lightning bolts as connected particle chains (respect maxParticles limit)
    for (uint8_t i = 0; i < boltCount && pool_.getActiveCount() < params_.maxParticles; i++) {
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
    for (int step = 0; step <= steps && pool_.getActiveCount() < params_.maxParticles; step++) {
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
        if (random(100) < params_.branchChance && pool_.getActiveCount() < params_.maxParticles) {
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
    // Calculate available slots (respect maxParticles limit)
    uint8_t available = params_.maxParticles > pool_.getActiveCount()
                        ? params_.maxParticles - pool_.getActiveCount()
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
        for (uint8_t step = 0; step < branchLength && pool_.getActiveCount() < params_.maxParticles; step++) {
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

void Lightning::renderNoiseBackground(PixelMatrix& matrix) {
    // Storm sky noise background - sunset/night sky colors
    // Deep purples, dark oranges, and blues for dramatic atmosphere

    // Noise scales for rolling cloud movement
    const float noiseScale = 0.1f;      // Spatial frequency
    const float timeScale = noiseTime_; // Animated movement

    // In music mode, add dramatic pulsing (clouds darken before lightning)
    float stormIntensity = 1.0f;
    if (audio_.hasRhythm()) {
        // Inverse pulse - darken BETWEEN beats, brighten slightly on beat
        // Creates tension-release cycle
        float phasePulse = audio_.phaseToPulse();
        stormIntensity = 0.5f + 0.5f * phasePulse;  // Range 0.5-1.0
    }

    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            // Height-based color: sunset colors at bottom, night sky at top
            float normalizedY = (float)y / (height_ - 1);

            // Sample multiple noise layers for complex cloud structure
            float nx = x * noiseScale;
            float ny = y * noiseScale;

            // Primary cloud layer (large, slow-moving)
            float cloud1 = SimplexNoise::noise3D_01(nx, ny, timeScale);

            // Secondary detail layer (smaller, faster)
            float cloud2 = SimplexNoise::noise3D_01(nx * 2.0f, ny * 2.0f, timeScale * 1.2f);

            // Combine clouds
            float noiseVal = cloud1 * 0.7f + cloud2 * 0.3f;

            // Apply storm intensity modulation
            float intensity = noiseVal * stormIntensity;

            // Moderate storm sky - visible atmosphere, bolts pop against it
            intensity *= 0.12f;

            // Clamp and convert to 0-255 range
            intensity = constrain(intensity, 0.0f, 1.0f);
            uint8_t level = (uint8_t)(intensity * 255.0f);

            // Storm sky color palette:
            // Bottom: sunset orange/red glow (horizon)
            // Middle: deep purple storm clouds
            // Top: dark blue night sky
            uint8_t r, g, b;
            if (normalizedY > 0.7f) {
                // Bottom 30%: sunset/horizon glow (orange-red)
                float horizonFade = (normalizedY - 0.7f) / 0.3f;  // 0 to 1
                r = (uint8_t)(level * (0.6f + 0.4f * horizonFade));
                g = (uint8_t)(level * (0.2f + 0.2f * horizonFade));
                b = (uint8_t)(level * 0.3f);
            } else if (normalizedY > 0.3f) {
                // Middle 40%: deep purple storm clouds
                r = (uint8_t)(level * 0.4f);
                g = (uint8_t)(level * 0.1f);
                b = (uint8_t)(level * 0.5f);
            } else {
                // Top 30%: dark blue night sky
                r = (uint8_t)(level * 0.15f);
                g = (uint8_t)(level * 0.1f);
                b = (uint8_t)(level * 0.4f);
            }

            // Set pixel (first layer, direct set)
            matrix.setPixel(x, y, r, g, b);
        }
    }
}
