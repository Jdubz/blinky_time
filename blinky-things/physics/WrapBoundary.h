#pragma once

#include "BoundaryBehavior.h"

/**
 * WrapBoundary - Wrap particles around edges
 *
 * Perfect for circular LED arrangements like the hat brim.
 * Particles that exit one edge seamlessly appear at the opposite edge.
 */
class WrapBoundary : public BoundaryBehavior {
public:
    /**
     * @param wrapX Wrap on X axis (horizontal)
     * @param wrapY Wrap on Y axis (vertical)
     */
    WrapBoundary(bool wrapX = true, bool wrapY = false)
        : wrapX_(wrapX), wrapY_(wrapY) {}

    BoundaryAction checkBounds(const Particle* p,
                               uint16_t width, uint16_t height) const override {
        bool outX = p->x < 0 || p->x >= width;
        bool outY = p->y < 0 || p->y >= height;

        // If out on a wrapped axis, wrap it
        if ((outX && wrapX_) || (outY && wrapY_)) {
            return BoundaryAction::WRAP;
        }
        // If out on a non-wrapped axis, kill it
        if (outX || outY) {
            return BoundaryAction::KILL;
        }
        return BoundaryAction::NONE;
    }

    void applyCorrection(Particle* p, uint16_t width, uint16_t height) override {
        // CRITICAL: Guard against zero dimensions to prevent infinite loop
        if (wrapX_ && width > 0) {
            // Use fmod for safe wrapping (no infinite loop)
            p->x = fmod(p->x, (float)width);
            if (p->x < 0) p->x += width;
        }
        if (wrapY_ && height > 0) {
            p->y = fmod(p->y, (float)height);
            if (p->y < 0) p->y += height;
        }
    }

private:
    bool wrapX_, wrapY_;
};
