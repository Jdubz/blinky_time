#include "Fire.h"
#include "../physics/PhysicsContext.h"
#include "../physics/MatrixBackground.h"
#include "../physics/LinearBackground.h"
#include "../physics/EdgeSpawnRegion.h"
#include "../physics/RandomSpawnRegion.h"
#include "../physics/KillBoundary.h"
#include "../physics/WrapBoundary.h"
#include "../physics/MatrixForceAdapter.h"
#include "../physics/LinearForceAdapter.h"
#include <Arduino.h>

Fire::Fire()
    : params_(), beatCount_(0), noiseTime_(0.0f),
      background_(nullptr) {}

Fire::~Fire() {
    // Physics components use placement new, no delete needed
}

bool Fire::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;

    beatCount_ = 0;

    return true;
}

void Fire::initPhysicsContext() {
    // Set physics parameters from FireParams
    gravity_ = params_.gravity;
    drag_ = params_.drag;

    // Create layout-appropriate physics components
    bool wrap = PhysicsContext::shouldWrapByDefault(layout_);

    // Spawn region: bottom edge for matrix, random for linear
    spawnRegion_ = PhysicsContext::createSpawnRegion(
        layout_, GeneratorType::FIRE, width_, height_, spawnBuffer_);

    // Boundary: kill for matrix, wrap for linear
    boundary_ = PhysicsContext::createBoundary(
        layout_, GeneratorType::FIRE, wrap, boundaryBuffer_);

    // Force adapter: 2D for matrix, 1D for linear
    forceAdapter_ = PhysicsContext::createForceAdapter(layout_, forceBuffer_);
    if (forceAdapter_) {
        forceAdapter_->setWind(params_.windBase, params_.windVariation);
    }

    // Background model: height-falloff for matrix, uniform for linear
    background_ = PhysicsContext::createBackground(
        layout_, BackgroundStyle::FIRE, backgroundBuffer_);
}

void Fire::generate(PixelMatrix& matrix, const AudioControl& audio) {
    // Advance noise animation time (blend between organic and music-driven)
    float organicSpeed = 0.015f + 0.005f * audio.energy;
    float musicSpeed = 0.04f + 0.03f * audio.energy;
    noiseTime_ += organicSpeed * (1.0f - audio.rhythmStrength) + musicSpeed * audio.rhythmStrength;

    // Render noise background first (underlayer)
    if (background_) {
        background_->setIntensity(params_.backgroundIntensity);
        background_->render(matrix, width_, height_, noiseTime_, audio);
    }

    // Modulate wind turbulence by audio (phase breathing + transient gusts)
    if (forceAdapter_) {
        float phasePulse = audio.phaseToPulse();  // 1.0 on beat, 0.0 off-beat
        // Wind breathes: 30% base + 70% phase modulation (dramatic calming between beats)
        float phaseWind = 0.3f + 0.7f * phasePulse;
        // Transient gusts: spike to 3x on strong hits
        float transientGust = 1.0f + 2.0f * audio.pulse;
        // Blend modulation by rhythmStrength (no modulation when no rhythm detected)
        float mod = 1.0f * (1.0f - audio.rhythmStrength) +
                    phaseWind * transientGust * audio.rhythmStrength;
        forceAdapter_->setWind(params_.windBase, params_.windVariation * mod);
    }

    // Run particle system (spawn → physics → render)
    // Particles are the only visual primitive; no heat buffer, no secondary layer
    ParticleGenerator::generate(matrix, audio);
}

void Fire::reset() {
    ParticleGenerator::reset();
    beatCount_ = 0;
    noiseTime_ = 0.0f;
}

