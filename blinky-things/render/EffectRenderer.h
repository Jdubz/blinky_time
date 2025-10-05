#pragma once
// NOTE: Adafruit_NeoPixel.h must be included in main .ino file BEFORE BlinkyArchitecture.h
// to avoid pinDefinitions.h redefinition errors with PDM library
#include <Adafruit_NeoPixel.h>
#include "../types/PixelMatrix.h"
#include "LEDMapper.h"

/**
 * EffectRenderer - Renders PixelMatrix to physical LEDs
 *
 * Handles the mapping from logical pixel coordinates to physical
 * LED indices, taking into account different wiring patterns and
 * orientations.
 */
class EffectRenderer {
private:
    Adafruit_NeoPixel& leds_;
    LEDMapper& ledMapper_;

public:
    EffectRenderer(Adafruit_NeoPixel& leds, LEDMapper& mapper);

    /**
     * Render a PixelMatrix to the physical LEDs
     * @param matrix The pixel matrix to render
     */
    void render(const PixelMatrix& matrix);

    /**
     * Clear all LEDs
     */
    void clear();

    /**
     * Show the rendered frame (calls leds.show())
     */
    void show();

    /**
     * Test pattern for verifying LED mapping
     * @param pattern Test pattern type (0=corners, 1=gradient, 2=checkerboard)
     */
    void renderTestPattern(int pattern = 0);
};
