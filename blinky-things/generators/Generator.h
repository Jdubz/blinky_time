#pragma once
#include "../core/EffectMatrix.h"
#include "../devices/DeviceConfig.h"

/**
 * Generator - Base class for visual pattern generators
 *
 * Generators create visual patterns and output them to an EffectMatrix.
 * They are the source of visual content (fire, water, lightning, etc.).
 *
 * Architecture flow:
 * Generator -> Effects -> Renderer -> Hardware
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
     * @param energy Audio energy level (0.0 to 1.0)
     * @param hit Audio hit/transient level (0.0 to 1.0)
     */
    virtual void generate(EffectMatrix& matrix, float energy = 0.0f, float hit = 0.0f) = 0;

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
    
    // Audio responsiveness
    bool audioReactive_ = true;
    float audioSensitivity_ = 1.0f;
};