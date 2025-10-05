#pragma once
#include "EffectMatrix.h"

/**
 * Generator - Base interface for visual pattern generators
 *
 * Generators create visual patterns and output them to an EffectMatrix.
 * They are the source of visual content (fire, stars, waves, etc.).
 *
 * Architecture flow:
 * Generator -> Effects -> Renderer -> Hardware
 */
class Generator {
public:
    virtual ~Generator() = default;

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
     * Get the name of this generator for debugging/logging
     */
    virtual const char* getName() const { return "Generator"; }
};
