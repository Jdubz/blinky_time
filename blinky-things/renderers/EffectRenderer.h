#pragma once
#include <Adafruit_NeoPixel.h>
#include "../core/EffectMatrix.h"
#include "../hardware/LEDMapper.h"

/**
 * EffectRenderer - Renders EffectMatrix to physical LEDs
 *
 * Handles the mapping from logical effect coordinates to physical
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
     * Render an EffectMatrix to the physical LEDs
     * @param matrix The effect matrix to render
     */
    void render(const EffectMatrix& matrix);

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
