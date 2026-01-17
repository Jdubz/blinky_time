#pragma once

#include "ForceAdapter.h"
#include <Arduino.h>

/**
 * MatrixForceAdapter - Standard 2D force application
 *
 * For matrix layouts:
 * - Gravity affects Y axis (vertical): negative = up, positive = down
 * - Wind affects X axis (horizontal)
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
            float wind = baseWind_;
            if (windVariation_ > 0.0f) {
                // Add time-varying wind with sine wave
                // Use Y position for spatial variation
                wind += windVariation_ * sin(noisePhase_ + p->y * 0.1f);
            }
            // Mass affects wind response
            p->vx += (wind / p->mass) * dt;
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
        noisePhase_ += dt * 0.5f;
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
