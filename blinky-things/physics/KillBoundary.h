#pragma once

#include "BoundaryBehavior.h"

/**
 * KillBoundary - Kill particles when they exit bounds
 *
 * Standard behavior for fire on matrix layouts where
 * sparks should disappear when they rise out of view.
 */
class KillBoundary : public BoundaryBehavior {
public:
    BoundaryAction checkBounds(const Particle* p,
                               uint16_t width, uint16_t height) const override {
        if (p->x < 0 || p->x >= width || p->y < 0 || p->y >= height) {
            return BoundaryAction::KILL;
        }
        return BoundaryAction::NONE;
    }

    void applyCorrection(Particle* p, uint16_t width, uint16_t height) override {
        // No correction needed - particle will be killed
        (void)p;
        (void)width;
        (void)height;
    }
};
