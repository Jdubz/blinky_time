#pragma once
#include "../interfaces/ILedStrip.h"
#include <Adafruit_NeoPixel.h>

/**
 * NeoPixelLedStrip - ILedStrip implementation using Adafruit_NeoPixel
 *
 * Wraps the Adafruit_NeoPixel library to implement the ILedStrip interface.
 * Can either own a NeoPixel instance or wrap an existing one.
 */
class NeoPixelLedStrip : public ILedStrip {
public:
    /**
     * Constructor that creates and owns a NeoPixel instance
     */
    NeoPixelLedStrip(uint16_t numPixels, int16_t pin, uint32_t type);

    /**
     * Constructor that wraps an existing NeoPixel instance (does not take ownership)
     */
    explicit NeoPixelLedStrip(Adafruit_NeoPixel& existingStrip);

    ~NeoPixelLedStrip() override;

    // Non-copyable (Rule of Three: has destructor with resource management)
    NeoPixelLedStrip(const NeoPixelLedStrip&) = delete;
    NeoPixelLedStrip& operator=(const NeoPixelLedStrip&) = delete;

    // ILedStrip interface
    void begin() override;
    void show() override;
    void setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void setPixelColor(uint16_t index, uint32_t color) override;
    void clear() override;
    void setBrightness(uint8_t brightness) override;
    uint8_t getBrightness() const override;
    uint16_t numPixels() const override;
    uint32_t Color(uint8_t r, uint8_t g, uint8_t b) const override;

    /**
     * Check if the strip was successfully initialized
     * Returns false if allocation failed in the owning constructor
     */
    bool isValid() const { return strip_ != nullptr; }

private:
    Adafruit_NeoPixel* strip_;
    bool ownsStrip_;
};
