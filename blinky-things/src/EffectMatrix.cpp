#include "../core/EffectMatrix.h"
#include <Arduino.h>

EffectMatrix::EffectMatrix(int width, int height)
    : width_(width), height_(height) {
    pixels_ = new RGB[width * height];
    clear();
}

EffectMatrix::~EffectMatrix() {
    delete[] pixels_;
}

EffectMatrix::EffectMatrix(const EffectMatrix& other)
    : width_(other.width_), height_(other.height_) {
    pixels_ = new RGB[width_ * height_];
    for (int i = 0; i < width_ * height_; i++) {
        pixels_[i] = other.pixels_[i];
    }
}

EffectMatrix& EffectMatrix::operator=(const EffectMatrix& other) {
    if (this != &other) {
        delete[] pixels_;
        width_ = other.width_;
        height_ = other.height_;
        pixels_ = new RGB[width_ * height_];
        for (int i = 0; i < width_ * height_; i++) {
            pixels_[i] = other.pixels_[i];
        }
    }
    return *this;
}

RGB& EffectMatrix::getPixel(int x, int y) {
    return pixels_[y * width_ + x];
}

const RGB& EffectMatrix::getPixel(int x, int y) const {
    return pixels_[y * width_ + x];
}

void EffectMatrix::setPixel(int x, int y, const RGB& color) {
    if (isValidCoordinate(x, y)) {
        pixels_[y * width_ + x] = color;
    }
}

void EffectMatrix::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    setPixel(x, y, RGB(r, g, b));
}

void EffectMatrix::clear() {
    for (int i = 0; i < width_ * height_; i++) {
        pixels_[i] = RGB(0, 0, 0);
    }
}

void EffectMatrix::fill(const RGB& color) {
    for (int i = 0; i < width_ * height_; i++) {
        pixels_[i] = color;
    }
}

void EffectMatrix::fill(uint8_t r, uint8_t g, uint8_t b) {
    fill(RGB(r, g, b));
}

RGB EffectMatrix::getPixelSafe(int x, int y) const {
    if (isValidCoordinate(x, y)) {
        return getPixel(x, y);
    }
    return RGB(0, 0, 0); // Return black for out of bounds
}

bool EffectMatrix::isValidCoordinate(int x, int y) const {
    return x >= 0 && x < width_ && y >= 0 && y < height_;
}

void EffectMatrix::printMatrix() const {
    Serial.print(F("EffectMatrix "));
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
