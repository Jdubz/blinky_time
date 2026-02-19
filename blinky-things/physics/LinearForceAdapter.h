#pragma once

#include "ForceAdapter.h"
#include "../math/SimplexNoise.h"
#include <Arduino.h>

/**
 * LinearForceAdapter - 1D force application for linear layouts
 *
 * For linear layouts (hat brim):
 * - Gravity affects X axis (horizontal "pull")
 * - Wind affects X axis
 * - Creates lateral spreading effect
 *
 * This maps the semantic concept of "gravity" (fire rises, water falls)
 * to horizontal motion for 1D arrangements.
 */
class LinearForceAdapter : public ForceAdapter {
public:
    LinearForceAdapter()
        : baseWind_(0.0f), windVariation_(0.0f), noisePhase_(0.0f) {}

    void applyGravity(Particle* p, float dt, float gravityMagnitude) override {
        if (p->hasFlag(ParticleFlags::GRAVITY)) {
            // For linear: gravity affects X velocity (lateral pull)
            // This creates spreading effect rather than rising/falling
            p->vx += gravityMagnitude * dt;
        }
    }

    void applyWind(Particle* p, float dt) override {
        if (p->hasFlag(ParticleFlags::WIND)) {
            // Base wind: force/acceleration for sustained directional drift
            p->vx += (baseWind_ / p->mass) * dt;

            if (windVariation_ > 0.0f) {
                // Turbulence as flow-field advection (same reasoning as MatrixForceAdapter)
                float turbulence = SimplexNoise::fbm3D(
                    p->x * 0.15f,       // Spatial frequency along strip
                    noisePhase_ * 0.6f, // Time variation
                    0.0f,
                    2,
                    0.5f
                );

                // Direct position advection: windVariation is LEDs/sec of displacement
                p->x += windVariation_ * turbulence * dt;
            }
        }
    }

    void applyDrag(Particle* p, float dt, float dragCoeff) override {
        // Same drag behavior as matrix - affects both axes
        float safeDt = min(dt, 1.0f);
        float k = 1.0f - dragCoeff;
        float damping = 1.0f - k * safeDt;
        p->vx *= damping;
        p->vy *= damping;
    }

    void update(float dt) override {
        noisePhase_ += dt * 3.0f;  // Increased from 0.5 to 3.0 for faster variation
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
