#pragma once

#include "Particle.h"
#include <string.h>

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
                particles_[i].mass = mass;
                particles_[i].flags = flags;
                activeCount_++;
                return &particles_[i];
            }
        }
        return nullptr;  // Pool exhausted
    }

    /**
     * Kill a particle (returns it to pool)
     */
    void kill(Particle* p) {
        if (p && p->isAlive()) {
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
                updateFunc(&particles_[i]);

                // Auto-kill if intensity drops to 0 or age exceeds maxAge
                if (!particles_[i].isAlive() && activeCount_ > 0) {
                    activeCount_--;
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