void Fire::spawnParticles(float dt) {
    float spawnProb;
    uint8_t sparkCount = 0;

    // MUSIC-DRIVEN behavior (rhythmStrength weighted)
    float phasePulse = audio_.phaseToPulse();
    // musicSpawnPulse controls phase depth: 0=flat (no modulation), 1=full range (silent off-beat)
    float phasePump = (1.0f - params_.musicSpawnPulse) + params_.musicSpawnPulse * phasePulse;

    float musicSpawnProb = params_.baseSpawnChance * phasePump + params_.audioSpawnBoost * audio_.energy;

    // Transient response (stronger in music mode)
    if (audio_.pulse > params_.organicTransientMin) {
        float transientStrength = (audio_.pulse - params_.organicTransientMin) /
                                 (1.0f - params_.organicTransientMin);
        uint8_t musicSparks = (uint8_t)(params_.burstSparks * transientStrength);
        uint8_t organicSparks = 2;  // Small boost in organic mode
        sparkCount += (uint8_t)(organicSparks * (1.0f - audio_.rhythmStrength) +
                                musicSparks * audio_.rhythmStrength);
    }

    // Extra burst on predicted beats (only when rhythm is strong)
    if (beatHappened() && audio_.rhythmStrength > 0.3f) {
        beatCount_++;
        sparkCount += (uint8_t)(params_.burstSparks * audio_.rhythmStrength);
    }

    // ORGANIC-DRIVEN behavior (inverse rhythmStrength weighted)
    float organicSpawnProb = params_.baseSpawnChance + params_.audioSpawnBoost * audio_.energy;

    // Continuous spark rate proportional to energy (organic mode)
    if (audio_.energy > 0.05f) {
        uint8_t organicSparks = (uint8_t)((audio_.energy - 0.05f) * params_.burstSparks * 0.5f);
        sparkCount += (uint8_t)(organicSparks * (1.0f - audio_.rhythmStrength));
    }

    // BLEND spawn probability between modes
    spawnProb = organicSpawnProb * (1.0f - audio_.rhythmStrength) +
                musicSpawnProb * audio_.rhythmStrength;

    // Random baseline spawning
    if (random(1000) < spawnProb * 1000) {
        sparkCount++;
    }

    // Spawn sparks using layout-aware spawn region with variety
    for (uint8_t i = 0; i < sparkCount && pool_.getActiveCount() < params_.maxParticles; i++) {
        float x, y;
        getSpawnPosition(x, y);

        // Base speed for this spark
        float baseSpeed = params_.sparkVelocityMin +
                         random(100) * (params_.sparkVelocityMax - params_.sparkVelocityMin) / 100.0f;

        // Determine spark type (more variety during music mode)
        SparkType type;
        float varietyRoll = random(1000) / 1000.0f;

        // Music mode: favor burst sparks on transients, fast sparks otherwise
        // Organic mode: mix of fast sparks and slow embers
        if (audio_.rhythmStrength > 0.5f && audio_.pulse > 0.3f) {
            type = SparkType::BURST_SPARK;  // High-energy transient
        } else if (varietyRoll < params_.fastSparkRatio) {
            type = SparkType::FAST_SPARK;   // Primary sparks
        } else {
            type = SparkType::SLOW_EMBER;   // Glowing embers
        }

        spawnTypedParticle(type, x, y, baseSpeed);
    }
}

void Fire::spawnTypedParticle(SparkType type, float x, float y, float baseSpeed) {
    float vx, vy;
    getInitialVelocity(baseSpeed, vx, vy);

    // Add spread perpendicular to main direction
    float spreadAmount = (random(200) - 100) * params_.sparkSpread / 100.0f;
    if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
        vx += spreadAmount;  // Matrix: horizontal spread
    } else {
        vy += spreadAmount * 0.3f;  // Linear: minimal vertical spread
    }

    // Blend velocity multiplier between organic and music modes
    float organicVelMult = 0.8f;
    float phasePulse = audio_.phaseToPulse();
    float musicVelMult = 0.8f + 0.4f * phasePulse + 0.3f * audio_.pulse;  // Faster on-beat + transient boost
    float velocityMult = organicVelMult * (1.0f - audio_.rhythmStrength) +
                        musicVelMult * audio_.rhythmStrength;

    // Type-specific characteristics
    uint8_t intensity, lifespan;
    float speedMult;

    switch (type) {
    case SparkType::FAST_SPARK: {
        // Sort min/max so random() always gets valid (lo, hi) even if misconfigured via serial
        int lo = min((int)params_.intensityMin, (int)params_.intensityMax);
        int hi = max((int)params_.intensityMin, (int)params_.intensityMax) + 1;
        intensity = (uint8_t)random(lo, hi);
        lifespan = params_.defaultLifespan;  // 1.7s (170 centiseconds)
        speedMult = 1.0f;
        break;
    }

    case SparkType::SLOW_EMBER: {
        // Embers are dimmer than sparks; guard bounds to prevent inverted/zero range
        // which would cause UB (random() requires max > min)
        int lo = max(0, (int)params_.intensityMin - 30);
        int hi = max(0, (int)params_.intensityMax - 50);
        if (hi <= lo) hi = lo + 1;              // Prevent random(x, x) or inverted range
        intensity = (uint8_t)max(1, random(lo, hi));  // max(1,...) ensures spawn succeeds
        lifespan = (uint8_t)min(255, params_.defaultLifespan * 1.5f);  // 2.55s max, clamped
        speedMult = 0.6f;  // 40% slower
        break;
    }

    case SparkType::BURST_SPARK:
        // Maximum brightness on transient
        intensity = params_.intensityMax;
        lifespan = (uint8_t)(params_.defaultLifespan * 0.8f);  // 1.36s (136 centiseconds)
        speedMult = 1.0f;
        break;

    default:
        intensity = params_.intensityMax;
        lifespan = params_.defaultLifespan;
        speedMult = 1.0f;
        break;
    }

    // Phase-driven intensity boost: brighter on-beat, dimmer off-beat
    if (audio_.rhythmStrength > 0.3f) {
        int boost = (int)(phasePulse * 35 * audio_.rhythmStrength);
        intensity = (uint8_t)min(255, (int)intensity + boost);
    }

    // Apply speed and velocity multipliers
    vx *= velocityMult * speedMult;
    vy *= velocityMult * speedMult;

    // Spawn the particle (trailHeatFactor=0: heat is per-particle intensity, not heat buffer)
    pool_.spawn(x, y, vx, vy, intensity, lifespan, 1.0f,
               ParticleFlags::GRAVITY | ParticleFlags::WIND | ParticleFlags::FADE,
               0);
}

