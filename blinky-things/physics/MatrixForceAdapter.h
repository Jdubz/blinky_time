#pragma once

#include "ForceAdapter.h"
#include "../math/SimplexNoise.h"
#include <Arduino.h>

/**
 * MatrixForceAdapter - Standard 2D force application
 *
 * For matrix layouts:
 * - Gravity affects Y axis (vertical): negative = up, positive = down
 * - Wind affects both axes (curl noise creates 2D swirling turbulence)
 * - Drag affects both axes
 */
class MatrixForceAdapter : public ForceAdapter {
public:
    MatrixForceAdapter()
        : baseWind_(0.0f), windVariation_(0.0f), noisePhase_(0.0f) {}

    void applyGravity(Particle* p, float dt, float gravityMagnitude) override {
        if (p->hasFlag(ParticleFlags::GRAVITY)) {
            p->vy += gravityMagnitude * dt;
        }
    }

    void applyWind(Particle* p, float dt) override {
        if (p->hasFlag(ParticleFlags::WIND)) {
            // Base wind: applied as a force/acceleration (sustained directional drift)
            p->vx += (baseWind_ / p->mass) * dt;

            if (windVariation_ > 0.0f) {
                // CURL NOISE TURBULENCE — applied as flow-field advection, not force.
                //
                // Why advection instead of force (vx += force*dt):
                //   Force accumulates over many frames before becoming visible.
                //   On a small 8-row matrix with fast particles (exit in ~20 frames),
                //   force-based wind only displaces particles ~1 LED laterally even at
                //   windVariation=50. Completely invisible.
                //
                //   Advection (x += velocity*dt) makes windVariation the *displacement
                //   rate* in LEDs/sec. At windVariation=10, a particle moves 0.17 LEDs
                //   per frame laterally — clearly visible in its ~19-frame lifetime.
                //
                // scale = 0.25: on a 16-LED grid, spans 4 noise units → several full
                // variation cycles so adjacent particles feel different forces.
                const float scale = 0.25f;
                const float offset = 100.0f;

                float noiseX = SimplexNoise::fbm3D(
                    p->x * scale,
                    (p->y + offset) * scale,
                    noisePhase_ * 0.5f,
                    3, 0.6f
                );

                float noiseY = SimplexNoise::fbm3D(
                    (p->x + offset) * scale,
                    p->y * scale,
                    noisePhase_ * 0.5f,
                    3, 0.6f
                );

                // Direct position advection: windVariation is LEDs/sec of displacement
                p->x += windVariation_ * noiseX * dt;
                p->y += windVariation_ * noiseY * dt;
            }
        }
    }

    void applyDrag(Particle* p, float dt, float dragCoeff) override {
        // Simple velocity damping using first-order approximation
        // Clamp dt to prevent excessive damping
        float safeDt = min(dt, 1.0f);
        float k = 1.0f - dragCoeff;
        float damping = 1.0f - k * safeDt;
        p->vx *= damping;
        p->vy *= damping;
    }

    void update(float dt) override {
        noisePhase_ += dt * 3.0f;
        // Wrap phase to prevent unbounded growth
        if (noisePhase_ > TWO_PI) {
            noisePhase_ -= TWO_PI;
        }
    }

    void setWind(float baseWind, float variation) override {
        baseWind_ = baseWind;
        windVariation_ = variation;
    }

private:
    float baseWind_;
    float windVariation_;
    float noisePhase_;
};
