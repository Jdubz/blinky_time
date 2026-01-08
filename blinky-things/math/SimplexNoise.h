#pragma once
/**
 * SimplexNoise.h - Fast simplex noise for embedded systems
 *
 * Provides 2D and 3D simplex noise for organic backgrounds and effects.
 * Optimized for nRF52840 (no heap, minimal memory, fast computation).
 *
 * Usage:
 *   float n = SimplexNoise::noise2D(x * 0.1f, y * 0.1f);  // Returns -1 to 1
 *   float n = SimplexNoise::noise3D(x, y, time);          // 3D for animation
 */

#include <cstdint>
#include <cmath>

class SimplexNoise {
public:
    /**
     * 2D Simplex noise
     * @param x, y - Input coordinates (scale affects frequency)
     * @return Value in range [-1, 1]
     */
    static float noise2D(float x, float y);

    /**
     * 3D Simplex noise (useful for animated 2D fields)
     * @param x, y, z - Input coordinates (z often used as time)
     * @return Value in range [-1, 1]
     */
    static float noise3D(float x, float y, float z);

    /**
     * Fractal Brownian Motion - layered noise for richer textures
     * @param x, y, z - Input coordinates
     * @param octaves - Number of noise layers (1-4 typical, more = slower)
     * @param persistence - Amplitude multiplier per octave (0.5 typical)
     * @return Value in range approximately [-1, 1]
     */
    static float fbm3D(float x, float y, float z, int octaves = 3, float persistence = 0.5f);

    /**
     * Normalized noise in range [0, 1] instead of [-1, 1]
     */
    static inline float noise3D_01(float x, float y, float z) {
        return (noise3D(x, y, z) + 1.0f) * 0.5f;
    }

private:
    // Permutation table (static, no heap)
    static const uint8_t perm[512];

    // Gradient vectors for 2D
    static const int8_t grad2[8][2];

    // Gradient vectors for 3D
    static const int8_t grad3[12][3];

    // Fast floor function
    static inline int fastFloor(float x) {
        int xi = static_cast<int>(x);
        return x < xi ? xi - 1 : xi;
    }

    // Dot products
    static inline float dot2(const int8_t* g, float x, float y) {
        return g[0] * x + g[1] * y;
    }

    static inline float dot3(const int8_t* g, float x, float y, float z) {
        return g[0] * x + g[1] * y + g[2] * z;
    }
};
