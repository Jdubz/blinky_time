#pragma once

#include <stdint.h>

/**
 * Particle - Core particle data structure for unified generator system
 *
 * Memory: 24 bytes per particle
 * - Position: 8 bytes (float x, y)
 * - Velocity: 8 bytes (float vx, vy)
 * - State: 8 bytes (uint8_t intensity, age, maxAge, flags; float mass)
 *
 * Supports sparks (Fire), drops/splashes (Water), and bolts (Lightning)
 * through configurable behavior flags and physics properties.
 */
struct Particle {
    // Position (in LED coordinate space)
    float x, y;           // 8 bytes - Fractional position for smooth subpixel movement

    // Velocity (LEDs per update)
    float vx, vy;         // 8 bytes - Velocity vector

    // Lifecycle
    uint8_t intensity;    // 1 byte - Current brightness/heat (0-255)
    uint8_t age;          // 1 byte - Frames since spawn (0-255, wraps)
    uint8_t maxAge;       // 1 byte - Death age (0=infinite, 1-255=lifespan frames)
    uint8_t flags;        // 1 byte - Behavior flags (see ParticleFlags)

    // Physics
    float mass;           // 4 bytes - Mass for force calculations (0.1-2.0 typical)

    // === TOTAL: 24 bytes ===

    /**
     * Check if particle is alive
     * A particle is alive if it has intensity and hasn't exceeded max age
     */
    inline bool isAlive() const {
        return intensity > 0 && (maxAge == 0 || age < maxAge);
    }

    /**
     * Check if particle has specific flag
     */
    inline bool hasFlag(uint8_t flag) const {
        return (flags & flag) != 0;
    }

    /**
     * Set a behavior flag
     */
    inline void setFlag(uint8_t flag) {
        flags |= flag;
    }

    /**
     * Clear a behavior flag
     */
    inline void clearFlag(uint8_t flag) {
        flags &= ~flag;
    }
};

/**
 * ParticleFlags - Particle behavior flags (bitfield)
 *
 * These flags control particle behavior during update and rendering.
 * Multiple flags can be combined using bitwise OR.
 */
namespace ParticleFlags {
    constexpr uint8_t NONE           = 0x00;  // No special behavior
    constexpr uint8_t EMIT_TRAIL     = 0x01;  // Leave trail/heat behind (Fire sparks)
    constexpr uint8_t BOUNCE         = 0x02;  // Bounce off bounds instead of dying
    constexpr uint8_t FADE           = 0x04;  // Fade intensity over lifetime
    constexpr uint8_t BRANCH         = 0x08;  // Can spawn child particles (Lightning)
    constexpr uint8_t SPLASH         = 0x10;  // Spawn splash particles on death (Water)
    constexpr uint8_t GRAVITY        = 0x20;  // Affected by gravity force
    constexpr uint8_t WIND           = 0x40;  // Affected by wind force
    constexpr uint8_t RADIAL         = 0x80;  // Radial expansion from spawn point
}
