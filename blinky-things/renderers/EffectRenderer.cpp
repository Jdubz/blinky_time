#include "EffectRenderer.h"

EffectRenderer::EffectRenderer(Adafruit_NeoPixel& leds, LEDMapper& mapper)
    : leds_(leds), ledMapper_(mapper) {
}

void EffectRenderer::render(const EffectMatrix& matrix) {
    int width = matrix.getWidth();
    int height = matrix.getHeight();

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const RGB& color = matrix.getPixel(x, y);
            int ledIndex = ledMapper_.getIndex(x, y);

            if (ledIndex >= 0 && ledIndex < leds_.numPixels()) {
                leds_.setPixelColor(ledIndex, leds_.Color(color.r, color.g, color.b));
            }
        }
    }
}

void EffectRenderer::clear() {
    for (int i = 0; i < leds_.numPixels(); i++) {
        leds_.setPixelColor(i, 0);
    }
}

void EffectRenderer::show() {
    leds_.show();
}

void EffectRenderer::renderTestPattern(int pattern) {
    clear();

    int width = ledMapper_.getWidth();
    int height = ledMapper_.getHeight();

    switch (pattern) {
        case 0: // Corners - Red, Green, Blue, Yellow
            {
                int corners[4][2] = {{0, 0}, {width-1, 0}, {0, height-1}, {width-1, height-1}};
                uint32_t colors[4] = {
                    leds_.Color(255, 0, 0),   // Red
                    leds_.Color(0, 255, 0),   // Green
                    leds_.Color(0, 0, 255),   // Blue
                    leds_.Color(255, 255, 0)  // Yellow
                };

                for (int i = 0; i < 4; i++) {
                    int ledIndex = ledMapper_.getIndex(corners[i][0], corners[i][1]);
                    if (ledIndex >= 0 && ledIndex < leds_.numPixels()) {
                        leds_.setPixelColor(ledIndex, colors[i]);
                    }
                }
            }
            break;

        case 1: // Gradient from bottom (red) to top (blue)
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    float t = (float)y / (height - 1);
                    uint8_t r = (uint8_t)(255 * (1.0f - t));
                    uint8_t g = 0;
                    uint8_t b = (uint8_t)(255 * t);

                    int ledIndex = ledMapper_.getIndex(x, y);
                    if (ledIndex >= 0 && ledIndex < leds_.numPixels()) {
                        leds_.setPixelColor(ledIndex, leds_.Color(r, g, b));
                    }
                }
            }
            break;

        case 2: // Checkerboard
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    bool isWhite = (x + y) % 2 == 0;
                    uint32_t color = isWhite ? leds_.Color(128, 128, 128) : leds_.Color(0, 0, 0);

                    int ledIndex = ledMapper_.getIndex(x, y);
                    if (ledIndex >= 0 && ledIndex < leds_.numPixels()) {
                        leds_.setPixelColor(ledIndex, color);
                    }
                }
            }
            break;
    }
}
