#pragma once
#include "../PlatformDetect.h"

#ifdef BLINKY_PLATFORM_NRF52840

#include "../interfaces/ILedStrip.h"
#include <new>
#include <string.h>

/**
 * Nrf52PwmLedStrip — Async WS2812B driver using nRF52840 PWM + EasyDMA
 *
 * Replaces Adafruit_NeoPixel for high-LED-count devices (512+ LEDs).
 * Key advantages over Adafruit_NeoPixel on nRF52840:
 *
 *   1. Pre-allocated PWM pattern buffer (no malloc/free per frame)
 *      Adafruit mallocs numLEDs*24*2 bytes (~48 KB for 1024 LEDs) every show()
 *      call, then frees it. This fragments the heap and eventually fails.
 *
 *   2. Non-blocking show() — starts DMA and returns immediately
 *      Adafruit blocks for the entire wire time (~31ms for 1024 LEDs).
 *      This driver overlaps CPU work with DMA transmission.
 *
 *   3. Persistent PWM peripheral — configured once in begin(), not per-frame
 *
 * Memory: GRB buffer (3 * numLEDs) + PWM pattern buffer (numLEDs * 24 * 2 + 4)
 * For 1024 LEDs: 3 KB + 48 KB = ~51 KB total (pre-allocated, no fragmentation).
 *
 * WS2812B timing (800 KHz):
 *   PWM at 16 MHz, counter top = 20 (1.25 µs period)
 *   T0H = 6/20 = 375 ns  (spec: 250-550 ns)
 *   T1H = 13/20 = 812 ns (spec: 650-950 ns)
 */
class Nrf52PwmLedStrip : public ILedStrip {
public:
    Nrf52PwmLedStrip(uint16_t numPixels, int16_t pin);
    ~Nrf52PwmLedStrip() override;

    // Non-copyable
    Nrf52PwmLedStrip(const Nrf52PwmLedStrip&) = delete;
    Nrf52PwmLedStrip& operator=(const Nrf52PwmLedStrip&) = delete;

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

    bool isValid() const { return pixels_ != nullptr && pwmBuffer_ != nullptr; }

    /// True if DMA transmission is currently in progress.
    bool isTransmitting() const;

private:
    // WS2812B timing constants (800 KHz, 16 MHz PWM clock)
    static constexpr uint16_t CTOP = 20;               // Counter top: 1.25 µs
    static constexpr uint16_t T0H  = 6  | 0x8000;      // 375 ns high, bit15 = start high
    static constexpr uint16_t T1H  = 13 | 0x8000;      // 812 ns high

    void convertAndTransmit();
    void waitForDma();
    NRF_PWM_Type* findFreePwm();

    uint16_t numPixels_;
    int16_t pin_;
    uint8_t brightness_;

    uint8_t* pixels_;           // GRB pixel buffer (3 * numPixels_)
    uint16_t* pwmBuffer_;       // PWM duty-cycle pattern (numPixels_ * 24 + 2)
    uint32_t pwmBufferLen_;     // Length of pwmBuffer_ in uint16_t elements

    NRF_PWM_Type* pwm_;         // Claimed PWM peripheral (persistent)
    bool begun_;                // begin() called
    bool transmitting_;         // DMA in progress
};

#endif // BLINKY_PLATFORM_NRF52840
