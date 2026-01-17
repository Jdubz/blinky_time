#pragma once

#include "../types/PixelMatrix.h"
#include "../audio/AudioControl.h"
#include <stdint.h>

/**
 * BackgroundModel - Abstract noise background rendering
 *
 * Defines how noise-based backgrounds are rendered for different
 * generator types (fire embers, water surface, storm clouds).
 *
 * Different layouts require different approaches:
 * - MATRIX: Y-based height falloff (hotter at bottom for fire)
 * - LINEAR: Position-based variation only (uniform glow)
 */
class BackgroundModel {
public:
    virtual ~BackgroundModel() = default;

    /**
     * Render noise background to the matrix
     * @param matrix Output pixel matrix
     * @param width Grid width
     * @param height Grid height
     * @param noiseTime Animation time for noise sampling
     * @param audio Current audio state for beat-reactive effects
     */
    virtual void render(PixelMatrix& matrix, uint16_t width, uint16_t height,
                       float noiseTime, const AudioControl& audio) = 0;

    /**
     * Get intensity modifier for a position
     * Used by generators that need position-based intensity scaling
     * @param x X coordinate
     * @param y Y coordinate
     * @param width Grid width
     * @param height Grid height
     * @return Intensity modifier (0.0 to 1.0)
     */
    virtual float getIntensityAt(int x, int y, uint16_t width, uint16_t height) const = 0;
};
