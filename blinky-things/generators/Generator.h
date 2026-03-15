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
    AUDIO
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

    // Device dimensions for scaling — computed once in begin()
    // No reference device: generators compute all physics from these directly.
    float traversalDim_ = 1.0f;   // Primary movement axis (height for matrix, width for linear)
    float crossDim_ = 1.0f;       // Perpendicular axis (width for matrix, ~sqrt(numLeds) for linear)

    void computeDimensionScales() {
        traversalDim_ = (layout_ == LINEAR_LAYOUT) ? (float)width_ : (float)height_;
        // For linear layouts, crossDim_ can't be 1 — that would make burst sparks,
        // spread, and wind all collapse to trivial values.  Use sqrt(numLeds) as a
        // reasonable "virtual width" so cross-scaled params stay proportional.
        crossDim_ = (layout_ == LINEAR_LAYOUT) ? sqrtf((float)numLeds_) : (float)width_;
    }

};
