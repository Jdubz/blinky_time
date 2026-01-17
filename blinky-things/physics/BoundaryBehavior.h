#pragma once

#include "../particles/Particle.h"
#include <stdint.h>

/**
 * BoundaryAction - What to do when particle hits boundary
 */
enum class BoundaryAction {
    NONE,       // Particle is within bounds
    KILL,       // Kill the particle
    BOUNCE,     // Bounce off the boundary
    WRAP        // Wrap to opposite edge
};

/**
 * BoundaryBehavior - Abstract edge handling strategy
 *
 * Defines what happens when particles reach grid edges.
 * Different layouts may want different behaviors:
 * - MATRIX: Kill at top (fire rises out), bounce on sides
 * - LINEAR with wrap: Wrap around (circular hat brim)
 * - LINEAR without wrap: Bounce at ends
 */
class BoundaryBehavior {
public:
    virtual ~BoundaryBehavior() = default;

    /**
     * Check if particle is out of bounds and determine action
     * @param p The particle to check
     * @param width Grid width
     * @param height Grid height
     * @return Action to take
     */
    virtual BoundaryAction checkBounds(const Particle* p,
                                       uint16_t width, uint16_t height) const = 0;

    /**
     * Apply boundary correction to particle
     * Called after checkBounds returns BOUNCE or WRAP
     * @param p The particle to correct
     * @param width Grid width
     * @param height Grid height
     */
    virtual void applyCorrection(Particle* p,
                                 uint16_t width, uint16_t height) = 0;
};
