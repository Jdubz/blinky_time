#include "Fire.h"
#include "../physics/PhysicsContext.h"
#include "../physics/MatrixPropagation.h"
#include "../physics/LinearPropagation.h"
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
    : heat_(nullptr), params_(), beatCount_(0), noiseTime_(0.0f),
      propagation_(nullptr), background_(nullptr) {}

Fire::~Fire() {
    if (heat_) {
        delete[] heat_;
        heat_ = nullptr;
    }
    // Physics components use placement new, no delete needed
}

bool Fire::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;

    // Allocate heat buffer
    heat_ = new uint8_t[numLeds_];
    if (!heat_) return false;
    memset(heat_, 0, numLeds_);

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

    // Propagation model: upward for matrix, lateral for linear
    propagation_ = PhysicsContext::createPropagation(
        layout_, width_, height_, wrap, propagationBuffer_);

    // Background model: height-falloff for matrix, uniform for linear
    background_ = PhysicsContext::createBackground(
        layout_, BackgroundStyle::FIRE, backgroundBuffer_);
}

void Fire::generate(PixelMatrix& matrix, const AudioControl& audio) {
    // Advance noise animation time (blend between organic and music-driven)
    float organicSpeed = 0.015f + 0.005f * audio.energy;
    float musicSpeed = 0.04f + 0.03f * audio.energy;
    float timeSpeed = organicSpeed * (1.0f - audio.rhythmStrength) + musicSpeed * audio.rhythmStrength;
    noiseTime_ += timeSpeed;

    // Render noise background first (underlayer)
    if (background_) {
        background_->setIntensity(params_.backgroundIntensity);
        background_->render(matrix, width_, height_, noiseTime_, audio);
    }

    // Apply cooling to heat buffer
    applyCooling();

    // Run particle system (spawns, updates, renders particles)
    ParticleGenerator::generate(matrix, audio);

    // REMOVED: Upward heat propagation (was defeating particle physics)
    // Heat now only cools in place and creates thermal updraft force in updateParticle()
    // This makes fire rise due to physics (thermal buoyancy) rather than artificial spreading
    // if (propagation_) {
    //     propagation_->propagate(heat_, width_, height_, 0.7f);
    // }

    // Blend heat buffer with particle rendering
    blendHeatToMatrix(matrix);
}

void Fire::reset() {
    ParticleGenerator::reset();
    if (heat_) {
        memset(heat_, 0, numLeds_);
    }
    beatCount_ = 0;
    noiseTime_ = 0.0f;
}

