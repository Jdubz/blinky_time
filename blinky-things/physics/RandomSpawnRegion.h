#pragma once

#include "SpawnRegion.h"
#include <Arduino.h>

/**
 * RandomSpawnRegion - Spawn particles at random positions
 *
 * Used for:
 * - Fire on linear: Embers appear at random spots along the string
 * - Lightning: Bolts originate at random positions
 * - Water on linear: Drops appear at random spots
 */
class RandomSpawnRegion : public SpawnRegion {
public:
    RandomSpawnRegion(uint16_t width, uint16_t height)
        : width_(width), height_(height) {}

    void getSpawnPosition(float& x, float& y) override {
        x = random(width_ * 100) / 100.0f;
        y = random(max((uint16_t)1, height_) * 100) / 100.0f;
    }

    void getInitialVelocity(float speed, float& vx, float& vy) const override {
        // For random spawn, give random velocity direction
        // Biased toward horizontal for linear layouts
        float angle = random(360) * DEG_TO_RAD;
        vx = cos(angle) * speed;
        vy = sin(angle) * speed * 0.3f;  // Reduced vertical component
    }

private:
    uint16_t width_, height_;
};
