#pragma once
#include <stdint.h>

/**
 * PixelMatrix - 2D array of RGB pixel data
 *
 * Stores a 2D array of RGB colors that flows through the rendering pipeline:
 * Inputs -> Generator -> Effect (optional) -> Render -> LEDs
 *
 * This is the intermediate data format between pipeline stages.
 */
struct RGB {
    uint8_t r, g, b;

    RGB() : r(0), g(0), b(0) {}
    RGB(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue) {}

    // Convert to 32-bit color value compatible with Adafruit_NeoPixel
    uint32_t to32bit() const {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    // Comparison for testing
    bool operator==(const RGB& other) const {
        return r == other.r && g == other.g && b == other.b;
    }

    bool operator!=(const RGB& other) const {
        return !(*this == other);
    }
};

class PixelMatrix {
private:
    RGB* pixels_;
    int width_;
    int height_;

public:
    PixelMatrix(int width, int height);
    ~PixelMatrix();

    // Copy constructor and assignment operator
    PixelMatrix(const PixelMatrix& other);
    PixelMatrix& operator=(const PixelMatrix& other);

    // Accessors
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    int getTotalPixels() const { return width_ * height_; }

    // Pixel access
    RGB& getPixel(int x, int y);
    const RGB& getPixel(int x, int y) const;
    void setPixel(int x, int y, const RGB& color);
    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

    // Utility functions
    void clear();
    void fill(const RGB& color);
    void fill(uint8_t r, uint8_t g, uint8_t b);

    // Testing helpers
    RGB getPixelSafe(int x, int y) const; // Returns black if out of bounds
    bool isValidCoordinate(int x, int y) const;

    // Debug output
    void printMatrix() const;
};
