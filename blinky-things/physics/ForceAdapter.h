#pragma once

#include "../particles/Particle.h"

/**
 * ForceAdapter - Layout-aware force application
 *
 * Wraps force application and maps them to the appropriate axis
 * for the layout:
 * - MATRIX: gravity affects vy (vertical), wind affects vx
 * - LINEAR: gravity affects vx (lateral "pull"), wind affects vx
 *
 * This allows the same "gravity" concept to work across layouts
 * while producing visually appropriate results.
 */
class ForceAdapter {
public:
    virtual ~ForceAdapter() = default;

    /**
     * Apply gravity in layout-appropriate direction
     * For fire: negative gravity makes sparks "rise"
     * For water: positive gravity makes drops "fall"
     * @param p Particle to apply force to
     * @param dt Delta time in seconds
     * @param gravityMagnitude Gravity strength (positive = toward "ground")
     */
    virtual void applyGravity(Particle* p, float dt, float gravityMagnitude) = 0;

    /**
     * Apply wind force
     * @param p Particle to apply force to
     * @param dt Delta time in seconds
     */
    virtual void applyWind(Particle* p, float dt) = 0;

    /**
     * Apply drag (velocity damping)
     * @param p Particle to apply force to
     * @param dt Delta time in seconds
     * @param dragCoeff Drag coefficient (0-1, closer to 1 = less drag)
     */
    virtual void applyDrag(Particle* p, float dt, float dragCoeff) = 0;

    /**
     * Update time-varying forces (wind noise, etc.)
     * Call once per frame before applying forces
     * @param dt Delta time in seconds
     */
    virtual void update(float dt) = 0;

    /**
     * Configure wind parameters
     * @param baseWind Base wind force
     * @param variation Wind variation amount
     */
    virtual void setWind(float baseWind, float variation) = 0;
};
