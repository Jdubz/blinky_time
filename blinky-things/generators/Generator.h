#pragma once
#include "../types/PixelMatrix.h"
#include "../devices/DeviceConfig.h"
#include "../audio/AudioControl.h"

/**
 * GeneratorType - Type-safe enum for generator identification
 * Used instead of string comparison for type checking
 */
enum class GeneratorType {
    FIRE,
    WATER,
    LIGHTNING,
    CUSTOM
};

/**
 * Generator - Base class for visual pattern generators
 *
 * Generators create visual patterns and output them to a PixelMatrix.
 * They are the source of visual content (fire, water, lightning, etc.).
 *
 * Architecture flow:
 * AudioController -> Generator -> Effect (optional) -> Render -> LEDs
 */
class Generator {
public:
    virtual ~Generator() = default;

    /**
     * Initialize the generator with device configuration
     */
    virtual bool begin(const DeviceConfig& config) = 0;

    /**
     * Generate the next frame of the pattern with audio input
     * @param matrix The output matrix to fill with generated pattern
     * @param audio Unified audio control signal (energy, pulse, phase, rhythmStrength)
     */
    virtual void generate(PixelMatrix& matrix, const AudioControl& audio) = 0;

    /**
     * Reset the generator state
     */
    virtual void reset() = 0;

    /**
     * Get the name of this generator (for display/logging)
     */
    virtual const char* getName() const = 0;

    /**
     * Get the type of this generator (for type-safe checking)
     */
    virtual GeneratorType getType() const = 0;

protected:
    // Common generator properties
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint16_t numLeds_ = 0;
    LayoutType layout_ = MATRIX_LAYOUT;
    MatrixOrientation orientation_ = HORIZONTAL;

    // Timing
    uint32_t lastUpdateMs_ = 0;

    /**
     * Convert 2D coordinates to linear LED index
     * Handles different orientations and wiring patterns:
     * - HORIZONTAL: Standard row-major order
     * - VERTICAL: Zigzag pattern for vertical strips
     *
     * @param x X coordinate (column)
     * @param y Y coordinate (row)
     * @return LED index, or -1 if out of bounds
     */
    int coordsToIndex(int x, int y) const {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) return -1;

        switch (orientation_) {
            case VERTICAL:
                // Zigzag pattern for vertical orientation
                if (x % 2 == 0) {
                    // Even columns: top to bottom
                    return x * height_ + y;
                } else {
                    // Odd columns: bottom to top
                    return x * height_ + (height_ - 1 - y);
                }
            case HORIZONTAL:
            default:
                // Standard row-major order
                return y * width_ + x;
        }
    }

    /**
     * Convert linear LED index to 2D coordinates
     * Inverse of coordsToIndex
     *
     * @param index LED index
     * @param x Output X coordinate
     * @param y Output Y coordinate
     */
    void indexToCoords(int index, int& x, int& y) const {
        if (index < 0 || index >= numLeds_) {
            x = y = -1;
            return;
        }

        switch (orientation_) {
            case VERTICAL:
                // Reverse of zigzag pattern
                x = index / height_;
                if (x % 2 == 0) {
                    // Even columns: top to bottom
                    y = index % height_;
                } else {
                    // Odd columns: bottom to top
                    y = height_ - 1 - (index % height_);
                }
                break;
            case HORIZONTAL:
            default:
                // Standard row-major order
                x = index % width_;
                y = index / width_;
                break;
        }
    }
};
