#pragma once
#include "EffectMatrix.h"

/**
 * Effect - Base interface for visual effects that modify generated patterns
 *
 * Effects take a generated pattern and modify it (hue shift, brightness,
 * blur, color mapping, etc.). They operate on the matrix between generation
 * and rendering.
 *
 * Architecture flow:
 * Generator -> Effects -> Renderer -> Hardware
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
    virtual void apply(EffectMatrix* matrix) = 0;

    /**
     * Get the name of this effect for debugging/logging
     */
    virtual const char* getName() const = 0;
};
