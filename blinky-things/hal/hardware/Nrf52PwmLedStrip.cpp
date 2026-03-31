#include "Nrf52PwmLedStrip.h"

#ifdef BLINKY_PLATFORM_NRF52840

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

Nrf52PwmLedStrip::Nrf52PwmLedStrip(uint16_t numPixels, int16_t pin)
    : numPixels_(numPixels), pin_(pin), brightness_(0),
      pixels_(nullptr), pwmBuffer_(nullptr), pwmBufferLen_(0),
      pwm_(nullptr), begun_(false), transmitting_(false) {

    uint32_t numBytes = (uint32_t)numPixels * 3;
    pixels_ = new(std::nothrow) uint8_t[numBytes];
    if (pixels_) {
        memset(pixels_, 0, numBytes);
    }

    // Each byte = 8 PWM values (uint16_t), plus 2 end markers
    pwmBufferLen_ = numBytes * 8 + 2;
    pwmBuffer_ = new(std::nothrow) uint16_t[pwmBufferLen_];
}

Nrf52PwmLedStrip::~Nrf52PwmLedStrip() {
    if (pwm_) {
        waitForDma();
        pwm_->ENABLE = 0;
        pwm_->PSEL.OUT[0] = 0xFFFFFFFFUL;
    }
    delete[] pwmBuffer_;
    delete[] pixels_;
}

// ---------------------------------------------------------------------------
// ILedStrip interface
// ---------------------------------------------------------------------------

void Nrf52PwmLedStrip::begin() {
    if (begun_) return;
    if (!pixels_ || !pwmBuffer_) {
        Serial.println(F("[ERROR] Nrf52PwmLedStrip: buffer alloc failed"));
        return;
    }

    // PWM SEQ[n].CNT is 15 bits (max 32767). Each LED = 24 PWM values + 2 end markers.
    // Max LEDs per sequence = (32767 - 2) / 24 = 1365.
    // Check before claiming PWM peripheral to avoid leaking the slot on error.
    if (pwmBufferLen_ > 32767) {
        Serial.print(F("[ERROR] Too many LEDs for PWM DMA ("));
        Serial.print(numPixels_);
        Serial.println(F(", max 1365)"));
        return;
    }

    pwm_ = findFreePwm();
    if (!pwm_) {
        Serial.println(F("[ERROR] No free PWM peripheral for LED strip"));
        return;
    }

    // Configure PWM — persistent, not re-done per frame
    pwm_->MODE = (PWM_MODE_UPDOWN_Up << PWM_MODE_UPDOWN_Pos);
    pwm_->PRESCALER = (PWM_PRESCALER_PRESCALER_DIV_1 << PWM_PRESCALER_PRESCALER_Pos);
    pwm_->COUNTERTOP = (CTOP << PWM_COUNTERTOP_COUNTERTOP_Pos);
    pwm_->LOOP = (PWM_LOOP_CNT_Disabled << PWM_LOOP_CNT_Pos);
    pwm_->DECODER = (PWM_DECODER_LOAD_Common << PWM_DECODER_LOAD_Pos) |
                    (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);
    pwm_->SEQ[0].REFRESH = 0;
    pwm_->SEQ[0].ENDDELAY = 0;

    // Map Arduino pin to nRF GPIO
#if defined(ARDUINO_ARCH_NRF52840)
    uint32_t gpio = g_APinDescription[pin_].name;
#else
    uint32_t gpio = g_ADigitalPinMap[pin_];
#endif
    pwm_->PSEL.OUT[0] = gpio;
    pwm_->ENABLE = 1;
    begun_ = true;

    Serial.print(F("[INFO] Nrf52PwmLedStrip: ready, pin=D"));
    Serial.print(pin_);
    Serial.print(F(" (GPIO 0x"));
    Serial.print(gpio, HEX);
    Serial.print(F("), "));
    Serial.print(numPixels_);
    Serial.print(F(" LEDs, PWM buf="));
    Serial.print(pwmBufferLen_ * 2);
    Serial.println(F(" bytes"));
}

void Nrf52PwmLedStrip::show() {
    if (!begun_) return;
    convertAndTransmit();
}