void Fire::updateParticle(Particle* p, float dt) {
    if (params_.thermalForce <= 0.0f) return;

    // Thermal buoyancy: hotter particles rise faster.
    // As FADE flag reduces intensity over lifetime, buoyancy decreases naturally.
    float heat = p->intensity / 255.0f;

    // At full intensity (1.0): applies params_.thermalForce LEDs/sec^2 upward.
    // p->mass is clamped to 0.01 minimum by ParticlePool::spawn, no div-by-zero risk.
    if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
        p->vy -= (heat * params_.thermalForce / p->mass) * dt;  // Matrix: upward = negative Y
    } else {
        p->vx += (heat * params_.thermalForce / p->mass) * dt;  // Linear: forward = positive X
    }
}

void Fire::renderParticle(const Particle* p, PixelMatrix& matrix) {
    int x = (int)p->x;
    int y = (int)p->y;

    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        uint32_t color = particleColor(p->intensity);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        // ADDITIVE BLENDING: Particles add to existing colors
        RGB existing = matrix.getPixel(x, y);
        matrix.setPixel(x, y,
                       min(255, (int)existing.r + r),
                       min(255, (int)existing.g + g),
                       min(255, (int)existing.b + b));
    }
}

uint32_t Fire::particleColor(uint8_t intensity) const {
    // Smooth fire palette with 6 color stops (black → deep red → red → orange → yellow-orange → bright yellow)
    // Each stop is at a specific intensity value with RGB color
    struct ColorStop {
        uint8_t position;
        uint8_t r, g, b;
    };

    const ColorStop palette[] = {
        {0,   0,   0,   0},     // Black
        {51,  64,  0,   0},     // Deep red (20%)
        {102, 255, 0,   0},     // Red (40%)
        {153, 255, 128, 0},     // Orange (60%)
        {204, 255, 200, 0},     // Yellow-orange (80%)
        {255, 255, 255, 64}     // Bright yellow (100%)
    };
    const int paletteSize = 6;

    // Find surrounding color stops
    int lowerIdx = 0;
    int upperIdx = 1;

    for (int i = 0; i < paletteSize - 1; i++) {
        if (intensity >= palette[i].position && intensity <= palette[i+1].position) {
            lowerIdx = i;
            upperIdx = i + 1;
            break;
        }
    }

    // Interpolate between stops
    const ColorStop& lower = palette[lowerIdx];
    const ColorStop& upper = palette[upperIdx];

    float range = upper.position - lower.position;
    float t = (range > 0) ? (float)(intensity - lower.position) / range : 0.0f;

    uint8_t r = (uint8_t)(lower.r + t * (upper.r - lower.r));
    uint8_t g = (uint8_t)(lower.g + t * (upper.g - lower.g));
    uint8_t b = (uint8_t)(lower.b + t * (upper.b - lower.b));

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

