#pragma once
#include <stdint.h>

/**
 * ColorPalette - Unified color palette system for generators
 *
 * Provides a flexible three-segment color palette that can be used by
 * Fire, Lightning, Water, and future generators. Each palette defines
 * how an intensity value (0-255) maps to an RGB color.
 *
 * The palette is divided into 3 segments at fixed breakpoints (85, 170)
 * to match the existing behavior of all generators.
 */

namespace Palette {

// Segment breakpoints (shared by all palettes)
// Intensity values [0, SEGMENT_1_THRESHOLD) are in segment 1
// Intensity values [SEGMENT_1_THRESHOLD, SEGMENT_2_THRESHOLD) are in segment 2
// Intensity values [SEGMENT_2_THRESHOLD, 255] are in segment 3
constexpr uint8_t SEGMENT_1_THRESHOLD = 85;   // Start of segment 2
constexpr uint8_t SEGMENT_2_THRESHOLD = 170;  // Start of segment 3

// RGB color struct for palette definition
struct RGB {
    uint8_t r, g, b;

    constexpr RGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}

    // Pack to uint32_t (0x00RRGGBB format)
    uint32_t pack() const {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
};

// Linear interpolation between two values
// Clamps value to [minVal, maxVal] to prevent underflow
inline uint8_t lerp(uint8_t a, uint8_t b, uint8_t value, uint8_t minVal, uint8_t maxVal) {
    if (maxVal <= minVal) return a;
    // Clamp value to prevent underflow when value < minVal
    if (value < minVal) value = minVal;
    if (value > maxVal) value = maxVal;
    uint16_t range = maxVal - minVal;
    uint16_t pos = value - minVal;
    return a + (((int16_t)b - (int16_t)a) * pos) / range;
}

// Interpolate between two RGB colors
inline RGB lerpRGB(const RGB& a, const RGB& b, uint8_t value, uint8_t minVal, uint8_t maxVal) {
    return RGB(
        lerp(a.r, b.r, value, minVal, maxVal),
        lerp(a.g, b.g, value, minVal, maxVal),
        lerp(a.b, b.b, value, minVal, maxVal)
    );
}

/**
 * Three-segment color palette definition
 * Defines colors at 4 key points: 0, 85, 170, 255
 */
struct ThreeSegmentPalette {
    RGB color0;    // Color at value 0
    RGB color85;   // Color at value 85 (end of segment 1)
    RGB color170;  // Color at value 170 (end of segment 2)
    RGB color255;  // Color at value 255 (end of segment 3)

    /**
     * Convert intensity value (0-255) to RGB color
     */
    uint32_t toColor(uint8_t value) const {
        if (value == 0) {
            return color0.pack();
        } else if (value < SEGMENT_1_THRESHOLD) {
            return lerpRGB(color0, color85, value, 0, SEGMENT_1_THRESHOLD - 1).pack();
        } else if (value < SEGMENT_2_THRESHOLD) {
            return lerpRGB(color85, color170, value, SEGMENT_1_THRESHOLD, SEGMENT_2_THRESHOLD - 1).pack();
        } else {
            return lerpRGB(color170, color255, value, SEGMENT_2_THRESHOLD, 255).pack();
        }
    }
};

// ============================================================================
// Pre-defined palettes for each generator type
// ============================================================================

// Fire: black -> red -> orange -> yellow
// Note: Fire uses a slightly different algorithm (simple multiply instead of
// lerp in segment 1) to preserve the original visual appearance.

// Lightning: black -> bright yellow -> white -> electric blue
constexpr ThreeSegmentPalette LIGHTNING = {
    RGB(0, 0, 0),        // value 0: off (black)
    RGB(255, 200, 0),    // value 85: bright yellow
    RGB(255, 255, 180),  // value 170: white-ish
    RGB(150, 200, 255)   // value 255: electric blue
};

// Water: black -> medium blue -> cyan -> light blue
constexpr ThreeSegmentPalette WATER = {
    RGB(0, 0, 0),        // value 0: off (black)
    RGB(0, 0, 150),      // value 85: medium blue
    RGB(0, 120, 255),    // value 170: cyan
    RGB(80, 200, 255)    // value 255: light blue
};

} // namespace Palette
