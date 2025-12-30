#pragma once
#include "../types/PixelMatrix.h"
#include "../devices/DeviceConfig.h"
#include "../audio/AudioControl.h"

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
     * Get the name of this generator
     */
    virtual const char* getName() const = 0;

protected:
    // Common generator properties
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint16_t numLeds_ = 0;
    LayoutType layout_ = MATRIX_LAYOUT;

    // Timing
    uint32_t lastUpdateMs_ = 0;


    // Common coordinate conversion helpers (row-major layout)
    // Subclasses can override for custom wiring patterns
    int coordsToIndexRowMajor(int x, int y) const {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) return -1;
        return y * width_ + x;
    }

    void indexToCoordsRowMajor(int index, int& x, int& y) const {
        if (index < 0 || index >= numLeds_) {
            x = y = -1;
            return;
        }
        x = index % width_;
        y = index / width_;
    }
};
