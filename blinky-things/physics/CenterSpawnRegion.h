#pragma once

#include "SpawnRegion.h"
#include <Arduino.h>

/**
 * CenterSpawnRegion - Spawn particles from center with spread
 *
 * Spawns particles within a percentage of the center.
 * Useful for effects that expand outward from a focal point.
 */
class CenterSpawnRegion : public SpawnRegion {
public:
    /**
     * @param width Grid width
     * @param height Grid height
     * @param spread Spawn spread as fraction of dimensions (0.0-1.0)
     */
    CenterSpawnRegion(uint16_t width, uint16_t height, float spread = 0.2f)
        : width_(width), height_(height), spread_(spread) {}

    void getSpawnPosition(float& x, float& y) override {
        float centerX = width_ / 2.0f;
        float centerY = height_ / 2.0f;

        // Spawn within spread percentage of center
        float spreadX = width_ * spread_;
        float spreadY = height_ * spread_;

        x = centerX + (random(200) - 100) / 100.0f * spreadX;
        y = centerY + (random(200) - 100) / 100.0f * spreadY;

        // Clamp to valid range
        x = constrain(x, 0.0f, (float)(width_ - 1));
        y = constrain(y, 0.0f, (float)(height_ - 1));
    }

    bool isInRegion(float x, float y) const override {
        float centerX = width_ / 2.0f;
        float centerY = height_ / 2.0f;
        float dx = abs(x - centerX) / max(1, (int)width_);
        float dy = abs(y - centerY) / max(1, (int)height_);
        return dx <= spread_ && dy <= spread_;
    }

    void getCenter(float& x, float& y) const override {
        x = width_ / 2.0f;
        y = height_ / 2.0f;
    }

    void getInitialVelocity(float speed, float& vx, float& vy) const override {
        // Radial expansion from center
        float angle = random(360) * DEG_TO_RAD;
        vx = cos(angle) * speed;
        vy = sin(angle) * speed;
    }

private:
    uint16_t width_, height_;
    float spread_;
};
