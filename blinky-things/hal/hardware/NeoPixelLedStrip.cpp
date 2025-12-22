#include "NeoPixelLedStrip.h"

NeoPixelLedStrip::NeoPixelLedStrip(uint16_t numPixels, int16_t pin, uint32_t type)
    : ownsStrip_(true) {
    strip_ = new Adafruit_NeoPixel(numPixels, pin, type);
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
    if (strip_) {
        return strip_->Color(r, g, b);
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
