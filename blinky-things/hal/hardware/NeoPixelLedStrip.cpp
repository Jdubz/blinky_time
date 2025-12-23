#include "NeoPixelLedStrip.h"

NeoPixelLedStrip::NeoPixelLedStrip(uint16_t numPixels, int16_t pin, uint32_t type)
    : strip_(nullptr), ownsStrip_(true) {
    strip_ = new Adafruit_NeoPixel(numPixels, pin, type);
    if (!strip_) {
        // Allocation failed - ownsStrip_ stays true but strip_ is null
        // All methods check strip_ before use, so this is safe
        ownsStrip_ = false;
    }
}

NeoPixelLedStrip::NeoPixelLedStrip(Adafruit_NeoPixel& existingStrip)
    : strip_(&existingStrip), ownsStrip_(false) {
}

NeoPixelLedStrip::~NeoPixelLedStrip() {
    if (ownsStrip_ && strip_) {
        delete strip_;
    }
}

void NeoPixelLedStrip::begin() {
    if (strip_) {
        strip_->begin();
    }
}

void NeoPixelLedStrip::show() {
    if (strip_) {
        strip_->show();
    }
}

void NeoPixelLedStrip::setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (strip_) {
        strip_->setPixelColor(index, strip_->Color(r, g, b));
    }
}

void NeoPixelLedStrip::setPixelColor(uint16_t index, uint32_t color) {
    if (strip_) {
        strip_->setPixelColor(index, color);
    }
}

void NeoPixelLedStrip::clear() {
    if (strip_) {
        strip_->clear();
    }
}

void NeoPixelLedStrip::setBrightness(uint8_t brightness) {
    if (strip_) {
        strip_->setBrightness(brightness);
    }
}

uint8_t NeoPixelLedStrip::getBrightness() const {
    if (strip_) {
        return strip_->getBrightness();
    }
    return 0;
}

uint16_t NeoPixelLedStrip::numPixels() const {
    if (strip_) {
        return strip_->numPixels();
    }
    return 0;
}

uint32_t NeoPixelLedStrip::Color(uint8_t r, uint8_t g, uint8_t b) const {
    if (!strip_) return 0;
    return strip_->Color(r, g, b);
}
