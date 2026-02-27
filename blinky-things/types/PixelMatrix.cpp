#include "PixelMatrix.h"
#include "BlinkyAssert.h"
#include <Arduino.h>
#include <string.h>  // For memcpy

PixelMatrix::PixelMatrix(int width, int height)
    : width_(width), height_(height), pixels_(nullptr) {
    if (width <= 0 || height <= 0) {
        width_ = 0;
        height_ = 0;
        return;
    }
    pixels_ = new(std::nothrow) RGB[width * height];
    if (pixels_) {
        clear();
    } else {
        // Allocation failed - set dimensions to 0 to indicate invalid state
        width_ = 0;
        height_ = 0;
        Serial.println(F("[ERROR] PixelMatrix allocation failed!"));
    }
}

PixelMatrix::~PixelMatrix() {
    delete[] pixels_;
}

PixelMatrix::PixelMatrix(const PixelMatrix& other)
    : width_(other.width_), height_(other.height_), pixels_(nullptr) {
    if (other.isValid()) {
        pixels_ = new(std::nothrow) RGB[width_ * height_];
        if (pixels_) {
            memcpy(pixels_, other.pixels_, sizeof(RGB) * width_ * height_);
        } else {
            // Allocation failed, mark as invalid
            width_ = 0;
            height_ = 0;
        }
    } else {
        // Source was invalid, so this object is also invalid
        width_ = 0;
        height_ = 0;
    }
}

PixelMatrix& PixelMatrix::operator=(const PixelMatrix& other) {
    if (this != &other) {
        delete[] pixels_;
        pixels_ = nullptr;
        width_ = other.width_;
        height_ = other.height_;
        if (other.isValid()) {
            pixels_ = new(std::nothrow) RGB[width_ * height_];
            if (pixels_) {
                memcpy(pixels_, other.pixels_, sizeof(RGB) * width_ * height_);
            } else {
                // Allocation failed, mark as invalid
                width_ = 0;
                height_ = 0;
            }
        } else {
            // Source was invalid, so this object is also invalid
            width_ = 0;
            height_ = 0;
        }
    }
    return *this;
}

RGB& PixelMatrix::getPixel(int x, int y) {
    BLINKY_ASSERT(pixels_ && isValidCoordinate(x, y), "PixelMatrix::getPixel OOB");
    static RGB fallback;
    if (!pixels_ || !isValidCoordinate(x, y)) {
        fallback = RGB(0, 0, 0);
        return fallback;
    }
    return pixels_[y * width_ + x];
}

const RGB& PixelMatrix::getPixel(int x, int y) const {
    BLINKY_ASSERT(pixels_ && isValidCoordinate(x, y), "PixelMatrix::getPixel const OOB");
    static RGB fallback;
    if (!pixels_ || !isValidCoordinate(x, y)) {
        return fallback;
    }
    return pixels_[y * width_ + x];
}

void PixelMatrix::setPixel(int x, int y, const RGB& color) {
    BLINKY_ASSERT(pixels_ && isValidCoordinate(x, y), "PixelMatrix::setPixel OOB");
    if (pixels_ && isValidCoordinate(x, y)) {
        pixels_[y * width_ + x] = color;
    }
}

void PixelMatrix::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    setPixel(x, y, RGB(r, g, b));
}

void PixelMatrix::clear() {
    if (!pixels_) return;
    for (int i = 0; i < width_ * height_; i++) {
        pixels_[i] = RGB(0, 0, 0);
    }
}

void PixelMatrix::fill(const RGB& color) {
    if (!pixels_) return;
    for (int i = 0; i < width_ * height_; i++) {
        pixels_[i] = color;
    }
}

void PixelMatrix::fill(uint8_t r, uint8_t g, uint8_t b) {
    fill(RGB(r, g, b));
}

RGB PixelMatrix::getPixelSafe(int x, int y) const {
    if (isValidCoordinate(x, y)) {
        return getPixel(x, y);
    }
    return RGB(0, 0, 0); // Return black for out of bounds
}

bool PixelMatrix::isValidCoordinate(int x, int y) const {
    return x >= 0 && x < width_ && y >= 0 && y < height_;
}

void PixelMatrix::printMatrix() const {
    Serial.print(F("PixelMatrix "));
    Serial.print(width_);
    Serial.print(F("x"));
    Serial.print(height_);
    Serial.println(F(":"));

    for (int y = 0; y < height_; y++) {
        Serial.print(F("Row "));
        Serial.print(y);
        Serial.print(F(": "));
        for (int x = 0; x < width_; x++) {
            const RGB& pixel = getPixel(x, y);
            Serial.print(F("("));
            Serial.print(pixel.r);
            Serial.print(F(","));
            Serial.print(pixel.g);
            Serial.print(F(","));
            Serial.print(pixel.b);
            Serial.print(F(") "));
        }
        Serial.println();
    }
}
