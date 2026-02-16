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
            if (windVariation_ > 0.0f) {
                // CURL NOISE TURBULENCE
                const float scale = 0.06f;
                const float offset = 100.0f;

                float noiseForVx = SimplexNoise::fbm3D(
                    p->x * scale,
                    (p->y + offset) * scale,
                    noisePhase_ * 0.3f,
                    3, 0.6f
                );

                float noiseForVy = SimplexNoise::fbm3D(
                    (p->x + offset) * scale,
                    p->y * scale,
                    noisePhase_ * 0.3f,
                    3, 0.6f
                );

                float windX = baseWind_ + windVariation_ * noiseForVx;
                float windY = windVariation_ * noiseForVy;

                p->vx += (windX / p->mass) * dt;
                p->vy += (windY / p->mass) * dt;
            } else {
                // No turbulence, just base wind
                p->vx += (baseWind_ / p->mass) * dt;
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