void Fire::spawnParticles(float dt) {
    float spawnProb = params_.baseSpawnChance;
    uint8_t sparkCount = 0;

    // MUSIC-DRIVEN behavior (rhythmStrength weighted)
    float phasePulse = audio_.phaseToPulse();
    float phasePump = 0.5f + 0.5f * phasePulse;

    float musicSpawnProb = params_.baseSpawnChance * phasePump + params_.audioSpawnBoost * audio_.energy;

    // Transient response (stronger in music mode)
    if (audio_.pulse > params_.organicTransientMin) {
        float transientStrength = (audio_.pulse - params_.organicTransientMin) /
                                 (1.0f - params_.organicTransientMin);
        uint8_t musicSparks = (uint8_t)(params_.burstSparks * transientStrength *
                               (0.5f + 0.5f * audio_.energy));
        uint8_t organicSparks = 2;  // Small boost in organic mode
        sparkCount += (uint8_t)(organicSparks * (1.0f - audio_.rhythmStrength) +
                                musicSparks * audio_.rhythmStrength);
    }

    // Extra burst on predicted downbeats (only when rhythm is strong)
    if (beatHappened() && audio_.rhythmStrength > 0.3f) {
        beatCount_++;
        if (beatCount_ % 4 == 0) {
            sparkCount += (uint8_t)(params_.burstSparks * 0.5f * audio_.rhythmStrength);
        }
    }

    // ORGANIC-DRIVEN behavior (inverse rhythmStrength weighted)
    float organicSpawnProb = params_.baseSpawnChance + params_.audioSpawnBoost * audio_.energy;

    // Continuous spark rate proportional to energy (organic mode)
    if (audio_.energy > 0.2f) {
        uint8_t organicSparks = (uint8_t)((audio_.energy - 0.2f) * params_.burstSparks * 0.5f);
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
    float musicVelMult = 1.0f + 0.3f * audio_.pulse;
    float velocityMult = organicVelMult * (1.0f - audio_.rhythmStrength) +
                        musicVelMult * audio_.rhythmStrength;

    // Type-specific characteristics
    uint8_t intensity, lifespan, trailHeatFactor;
    float speedMult;

    switch (type) {
    case SparkType::FAST_SPARK:
        // Fast, short-lived, minimal trail
        intensity = random(params_.intensityMin, params_.intensityMax + 1);
        lifespan = params_.defaultLifespan;  // 1.7s (170 centiseconds)
        trailHeatFactor = params_.trailHeatFactor;  // Minimal (5%)
        speedMult = 1.0f;  // Normal speed (variety is in lifespan/trail, not velocity)
        break;

    case SparkType::SLOW_EMBER:
        // Slow, long-lived, heavy trail
        intensity = random(max(0, params_.intensityMin - 30), max(0, params_.intensityMax - 50));  // Dimmer
        lifespan = (uint8_t)min(255, params_.defaultLifespan * 1.5f);  // 2.55s max (255 centiseconds), clamped
        trailHeatFactor = (uint8_t)min(255, params_.trailHeatFactor * 4);  // Heavy trail (20%), clamped
        speedMult = 0.6f;  // 40% slower (less extreme than 0.4)
        break;

    case SparkType::BURST_SPARK:
        // Medium speed, very bright, medium trail
        intensity = params_.intensityMax;  // Maximum brightness
        lifespan = (uint8_t)(params_.defaultLifespan * 0.8f);  // 1.36s (136 centiseconds)
        trailHeatFactor = params_.trailHeatFactor * 2;  // Medium trail (10%)
        speedMult = 1.0f;  // Normal speed
        break;

    default:
        intensity = params_.intensityMax;
        lifespan = params_.defaultLifespan;
        trailHeatFactor = params_.trailHeatFactor;
        speedMult = 1.0f;
        break;
    }

    // Apply speed and velocity multipliers
    vx *= velocityMult * speedMult;
    vy *= velocityMult * speedMult;

    // Spawn the particle with type-specific trail heat factor
    pool_.spawn(x, y, vx, vy, intensity, lifespan, 1.0f,
               ParticleFlags::GRAVITY | ParticleFlags::WIND |
               ParticleFlags::FADE | ParticleFlags::EMIT_TRAIL,
               trailHeatFactor);
}

void Fire::updateParticle(Particle* p, float dt) {
    // Get particle position once
    int x = (int)p->x;
    int y = (int)p->y;
    int index = coordsToIndex(x, y);

    if (index < 0 || index >= numLeds_) return;

    // Emit heat trail if flagged (uses per-particle trail heat factor)
    if (p->hasFlag(ParticleFlags::EMIT_TRAIL)) {
        uint8_t trailHeat = p->intensity * p->trailHeatFactor / 100;
        heat_[index] = min(255, (int)heat_[index] + trailHeat);
    }

    // Apply thermal buoyancy force based on heat at particle position
    // Heat creates upward thermal updraft (like hot air rising)
    float heatStrength = heat_[index] / 255.0f;

    // Thermal force strength: 30 LEDs/sec^2 at max heat
    // This is independent of gravity setting and strong enough to create visible rising motion
    // Can be overcome by wind/drag, creating natural turbulent behavior
    const float THERMAL_FORCE_STRENGTH = 30.0f;
    float thermalForce = heatStrength * THERMAL_FORCE_STRENGTH;

    // Apply upward force (layout-aware, respects particle mass)
    if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
        p->vy -= (thermalForce / p->mass) * dt;  // Matrix: upward = negative Y
    } else {
        p->vx += (thermalForce / p->mass) * dt;  // Linear: forward = positive X
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

void Fire::applyCooling() {
    uint8_t cooling = params_.trailDecay;

    // Audio-reactive cooling (breathing effect, blended by rhythmStrength)
    float breathe = -cos(audio_.phase * TWO_PI);
    int8_t coolingMod = (int8_t)(breathe * 15.0f * audio_.rhythmStrength);
    cooling = max(0, min(255, (int)cooling + coolingMod));

    // Apply cooling to all heat values
    for (int i = 0; i < numLeds_; i++) {
        uint8_t coolAmount = random(0, cooling + 1);
        if (heat_[i] > coolAmount) {
            heat_[i] -= coolAmount;
        } else {
            heat_[i] = 0;
        }
    }
}

void Fire::blendHeatToMatrix(PixelMatrix& matrix) {
    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            int index = coordsToIndex(x, y);
            if (index >= 0 && index < numLeds_) {
                uint32_t color = particleColor(heat_[index]);
                uint8_t r = (color >> 16) & 0xFF;
                uint8_t g = (color >> 8) & 0xFF;
                uint8_t b = color & 0xFF;

                // MAX BLENDING: Heat field takes brightest value
                RGB existing = matrix.getPixel(x, y);
                matrix.setPixel(x, y,
                               max(existing.r, r),
                               max(existing.g, g),
                               max(existing.b, b));
            }
        }
    }
}
