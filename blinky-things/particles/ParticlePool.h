#pragma once

#include "Particle.h"
#include <new>

/**
 * ParticlePool - Dynamically-sized particle pool
 *
 * Memory: 24 bytes/particle * capacity + overhead
 * Pool is allocated once in begin() and never freed — same zero-fragmentation
 * guarantee as a static array, but sized to match the actual device.
 *
 * Particles are reused from the pool when they die (intensity=0 or age>=maxAge).
 */
class ParticlePool {
public:
    ParticlePool() : particles_(nullptr), capacity_(0), activeCount_(0) {}

    ~ParticlePool() {
        delete[] particles_;
    }

    /**
     * Allocate the pool. Called once during generator begin().
     * @param capacity Number of particles to allocate
     * @return true if allocation succeeded
     */
    bool begin(uint16_t capacity) {
        // Free any previous allocation (supports re-init if device config changes)
        delete[] particles_;
        particles_ = nullptr;
        capacity_ = 0;
        activeCount_ = 0;

        if (capacity == 0) return false;

        particles_ = new(std::nothrow) Particle[capacity];
        if (!particles_) return false;

        capacity_ = capacity;

        // Initialize all particles as dead
        for (uint16_t i = 0; i < capacity_; i++) {
            particles_[i].intensity = 0;
            particles_[i].age = 255;
        }

        return true;
    }

    /**
     * Spawn a new particle with given parameters
     * Returns pointer to spawned particle, or nullptr if pool is full
     */
    Particle* spawn(float x, float y, float vx, float vy,
                    uint8_t intensity, uint8_t maxAge,
                    float mass, uint8_t flags) {
        if (!particles_) return nullptr;

        // Validate inputs to prevent NaN, infinity, and extreme values
        if (!isfinite(x) || !isfinite(y) || !isfinite(vx) || !isfinite(vy) || !isfinite(mass)) {
            return nullptr;  // Reject invalid floating-point values
        }
        if (intensity == 0) {
            return nullptr;  // Don't spawn invisible particles
        }

        // Find first dead particle
        for (uint16_t i = 0; i < capacity_; i++) {
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
                if (activeCount_ < capacity_) {
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
        if (!particles_ || !p || p < particles_ || p >= particles_ + capacity_) {
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
        if (!particles_) return;
        for (uint16_t i = 0; i < capacity_; i++) {
            if (particles_[i].isAlive()) {
                // Capture count before update to detect explicit kill() calls
                uint16_t beforeCount = activeCount_;
                updateFunc(&particles_[i]);

                // Auto-kill if died naturally, but only if the update didn't already change activeCount_
                // (explicit kill() will have already decremented activeCount_)
                if (!particles_[i].isAlive() &&
                    // cppcheck-suppress knownConditionTrueFalse
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
        if (!particles_) return;
        for (uint16_t i = 0; i < capacity_; i++) {
            if (particles_[i].isAlive()) {
                iterFunc(&particles_[i]);
            }
        }
    }

    /**
     * Clear all particles
     */
    void reset() {
        if (!particles_) return;
        for (uint16_t i = 0; i < capacity_; i++) {
            particles_[i].intensity = 0;
            particles_[i].age = 255;
        }
        activeCount_ = 0;
    }

    // Accessors
    uint16_t getActiveCount() const { return activeCount_; }
    uint16_t getCapacity() const { return capacity_; }
    bool isFull() const { return activeCount_ >= capacity_; }
    bool isInitialized() const { return particles_ != nullptr; }

private:
    Particle* particles_;
    uint16_t capacity_;
    uint16_t activeCount_;

    // Non-copyable (owns heap allocation)
    ParticlePool(const ParticlePool&) = delete;
    ParticlePool& operator=(const ParticlePool&) = delete;
};
