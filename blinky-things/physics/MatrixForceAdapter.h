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
                // CURL NOISE TURBULENCE (Bridson, SIGGRAPH 2007)
                //
                // Take the curl of a scalar noise field to get a divergence-free
                // velocity field. Particles swirl around each other instead of
                // being pushed independently. The curl of a 2D scalar field N is:
                //   curl_x =  dN/dy
                //   curl_y = -dN/dx
                // Approximated with finite differences (eps = 0.5 LED).
                // Uses 2-octave fbm for smooth flowing motion (raw noise3D is too jagged).
                //
                // Applied as advection (position displacement) not force, because
                // force accumulates too slowly on small matrices (~20 frame lifetime).
                const float scale = 0.25f;
                const float eps = 0.5f;
                float px = p->x * scale;
                float py = p->y * scale;
                float t = noisePhase_ * 0.5f;
                float epsScaled = eps * scale;

                // Finite-difference curl: 4 noise evals per particle (1 octave).
                // Single octave is sufficient for smooth wind turbulence and halves cost.
                float dNdy = SimplexNoise::noise3D(px, py + epsScaled, t)
                           - SimplexNoise::noise3D(px, py - epsScaled, t);
                float dNdx = SimplexNoise::noise3D(px + epsScaled, py, t)
                           - SimplexNoise::noise3D(px - epsScaled, py, t);

                // Curl direction (unnormalized, ~[-2,2] range)
                float curlX =  dNdy;
                float curlY = -dNdx;

                // Advection: windVariation is LEDs/sec of displacement
                p->x += windVariation_ * curlX * dt;
                p->y += windVariation_ * curlY * dt;
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