void Nrf52PwmLedStrip::setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (!pixels_ || index >= numPixels_) return;

    if (brightness_) {
        r = ((uint16_t)r * (uint16_t)brightness_) >> 8;
        g = ((uint16_t)g * (uint16_t)brightness_) >> 8;
        b = ((uint16_t)b * (uint16_t)brightness_) >> 8;
    }

    uint8_t* p = &pixels_[index * 3];
    p[0] = g;  // GRB order
    p[1] = r;
    p[2] = b;
}

void Nrf52PwmLedStrip::setPixelColor(uint16_t index, uint32_t color) {
    setPixelColor(index,
                  (uint8_t)(color >> 16),
                  (uint8_t)(color >> 8),
                  (uint8_t)color);
}

void Nrf52PwmLedStrip::clear() {
    if (pixels_) {
        memset(pixels_, 0, (uint32_t)numPixels_ * 3);
    }
}

void Nrf52PwmLedStrip::setBrightness(uint8_t brightness) {
    // Adafruit convention: store b+1 so internal 0 = max (no scaling).
    // setBrightness(0) → stores 1 → near-off; setBrightness(255) → stores 0 → max.
    brightness_ = (uint8_t)(brightness + 1);
}

uint8_t Nrf52PwmLedStrip::getBrightness() const {
    // brightness_==0 means setBrightness() was never called → max brightness (255)
    return brightness_ ? (brightness_ - 1) : 255;
}

uint16_t Nrf52PwmLedStrip::numPixels() const {
    return numPixels_;
}

uint32_t Nrf52PwmLedStrip::Color(uint8_t r, uint8_t g, uint8_t b) const {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

bool Nrf52PwmLedStrip::isTransmitting() const {
    return transmitting_ && pwm_ && !pwm_->EVENTS_SEQEND[0];
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void Nrf52PwmLedStrip::convertAndTransmit() {
    // Wait for any previous DMA to complete before overwriting the PWM buffer
    waitForDma();

    // Convert GRB pixel bytes → PWM duty-cycle pattern
    uint32_t numBytes = (uint32_t)numPixels_ * 3;
    uint16_t pos = 0;
    for (uint32_t n = 0; n < numBytes; n++) {
        uint8_t pix = pixels_[n];
        for (uint8_t mask = 0x80; mask; mask >>= 1) {
            pwmBuffer_[pos++] = (pix & mask) ? T1H : T0H;
        }
    }
    // End-of-sequence markers
    pwmBuffer_[pos++] = 0x8000;
    pwmBuffer_[pos++] = 0x8000;

    // Point DMA at the buffer and start
    pwm_->SEQ[0].PTR = (uint32_t)pwmBuffer_;
    pwm_->SEQ[0].CNT = pos;
    pwm_->EVENTS_SEQEND[0] = 0;
    pwm_->TASKS_SEQSTART[0] = 1;
    transmitting_ = true;
}

void Nrf52PwmLedStrip::waitForDma() {
    if (!transmitting_ || !pwm_) return;
    while (!pwm_->EVENTS_SEQEND[0]) {
        vTaskDelay(1);  // yield() is a no-op on Adafruit nRF52 core
    }
    pwm_->EVENTS_SEQEND[0] = 0;
    transmitting_ = false;
}

NRF_PWM_Type* Nrf52PwmLedStrip::findFreePwm() {
    NRF_PWM_Type* devices[] = {
        NRF_PWM0, NRF_PWM1, NRF_PWM2
#if defined(NRF_PWM3)
        , NRF_PWM3
#endif
    };

    for (unsigned int i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        NRF_PWM_Type* p = devices[i];
        if ((p->ENABLE == 0) &&
            (p->PSEL.OUT[0] & PWM_PSEL_OUT_CONNECT_Msk) &&
            (p->PSEL.OUT[1] & PWM_PSEL_OUT_CONNECT_Msk) &&
            (p->PSEL.OUT[2] & PWM_PSEL_OUT_CONNECT_Msk) &&
            (p->PSEL.OUT[3] & PWM_PSEL_OUT_CONNECT_Msk)) {
            return p;
        }
    }
    return nullptr;
}

#endif // BLINKY_PLATFORM_NRF52840
