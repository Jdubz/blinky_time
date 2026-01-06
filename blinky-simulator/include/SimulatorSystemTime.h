#pragma once
/**
 * SimulatorSystemTime - ISystemTime implementation for desktop simulation
 *
 * Provides controllable time for deterministic frame rendering.
 */

#include "ArduinoCompat.h"
#include <cstdint>

// Forward declare ISystemTime interface inline to avoid path issues
// This matches the interface from blinky-things/hal/interfaces/ISystemTime.h
#ifndef ISYSTEMTIME_DEFINED
#define ISYSTEMTIME_DEFINED
class ISystemTime {
public:
    virtual ~ISystemTime() = default;
    virtual uint32_t millis() const = 0;
    virtual uint32_t micros() const = 0;
    virtual void delay(uint32_t ms) = 0;
    virtual void delayMicroseconds(uint32_t us) = 0;
    virtual void noInterrupts() = 0;
    virtual void interrupts() = 0;
};
#endif

class SimulatorSystemTime : public ISystemTime {
private:
    uint32_t currentTimeMs_ = 0;
    bool useSimulated_ = true;

public:
    SimulatorSystemTime() = default;

    // Set absolute time
    void setTime(uint32_t ms) {
        currentTimeMs_ = ms;
        // Also update global simulated time
        SimulatorTime::setSimulatedTime(ms);
    }

    // Advance time by delta
    void advance(uint32_t deltaMs) {
        currentTimeMs_ += deltaMs;
        SimulatorTime::setSimulatedTime(currentTimeMs_);
    }

    // Get current time
    uint32_t getTime() const {
        return currentTimeMs_;
    }

    // Use real wall-clock time instead
    void useRealTime() {
        useSimulated_ = false;
        SimulatorTime::useRealTime();
    }

    // Use simulated time
    void useSimulatedTime() {
        useSimulated_ = true;
        SimulatorTime::setSimulatedTime(currentTimeMs_);
    }

    // ISystemTime interface
    uint32_t millis() const override {
        if (useSimulated_) {
            return currentTimeMs_;
        }
        return SimulatorTime::getRealMillis();
    }

    uint32_t micros() const override {
        return millis() * 1000;
    }

    void delay(uint32_t ms) override {
        if (useSimulated_) {
            currentTimeMs_ += ms;
            SimulatorTime::setSimulatedTime(currentTimeMs_);
        }
    }

    void delayMicroseconds(uint32_t us) override {
        delay(us / 1000);
    }

    void noInterrupts() override {
        // No-op in simulation
    }

    void interrupts() override {
        // No-op in simulation
    }
};
