#pragma once

#include "Particle.h"

/**
 * ParticlePool - Fixed-size particle pool with zero heap allocation
 *
 * Memory: 24 bytes/particle * N particles + overhead
 * - 32 particles = 776 bytes
 * - 64 particles = 1544 bytes
 * - 128 particles = 3080 bytes
 *
 * Uses compile-time fixed size to eliminate heap fragmentation.
 * Particles are reused from a static pool when they die.
 */
template<uint8_t MAX_PARTICLES>
class ParticlePool {
public:
    ParticlePool() : activeCount_(0) {
        // Initialize all particles as dead
        for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
            particles_[i].intensity = 0;
            particles_[i].age = 255;
        }
    }

    /**
     * Spawn a new particle with given parameters
     * Returns pointer to spawned particle, or nullptr if pool is full
     */
    Particle* spawn(float x, float y, float vx, float vy,
                    uint8_t intensity, uint8_t maxAge,
                    float mass, uint8_t flags) {
        // Find first dead particle
        for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
            if (!particles_[i].isAlive()) {
                particles_[i].x = x;
                particles_[i].y = y;
                particles_[i].vx = vx;
                particles_[i].vy = vy;
                particles_[i].intensity = intensity;
                particles_[i].age = 0;
                particles_[i].maxAge = maxAge;
                // Clamp mass to prevent division by zero and numerical issues
                // Min 0.01 prevents /0, max 10.0 prevents particles becoming too sluggish
                float clampedMass = mass;
                if (clampedMass < 0.01f) clampedMass = 0.01f;
                if (clampedMass > 10.0f) clampedMass = 10.0f;
                particles_[i].mass = clampedMass;
                particles_[i].flags = flags;
                // Increment count with overflow protection
                if (activeCount_ < MAX_PARTICLES) {
                    activeCount_++;
                }
                return &particles_[i];
            }
        }
        return nullptr;  // Pool exhausted
    }

    /**
     * Kill a particle (returns it to pool)
     */
    void kill(Particle* p) {
        // Validate pointer is within pool bounds
        if (!p || p < particles_ || p >= particles_ + MAX_PARTICLES) {
            return;  // Invalid pointer, ignore
        }

        if (p->isAlive()) {
            p->intensity = 0;
            p->age = 255;
            if (activeCount_ > 0) {
                activeCount_--;
            }
        }
    }

    /**
     * Update all active particles
     * Calls provided update function for each alive particle
     * Template parameter allows any callable (lambda, function pointer, functor)
     */
    template<typename UpdateFunc>
    void updateAll(UpdateFunc updateFunc) {
        for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
            if (particles_[i].isAlive()) {
                // Capture count before update to detect explicit kill() calls
                uint8_t beforeCount = activeCount_;
                updateFunc(&particles_[i]);

                // Auto-kill if died naturally, but only if the update didn't already change activeCount_
                // (explicit kill() will have already decremented activeCount_)
                if (!particles_[i].isAlive() &&
                    activeCount_ == beforeCount &&
                    activeCount_ > 0) {
                    activeCount_--;
                    particles_[i].intensity = 0;
                    particles_[i].age = 255;
                }
            }
        }
    }

    /**
     * Iterate over all alive particles (read-only)
     * Template parameter allows any callable (lambda, function pointer, functor)
     */
    template<typename IterFunc>
    void forEach(IterFunc iterFunc) const {
        for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
            if (particles_[i].isAlive()) {
                iterFunc(&particles_[i]);
            }
        }
    }

    /**
     * Clear all particles
     */
    void reset() {
        for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
            particles_[i].intensity = 0;
            particles_[i].age = 255;
        }
        activeCount_ = 0;
    }

    // Accessors
    uint8_t getActiveCount() const { return activeCount_; }
    uint8_t getCapacity() const { return MAX_PARTICLES; }
    bool isFull() const { return activeCount_ >= MAX_PARTICLES; }

private:
    Particle particles_[MAX_PARTICLES];
    uint8_t activeCount_;
};
