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
     * Initialize the generator with matrix dimensions
     */
    virtual void begin(int width, int height) = 0;
    
    /**
     * Generate the next frame of the pattern
     * @param matrix The output matrix to fill with generated pattern
     */
    virtual void generate(EffectMatrix* matrix) = 0;
    
    /**
     * Update internal state (called each frame before generate)
     */
    virtual void update() = 0;
    
    /**
     * Get the name of this generator for debugging/logging
     */
    virtual const char* getName() const = 0;
};