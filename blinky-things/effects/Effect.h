#pragma once
#include "../types/PixelMatrix.h"

/**
 * Effect - Base interface for visual effects that modify generated patterns
 *
 * Effects take a generated pattern and modify it (hue shift, brightness,
 * blur, color mapping, etc.). They are OPTIONAL in the pipeline.
 *
 * Architecture flow:
 * Inputs -> Generator -> Effect (optional) -> Render -> LEDs
 */
class Effect {
public:
    virtual ~Effect() = default;

    /**
     * Initialize the effect with matrix dimensions
     */
    virtual void begin(int width, int height) = 0;

    /**
     * Apply the effect to the matrix
     * @param matrix The matrix to modify (input and output)
     */
    virtual void apply(PixelMatrix* matrix) = 0;

    /**
     * Reset the effect state to initial conditions
     */
    virtual void reset() = 0;

    /**
     * Get the name of this effect for debugging/logging
     */
    virtual const char* getName() const = 0;
};
