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
    // Advance noise animation time
    float timeSpeed = audio.hasRhythm() ?
        0.04f + 0.03f * audio.energy :
        0.015f + 0.005f * audio.energy;
    noiseTime_ += timeSpeed;

    // Render noise background first (underlayer)
    if (background_) {
        background_->render(matrix, width_, height_, noiseTime_, audio);
    }

    // Apply cooling to heat buffer
    applyCooling();

    // Run particle system (spawns, updates, renders particles)
    ParticleGenerator::generate(matrix, audio);

    // Diffuse heat using layout-appropriate propagation
    if (propagation_) {
        propagation_->propagate(heat_, width_, height_, 0.7f);
    }

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

    if (audio_.hasRhythm()) {
        // MUSIC MODE: Dancey, pulsating spawning synced to beat
        float phasePulse = audio_.phaseToPulse();
        float phasePump = 0.5f + 0.5f * phasePulse;

        spawnProb *= phasePump;
        spawnProb += params_.audioSpawnBoost * audio_.pulse * phasePulse;

        if (beatHappened()) {
            beatCount_++;
            uint8_t baseSparks = params_.burstSparks;
            if (beatCount_ % 4 == 0) {
                baseSparks = params_.burstSparks * 3;  // Downbeat: triple burst
            } else if (beatCount_ % 2 == 0) {
                baseSparks = params_.burstSparks * 2;  // Backbeat: double burst
            }
            sparkCount = (uint8_t)(baseSparks * (0.4f + 0.6f * audio_.rhythmStrength) *
                                   (0.5f + 0.5f * audio_.energy));
        }
    } else {
        // AMBIENT MODE: Smooth, steady output with subtle shifts
        float smoothEnergy = 0.3f + 0.4f * audio_.energy;
        spawnProb *= smoothEnergy;

        if (audio_.pulse > params_.organicTransientMin) {
            float transientStrength = (audio_.pulse - params_.organicTransientMin) /
                                     (1.0f - params_.organicTransientMin);
            sparkCount = (uint8_t)(params_.burstSparks * 0.3f * transientStrength);
        }
    }

    // Random baseline spawning
    if (random(1000) < spawnProb * 1000) {
        sparkCount++;
    }

    // Spawn sparks using layout-aware spawn region
    for (uint8_t i = 0; i < sparkCount && pool_.getActiveCount() < params_.maxParticles; i++) {
        float x, y;
        getSpawnPosition(x, y);

        // Get initial velocity from spawn region
        float speed = params_.sparkVelocityMin +
                     random(100) * (params_.sparkVelocityMax - params_.sparkVelocityMin) / 100.0f;

        float vx, vy;
        getInitialVelocity(speed, vx, vy);

        // Add spread perpendicular to main direction
        float spreadAmount = (random(200) - 100) * params_.sparkSpread / 100.0f;
        if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
            // Matrix: spread is horizontal
            vx += spreadAmount;
        } else {
            // Linear: spread is vertical (minimal effect on 1D)
            vy += spreadAmount * 0.3f;
        }

        // Music mode: higher velocity
        float velocityMult = audio_.hasRhythm() ? (1.0f + 0.3f * audio_.pulse) : 0.8f;
        vx *= velocityMult;
        vy *= velocityMult;

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
            heat_[index] = min(255, (int)heat_[index] + trailHeat);
        }
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
    // Fire palette: black -> red -> orange -> yellow
    if (intensity < 85) {
        uint32_t red = min(255u, (uint32_t)intensity * 3);
        return (red << 16);
    } else if (intensity < 170) {
        uint32_t green = min(255u, (uint32_t)(intensity - 85) * 3);
        return (0xFF0000 | (green << 8));
    } else {
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
