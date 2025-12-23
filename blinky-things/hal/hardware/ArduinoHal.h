#pragma once
#include "../interfaces/IGpio.h"
#include "../interfaces/IAdc.h"
#include "../interfaces/ISystemTime.h"
#include <Arduino.h>

/**
 * ArduinoGpio - IGpio implementation for Arduino platforms
 */
class ArduinoGpio : public IGpio {
public:
    void pinMode(int pin, uint8_t mode) override {
        switch (mode) {
            case INPUT_MODE:
                ::pinMode(pin, INPUT);
                break;
            case OUTPUT_MODE:
                ::pinMode(pin, OUTPUT);
                break;
            case INPUT_PULLUP_MODE:
                ::pinMode(pin, INPUT_PULLUP);
                break;
        }
    }

    void digitalWrite(int pin, uint8_t value) override {
        ::digitalWrite(pin, value == HIGH_LEVEL ? HIGH : LOW);
    }

    int digitalRead(int pin) const override {
        return ::digitalRead(pin) == HIGH ? HIGH_LEVEL : LOW_LEVEL;
    }
};

/**
 * ArduinoAdc - IAdc implementation for Arduino platforms
 */
class ArduinoAdc : public IAdc {
private:
    uint8_t currentBits_ = 10; // Default to 10-bit for Arduino

public:
    void setResolution(uint8_t bits) override {
        currentBits_ = bits;
        #if defined(analogReadResolution)
        analogReadResolution(bits);
        #else
        // If analogReadResolution not available, we'll handle it in analogRead()
        #warning "analogReadResolution() not available on this platform - ADC stuck at 10-bit"
        #endif
    }

    void setReference(uint8_t reference) override {
        #if defined(AR_INTERNAL2V4)
        if (reference == REF_INTERNAL_2V4) {
            analogReference(AR_INTERNAL2V4);
        }
        #else
        (void)reference; // Suppress unused warning when AR_INTERNAL2V4 not available
        #endif
    }

    uint16_t analogRead(int pin) override {
        uint16_t raw = ::analogRead(pin);

        // Workaround for platforms where analogReadResolution() doesn't work
        // If we want 12-bit but getting 10-bit values, scale up
        #if !defined(analogReadResolution)
        if (currentBits_ == 12 && raw <= 1023) {
            raw = raw << 2; // Scale 10-bit (0-1023) to 12-bit (0-4092)
        }
        #endif

        return raw;
    }
};

/**
 * ArduinoSystemTime - ISystemTime implementation for Arduino platforms
 */
class ArduinoSystemTime : public ISystemTime {
public:
    uint32_t millis() const override {
        return ::millis();
    }

    uint32_t micros() const override {
        return ::micros();
    }

    void delay(uint32_t ms) override {
        ::delay(ms);
    }

    void delayMicroseconds(uint32_t us) override {
        ::delayMicroseconds(us);
    }

    void noInterrupts() override {
        ::noInterrupts();
    }

    void interrupts() override {
        ::interrupts();
    }
};
