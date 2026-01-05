#pragma once

#include "Particle.h"
#include <Arduino.h>

/**
 * ParticleForce - Abstract base for particle forces
 *
 * Forces modify particle velocity based on particle properties.
 * Multiple forces can be applied to the same particle.
 */
class ParticleForce {
public:
    virtual ~ParticleForce() = default;

    /**
     * Apply force to particle (modifies velocity)
     * @param p Particle to apply force to
     * @param dt Delta time in seconds
     */
    virtual void apply(Particle* p, float dt) = 0;
};

/**
 * GravityForce - Constant downward/upward acceleration
 *
 * Positive gravity = downward (water drops)
 * Negative gravity = upward (fire sparks rise)
 */
class GravityForce : public ParticleForce {
public:
    explicit GravityForce(float g = 9.8f) : gravity_(g) {}

    void apply(Particle* p, float dt) override {
        if (p->hasFlag(ParticleFlags::GRAVITY)) {
            // F = ma -> a = F/m -> dv = (F/m)*dt
            p->vy += (gravity_ / p->mass) * dt;
        }
    }

    void setGravity(float g) { gravity_ = g; }
    float getGravity() const { return gravity_; }

private:
    float gravity_;  // Acceleration in LEDs/sec² (applied with dt normalization)
};

/**
 * WindForce - Horizontal force with sine wave variation
 *
 * Creates turbulence effect using time-varying wind strength
 */
class WindForce : public ParticleForce {
public:
    explicit WindForce(float baseWind = 0.0f, float variation = 0.0f)
        : baseWind_(baseWind), variation_(variation), noisePhase_(0.0f) {}

    void apply(Particle* p, float dt) override {
        if (p->hasFlag(ParticleFlags::WIND)) {
            // Add time-varying wind with sine wave (cheaper than noise)
            float wind = baseWind_;
            if (variation_ > 0.0f) {
                wind += variation_ * sin(noisePhase_ + p->y * 0.1f);
            }
            p->vx += (wind / p->mass) * dt;
        }
    }

    /**
     * Update wind phase (call once per frame)
     */
    void update(float dt) {
        noisePhase_ += dt * 0.5f;  // Slow phase evolution
    }

    void setWind(float base, float var) {
        baseWind_ = base;
        variation_ = var;
    }

private:
    float baseWind_;     // Base horizontal force
    float variation_;    // Amount of variation
    float noisePhase_;   // Noise phase for variation
};

/**
 * DragForce - Velocity damping (air resistance)
 *
 * Reduces particle velocity over time.
 * Higher drag coefficient = less drag (1.0 = no drag, 0.0 = instant stop)
 */
class DragForce : public ParticleForce {
public:
    explicit DragForce(float coefficient = 0.98f) : dragCoeff_(coefficient) {}

    void apply(Particle* p, float dt) override {
        // Simple velocity damping: v *= dragCoeff^dt
        // For dt ~= 0.03s (33 FPS), dragCoeff=0.98 means ~60% velocity after 1 sec
        float damping = pow(dragCoeff_, dt * 30.0f);  // Normalize to 30 FPS
        p->vx *= damping;
        p->vy *= damping;
    }

    void setDrag(float coeff) { dragCoeff_ = coeff; }
    float getDrag() const { return dragCoeff_; }

private:
    float dragCoeff_;  // Drag coefficient (0-1, closer to 1 = less drag)
};

/**
 * RadialForce - Radial expansion from center point
 *
 * Used for splash effects - particles pushed outward from impact point
 */
class RadialForce : public ParticleForce {
public:
    RadialForce(float centerX, float centerY, float strength)
        : centerX_(centerX), centerY_(centerY), strength_(strength) {}

    void apply(Particle* p, float dt) override {
        if (p->hasFlag(ParticleFlags::RADIAL)) {
            float dx = p->x - centerX_;
            float dy = p->y - centerY_;
            float dist = sqrt(dx*dx + dy*dy);

            if (dist > 0.01f) {  // Avoid division by zero
                // Normalize direction and apply force
                float forceX = (dx / dist) * strength_ / p->mass;
                float forceY = (dy / dist) * strength_ / p->mass;
                p->vx += forceX * dt;
                p->vy += forceY * dt;
            }
        }
    }

    void setCenter(float x, float y) {
        centerX_ = x;
        centerY_ = y;
    }

    void setStrength(float s) { strength_ = s; }

private:
    float centerX_, centerY_;
    float strength_;
};
