#pragma once
#include "EffectMatrix.h"

/**
 * VisualEffect - Base interface for all visual effects
 * 
 * Effects generate color patterns in an EffectMatrix, which can then
 * be rendered to physical LEDs by an EffectRenderer. This separation
 * allows for easy testing, effect swapping, and hardware abstraction.
 */
class VisualEffect {
public:
    virtual ~VisualEffect() {}
    
    /**
     * Initialize the effect with matrix dimensions
     * @param width Matrix width in pixels
     * @param height Matrix height in pixels
     */
    virtual void begin(int width, int height) = 0;
    
    /**
     * Update the effect state based on audio input
     * @param energy Audio energy level (0.0 to 1.0)
     * @param hit Audio transient/hit level (0.0 to 1.0+)
     */
    virtual void update(float energy, float hit) = 0;
    
    /**
     * Generate the current effect frame into the provided matrix
     * @param matrix Output matrix to fill with colors
     */
    virtual void render(EffectMatrix& matrix) = 0;
    
    /**
     * Reset effect to default parameters
     */
    virtual void restoreDefaults() = 0;
    
    /**
     * Get effect name for debugging/UI
     */
    virtual const char* getName() const = 0;
};