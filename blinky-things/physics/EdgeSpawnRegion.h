#pragma once

#include "SpawnRegion.h"
#include <Arduino.h>

/**
 * Edge - Which edge to spawn from
 */
enum class Edge {
    TOP,
    BOTTOM,
    LEFT,
    RIGHT
};

/**
 * EdgeSpawnRegion - Spawn particles from a specific edge
 *
 * Used for:
 * - Fire on matrix: BOTTOM edge, particles rise upward
 * - Water on matrix: TOP edge, particles fall downward
 */
class EdgeSpawnRegion : public SpawnRegion {
public:
    EdgeSpawnRegion(Edge edge, uint16_t width, uint16_t height)
        : edge_(edge), width_(width), height_(height) {}

    void getSpawnPosition(float& x, float& y) override {
        switch (edge_) {
            case Edge::TOP:
                x = random(width_ * 100) / 100.0f;
                y = 0;
                break;
            case Edge::BOTTOM:
                x = random(width_ * 100) / 100.0f;
                y = height_ - 1;
                break;
            case Edge::LEFT:
                x = 0;
                y = random(height_ * 100) / 100.0f;
                break;
            case Edge::RIGHT:
            default:  // Default to right edge if edge_ is corrupted
                x = width_ - 1;
                y = random(height_ * 100) / 100.0f;
                break;
        }
    }

    bool isInRegion(float x, float y) const override {
        switch (edge_) {
            case Edge::TOP: return y < 1.0f;
            case Edge::BOTTOM: return y >= height_ - 1;
            case Edge::LEFT: return x < 1.0f;
            case Edge::RIGHT: return x >= width_ - 1;
        }
        return false;
    }

    void getCenter(float& x, float& y) const override {
        switch (edge_) {
            case Edge::TOP:
                x = width_ / 2.0f;
                y = 0;
                break;
            case Edge::BOTTOM:
                x = width_ / 2.0f;
                y = height_ - 1;
                break;
            case Edge::LEFT:
                x = 0;
                y = height_ / 2.0f;
                break;
            case Edge::RIGHT:
                x = width_ - 1;
                y = height_ / 2.0f;
                break;
        }
    }

    void getInitialVelocity(float speed, float& vx, float& vy) const override {
        switch (edge_) {
            case Edge::TOP:
                // Spawn from top, move downward
                vx = 0;
                vy = speed;
                break;
            case Edge::BOTTOM:
                // Spawn from bottom, move upward (fire)
                vx = 0;
                vy = -speed;
                break;
            case Edge::LEFT:
                // Spawn from left, move right
                vx = speed;
                vy = 0;
                break;
            case Edge::RIGHT:
            default:  // Default to right edge behavior if edge_ is corrupted
                // Spawn from right, move left
                vx = -speed;
                vy = 0;
                break;
        }
    }

    Edge getEdge() const { return edge_; }

private:
    Edge edge_;
    uint16_t width_, height_;
};
