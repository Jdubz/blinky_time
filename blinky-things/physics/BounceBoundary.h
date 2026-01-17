#pragma once

#include "BoundaryBehavior.h"

/**
 * BounceBoundary - Bounce particles off edges
 *
 * Used for lightning effects where bolts should stay visible
 * longer by bouncing off walls. Velocity is reversed and
 * damped (80% retention) to prevent infinite bouncing.
 */
class BounceBoundary : public BoundaryBehavior {
public:
    explicit BounceBoundary(float damping = 0.8f) : damping_(damping) {}

    BoundaryAction checkBounds(const Particle* p,
                               uint16_t width, uint16_t height) const override {
        if (p->x < 0 || p->x >= width || p->y < 0 || p->y >= height) {
            return BoundaryAction::BOUNCE;
        }
        return BoundaryAction::NONE;
    }

    void applyCorrection(Particle* p, uint16_t width, uint16_t height) override {
        // Bounce off left edge
        if (p->x < 0) {
            p->x = 0;
            p->vx = -p->vx * damping_;
        }
        // Bounce off right edge
        if (p->x >= width) {
            p->x = width - 0.001f;
            p->vx = -p->vx * damping_;
        }
        // Bounce off top edge
        if (p->y < 0) {
            p->y = 0;
            p->vy = -p->vy * damping_;
        }
        // Bounce off bottom edge
        if (p->y >= height) {
            p->y = height - 0.001f;
            p->vy = -p->vy * damping_;
        }
    }

private:
    float damping_;
};
