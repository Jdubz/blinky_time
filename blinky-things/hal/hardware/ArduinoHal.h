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
public:
    void setResolution(uint8_t bits) override {
        #if defined(analogReadResolution)
        analogReadResolution(bits);
        #else
        (void)bits; // Suppress unused warning when analogReadResolution not available
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
        return ::analogRead(pin);
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
