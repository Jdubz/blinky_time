#pragma once

#include "../generators/Generator.h"
#include "ParticlePool.h"
#include "Particle.h"
#include "../physics/PhysicsContext.h"
#include "../physics/PropagationModel.h"
#include "../physics/SpawnRegion.h"
#include "../physics/BoundaryBehavior.h"
#include "../physics/ForceAdapter.h"
#include "../physics/BackgroundModel.h"
#include <Arduino.h>

/**
 * ParticleGenerator - Base class for particle-based generators
 *
 * Provides unified particle lifecycle management with layout-aware physics:
 * 1. Initialize physics context based on layout type
 * 2. Spawn particles using SpawnRegion abstraction
 * 3. Apply forces using ForceAdapter abstraction
 * 4. Update positions with velocity clamping
 * 5. Handle boundaries using BoundaryBehavior abstraction
 * 6. Render particles to matrix
 *
 * Subclasses customize behavior by implementing:
 * - initPhysicsContext(): Set up layout-appropriate physics
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
        : pool_(), prevPhase_(1.0f), gravity_(0.0f), drag_(0.98f),
          spawnRegion_(nullptr), boundary_(nullptr), forceAdapter_(nullptr) {}

    virtual ~ParticleGenerator() override = default;

    bool begin(const DeviceConfig& config) override {
        width_ = config.matrix.width;
        height_ = config.matrix.height;
        numLeds_ = width_ * height_;
        layout_ = config.matrix.layoutType;
        orientation_ = config.matrix.orientation;

        pool_.reset();
        lastUpdateMs_ = millis();

        // Initialize physics context - subclasses implement this
        initPhysicsContext();

        // DEBUG: Verify subclass properly initialized physics components
        // These are required for the particle system to function correctly.
        // Null pointers are handled gracefully at runtime, but indicate
        // a bug in the subclass initPhysicsContext() implementation.
        #ifdef BLINKY_DEBUG
        if (!spawnRegion_) {
            Serial.println(F("WARN: spawnRegion_ null after initPhysicsContext"));
        }
        if (!boundary_) {
            Serial.println(F("WARN: boundary_ null after initPhysicsContext"));
        }
        if (!forceAdapter_) {
            Serial.println(F("WARN: forceAdapter_ null after initPhysicsContext"));
        }
        #endif

        return true;
    }

    void generate(PixelMatrix& matrix, const AudioControl& audio) override {
        audio_ = audio;

        // Calculate delta time
        uint32_t currentMs = millis();
        float dt = (currentMs - lastUpdateMs_) / 1000.0f;
        lastUpdateMs_ = currentMs;

        // Update physics forces
        if (forceAdapter_) {
            forceAdapter_->update(dt);
        }

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
    // Physics context initialization
    // ========================================

    /**
     * Initialize physics context for this generator
     * Subclasses MUST override this to set up:
     * - spawnRegion_
     * - boundary_
     * - forceAdapter_
     * - (optionally) propagation_ and background_ if needed
     *
     * Use PhysicsContext factory methods with placement new.
     */
    virtual void initPhysicsContext() = 0;

    // ========================================
    // Subclass hooks (must implement)
    // ========================================

    /**
     * Spawn particles based on current audio state
     * Use spawnRegion_->getSpawnPosition() for layout-aware spawning
     */
    virtual void spawnParticles(float dt) = 0;

    /**
     * Update a single particle (physics, aging, visual effects)
     * Called for each alive particle before forces/boundaries
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
     * Detect beat crossing (phase wraps from high to low)
     */
    bool beatHappened() const {
        return audio_.phase < BEAT_PHASE_MIN && prevPhase_ > BEAT_PHASE_MAX;
    }

    /**
     * Get spawn position from spawn region
     * Returns layout-appropriate spawn coordinates
     */
    void getSpawnPosition(float& x, float& y) {
        if (spawnRegion_) {
            spawnRegion_->getSpawnPosition(x, y);
        } else {
            // Fallback: random position
            x = random(width_ * 100) / 100.0f;
            y = random(height_ * 100) / 100.0f;
        }
    }

    /**
     * Get initial velocity for spawned particles
     * Returns layout-appropriate velocity direction
     */
    void getInitialVelocity(float speed, float& vx, float& vy) {
        if (spawnRegion_) {
            spawnRegion_->getInitialVelocity(speed, vx, vy);
        } else {
            // Fallback: upward for fire-like behavior
            vx = 0;
            vy = -speed;
        }
    }

    /**
     * Age particle and apply fade if flagged
     * @param dt Delta time in seconds
     *
     * NOTE: age and maxAge are stored in centiseconds (0.01s units) to support
     * frame-rate-independent timing while maintaining uint8_t range (0-2.55s)
     * At 60fps (dt=0.0167s): age increments by 1-2 per frame
     * At 30fps (dt=0.033s): age increments by 3 per frame
     */
    void ageParticle(Particle* p, float dt) {
        // Increment age by dt in centiseconds (dt * 100)
        // Cap at 255 to prevent uint8_t wraparound
        float ageIncrement = dt * 100.0f;  // Convert seconds to centiseconds (0.01s units)
        float newAge = p->age + ageIncrement;  // Add in float space to preserve fractional values
        if (newAge < 255) {
            p->age = (uint8_t)newAge;  // Convert after addition to avoid truncation
        } else {
            p->age = 255;
        }

        if (p->hasFlag(ParticleFlags::FADE) && p->maxAge > 0) {
            // Linear fade based on age
            float ageRatio = (float)p->age / p->maxAge;
            float newIntensity = (float)p->intensity * (1.0f - ageRatio);
            p->intensity = (uint8_t)max(0.0f, newIntensity);
        }
    }

    // ========================================
    // Update loop
    // ========================================

    void updateParticles(float dt) {
        pool_.updateAll([this, dt](Particle* p) {
            // Subclass-specific update
            updateParticle(p, dt);

            // Apply forces through adapter
            if (forceAdapter_) {
                forceAdapter_->applyGravity(p, dt, gravity_);
                forceAdapter_->applyWind(p, dt);
                forceAdapter_->applyDrag(p, dt, drag_);
            }

            // CRITICAL: Clamp velocity BEFORE position update to prevent tunneling
            p->vx = constrain(p->vx, -MAX_PARTICLE_VELOCITY, MAX_PARTICLE_VELOCITY);
            p->vy = constrain(p->vy, -MAX_PARTICLE_VELOCITY, MAX_PARTICLE_VELOCITY);

            // Update position
            p->x += p->vx * dt;
            p->y += p->vy * dt;

            // Age particle (pass dt for time-based aging)
            ageParticle(p, dt);

            // Handle boundaries through abstraction
            if (boundary_) {
                BoundaryAction action = boundary_->checkBounds(p, width_, height_);
                switch (action) {
                    case BoundaryAction::KILL:
                        pool_.kill(p);
                        break;
                    case BoundaryAction::BOUNCE:
                    case BoundaryAction::WRAP:
                        boundary_->applyCorrection(p, width_, height_);
                        break;
                    case BoundaryAction::NONE:
                        break;
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

    // Physics parameters (set by subclasses)
    float gravity_;
    float drag_;

    // Physics components (created by initPhysicsContext)
    SpawnRegion* spawnRegion_;
    BoundaryBehavior* boundary_;
    ForceAdapter* forceAdapter_;

    // Static buffers for placement new (avoid heap allocation)
    uint8_t spawnBuffer_[32];
    uint8_t boundaryBuffer_[32];
    uint8_t forceBuffer_[48];
};
