#pragma once
#include "../interfaces/ILedStrip.h"
#include "../../inputs/SerialConsole.h"

/**
 * CompositeLedStrip — front for two physical LED strands behind a single
 * ILedStrip interface. The renderer addresses one logical buffer of
 * `total` pixels; this wrapper routes the first half to `strand1` and the
 * second half to `strand2`, then drives `show()` on both.
 *
 * Used for devices like cart-inner that have two physical data pins
 * (e.g. D10 and D9) each driving a separate WS2812B strand. Each
 * underlying strand owns its own PWM peripheral (or NeoPixel object),
 * so writes are independent at the hardware level.
 *
 * Ownership: this class takes ownership of both underlying strips and
 * deletes them in its destructor.
 *
 * **Invariant:** both strands must be non-null. The caller (typically the
 * .ino's LED-init block) is responsible for verifying allocation success
 * BEFORE constructing this wrapper. We loud-halt rather than carry silent
 * null guards through every accessor — silent fallback on a missing
 * strand would mean half the LEDs go dark with no operator signal, which
 * violates the no-silent-fallbacks rule (PR #139 review).
 */
class CompositeLedStrip : public ILedStrip {
public:
    CompositeLedStrip(ILedStrip* strand1, ILedStrip* strand2)
        : s1_(strand1), s2_(strand2),
          n1_(strand1 ? strand1->numPixels() : 0),
          n2_(strand2 ? strand2->numPixels() : 0) {
        if (s1_ == nullptr || s2_ == nullptr) {
            // The .ino path is required to fall back to single-strand on
            // strand-alloc failure rather than wrapping nulls. Reaching
            // here means the caller missed that check — refuse to run
            // silently with broken pixel routing.
            Serial.println(F("[FATAL] CompositeLedStrip constructed with null strand — halting"));
            Serial.flush();
            while (true) { /* halt */ }
        }
    }

    ~CompositeLedStrip() override {
        delete s1_;
        delete s2_;
    }

    CompositeLedStrip(const CompositeLedStrip&) = delete;
    CompositeLedStrip& operator=(const CompositeLedStrip&) = delete;

    void begin() override {
        s1_->begin();
        s2_->begin();
    }

    void show() override {
        // Start both transmissions. With Nrf52PwmLedStrip's async DMA path
        // each strand kicks off independently and runs concurrently on its
        // own PWM peripheral.
        s1_->show();
        s2_->show();
    }

    void setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) override {
        if (index < n1_) {
            s1_->setPixelColor(index, r, g, b);
        } else if (index < n1_ + n2_) {
            s2_->setPixelColor(index - n1_, r, g, b);
        }
        // Out-of-range writes are silently dropped to match the behaviour
        // of the underlying single-strand drivers.
    }

    void setPixelColor(uint16_t index, uint32_t color) override {
        if (index < n1_) {
            s1_->setPixelColor(index, color);
        } else if (index < n1_ + n2_) {
            s2_->setPixelColor(index - n1_, color);
        }
    }

    void clear() override {
        s1_->clear();
        s2_->clear();
    }

    void setBrightness(uint8_t brightness) override {
        s1_->setBrightness(brightness);
        s2_->setBrightness(brightness);
    }

    uint8_t getBrightness() const override {
        // Both strands carry the same brightness (setBrightness propagates
        // to both). Forward to s1_ as the canonical source.
        return s1_->getBrightness();
    }

    uint16_t numPixels() const override {
        return n1_ + n2_;
    }

    uint32_t Color(uint8_t r, uint8_t g, uint8_t b) const override {
        // Both strands use the same colour-packing convention.
        return s1_->Color(r, g, b);
    }

private:
    ILedStrip* s1_;
    ILedStrip* s2_;
    uint16_t n1_;
    uint16_t n2_;
};
