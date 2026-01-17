#pragma once

#include "ForceAdapter.h"
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
            float wind = baseWind_;
            if (windVariation_ > 0.0f) {
                // Use X position for spatial variation (instead of Y)
                wind += windVariation_ * sin(noisePhase_ + p->x * 0.1f);
            }
            // Mass affects wind response
            p->vx += (wind / p->mass) * dt;
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
        noisePhase_ += dt * 0.5f;
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
