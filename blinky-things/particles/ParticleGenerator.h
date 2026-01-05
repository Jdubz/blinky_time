#pragma once

#include "../generators/Generator.h"
#include "ParticlePool.h"
#include "ParticleForce.h"
#include <Arduino.h>

/**
 * ParticleGenerator - Base class for particle-based generators
 *
 * Provides unified particle lifecycle management:
 * 1. Spawn particles based on audio
 * 2. Apply forces (gravity, wind, drag)
 * 3. Update positions
 * 4. Age and kill particles
 * 5. Render to matrix
 *
 * Subclasses customize behavior by implementing:
 * - spawnParticles(): When and how to create particles
 * - updateParticle(): Per-particle custom logic
 * - renderParticle(): How to draw each particle
 * - particleColor(): Particle appearance
 *
 * Template parameter MAX_PARTICLES determines pool size.
 */
template<uint8_t MAX_PARTICLES>
class ParticleGenerator : public Generator {
public:
    ParticleGenerator()
        : pool_(), gravityForce_(0.0f), windForce_(0.0f, 0.0f),
          dragForce_(0.98f), prevPhase_(1.0f) {}

    virtual ~ParticleGenerator() override = default;

    bool begin(const DeviceConfig& config) override {
        width_ = config.matrix.width;
        height_ = config.matrix.height;
        numLeds_ = width_ * height_;
        layout_ = config.matrix.layoutType;
        orientation_ = config.matrix.orientation;

        pool_.reset();
        lastUpdateMs_ = millis();

        return true;
    }

    void generate(PixelMatrix& matrix, const AudioControl& audio) override {
        audio_ = audio;

        // Calculate delta time
        uint32_t currentMs = millis();
        float dt = (currentMs - lastUpdateMs_) / 1000.0f;
        lastUpdateMs_ = currentMs;

        // Update physics forces
        updateForces(dt);

        // Spawn new particles based on audio
        spawnParticles(dt);

        // Update all particles (position, velocity, age, death)
        updateParticles(dt);

        // Render particles to matrix
        renderParticles(matrix);

        // Store previous phase for beat detection
        prevPhase_ = audio_.phase;
    }

    void reset() override {
        pool_.reset();
        audio_ = AudioControl();
        prevPhase_ = 1.0f;
    }

protected:
    // ========================================
    // Subclass hooks (must implement)
    // ========================================

    /**
     * Spawn particles based on current audio state
     * Called once per frame before particle updates
     */
    virtual void spawnParticles(float dt) = 0;

    /**
     * Update a single particle (physics, aging, visual effects)
     * Called for each alive particle
     */
    virtual void updateParticle(Particle* p, float dt) = 0;

    /**
     * Render a single particle to the matrix
     * Called for each alive particle after updates
     */
    virtual void renderParticle(const Particle* p, PixelMatrix& matrix) = 0;

    /**
     * Get color for particle based on intensity
     * Used by renderParticle
     */
    virtual uint32_t particleColor(uint8_t intensity) const = 0;

    // ========================================
    // Helper methods for subclasses
    // ========================================

    /**
     * Detect beat crossing (phase wraps from ~1.0 to ~0.0)
     */
    bool beatHappened() const {
        return audio_.phase < 0.2f && prevPhase_ > 0.8f;
    }

    /**
     * Check if particle is out of bounds
     */
    bool outOfBounds(const Particle* p) const {
        return p->x < 0 || p->x >= width_ || p->y < 0 || p->y >= height_;
    }

    /**
     * Apply standard physics forces to particle
     */
    void applyForces(Particle* p, float dt) {
        gravityForce_.apply(p, dt);
        windForce_.apply(p, dt);
        dragForce_.apply(p, dt);
    }

    /**
     * Age particle and apply fade if flagged
     */
    void ageParticle(Particle* p) {
        // Increment age, capping at 255 to prevent wraparound (zombie particles)
        if (p->age < 255) {
            p->age++;
        }

        if (p->hasFlag(ParticleFlags::FADE) && p->maxAge > 0) {
            // Linear fade based on age
            float ageRatio = (float)p->age / p->maxAge;
            uint8_t targetIntensity = p->intensity * (1.0f - ageRatio);
            p->intensity = max(0, min(255, targetIntensity));
        }
    }

    // ========================================
    // Update loop
    // ========================================

    void updateForces(float dt) {
        windForce_.update(dt);
    }

    void updateParticles(float dt) {
        pool_.updateAll([this, dt](Particle* p) {
            // Subclass-specific update
            updateParticle(p, dt);

            // Apply forces
            applyForces(p, dt);

            // Clamp velocity to prevent tunneling (max 50 LEDs/sec ≈ 1.65 LEDs/frame at 30 FPS)
            // This prevents particles from skipping through walls in a single frame
            const float MAX_VELOCITY = 50.0f;
            if (p->vx > MAX_VELOCITY) p->vx = MAX_VELOCITY;
            if (p->vx < -MAX_VELOCITY) p->vx = -MAX_VELOCITY;
            if (p->vy > MAX_VELOCITY) p->vy = MAX_VELOCITY;
            if (p->vy < -MAX_VELOCITY) p->vy = -MAX_VELOCITY;

            // Update position based on velocity
            // Velocity is in LEDs/sec, dt is in seconds, * TARGET_FPS normalizes frame rate
            // This makes motion consistent: at 30 FPS (dt≈0.033s), factor ≈ 1.0
            p->x += p->vx * dt * TARGET_FPS;
            p->y += p->vy * dt * TARGET_FPS;

            // Age particle
            ageParticle(p);

            // Handle bounds
            if (outOfBounds(p)) {
                if (p->hasFlag(ParticleFlags::BOUNCE)) {
                    // Bounce off walls
                    if (p->x < 0) { p->x = 0; p->vx = -p->vx * 0.8f; }
                    if (p->x >= width_) { p->x = width_ - 1; p->vx = -p->vx * 0.8f; }
                    if (p->y < 0) { p->y = 0; p->vy = -p->vy * 0.8f; }
                    if (p->y >= height_) { p->y = height_ - 1; p->vy = -p->vy * 0.8f; }
                } else {
                    // Kill particle
                    pool_.kill(p);
                }
            }
        });
    }

    void renderParticles(PixelMatrix& matrix) {
        pool_.forEach([this, &matrix](const Particle* p) {
            renderParticle(p, matrix);
        });
    }

    // ========================================
    // State
    // ========================================

    ParticlePool<MAX_PARTICLES> pool_;
    AudioControl audio_;
    float prevPhase_;
    // Note: lastUpdateMs_ is inherited from Generator base class

    // Forces
    GravityForce gravityForce_;
    WindForce windForce_;
    DragForce dragForce_;
};
