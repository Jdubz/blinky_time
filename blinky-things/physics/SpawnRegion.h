#pragma once

#include <stdint.h>

/**
 * SpawnRegion - Abstract particle spawn position strategy
 *
 * Defines where new particles originate. Different effects need
 * different spawn patterns:
 * - Fire: From "source" edge (bottom for matrix, random for linear)
 * - Water: From "top" edge (top for matrix, random for linear)
 * - Lightning: Random positions
 */
class SpawnRegion {
public:
    virtual ~SpawnRegion() = default;

    /**
     * Get a spawn position
     * @param x Output X coordinate (0.0 to width)
     * @param y Output Y coordinate (0.0 to height)
     */
    virtual void getSpawnPosition(float& x, float& y) = 0;

    /**
     * Check if a position is within the spawn region
     * Used for splash effects that spawn near a specific location
     */
    virtual bool isInRegion(float x, float y) const = 0;

    /**
     * Get the spawn region's "center" for radial effects
     */
    virtual void getCenter(float& x, float& y) const = 0;

    /**
     * Get initial velocity direction for particles spawned in this region
     * This provides layout-appropriate velocity vectors
     * @param speed Desired speed magnitude
     * @param vx Output X velocity component
     * @param vy Output Y velocity component
     */
    virtual void getInitialVelocity(float speed, float& vx, float& vy) const = 0;
};
