#pragma once
#include "../interfaces/ILedStrip.h"

/**
 * MockLedStrip - Test mock for LED strip
 *
 * Records all operations for verification in unit tests.
 * Uses a simple array instead of std::vector for Arduino compatibility.
 */
class MockLedStrip : public ILedStrip {
public:
    static constexpr uint16_t MAX_PIXELS = 256;

    explicit MockLedStrip(uint16_t numPixels)
        : numPixels_(numPixels < MAX_PIXELS ? numPixels : MAX_PIXELS),
          brightness_(255), showCount_(0), begun_(false) {
        clear();
    }

    void begin() override {
        begun_ = true;
    }

    void show() override {
        showCount_++;
    }

    void setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) override {
        if (index < numPixels_) {
            pixels_[index] = Color(r, g, b);
        }
    }

    void setPixelColor(uint16_t index, uint32_t color) override {
        if (index < numPixels_) {
            pixels_[index] = color;
        }
    }

    void clear() override {
        for (uint16_t i = 0; i < numPixels_; i++) {
            pixels_[i] = 0;
        }
    }

    void setBrightness(uint8_t brightness) override {
        brightness_ = brightness;
    }

    uint8_t getBrightness() const override {
        return brightness_;
    }

    uint16_t numPixels() const override {
        return numPixels_;
    }

    uint32_t Color(uint8_t r, uint8_t g, uint8_t b) const override {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    // Test inspection methods
    uint32_t getPixelColor(uint16_t index) const {
        return index < numPixels_ ? pixels_[index] : 0;
    }

    uint8_t getRed(uint16_t index) const {
        return (getPixelColor(index) >> 16) & 0xFF;
    }

    uint8_t getGreen(uint16_t index) const {
        return (getPixelColor(index) >> 8) & 0xFF;
    }

    uint8_t getBlue(uint16_t index) const {
        return getPixelColor(index) & 0xFF;
    }

    int getShowCount() const { return showCount_; }
    bool hasBegun() const { return begun_; }

    void reset() {
        clear();
        showCount_ = 0;
        begun_ = false;
        brightness_ = 255;
    }

private:
    uint32_t pixels_[MAX_PIXELS];
    uint16_t numPixels_;
    uint8_t brightness_;
    int showCount_;
    bool begun_;
};
