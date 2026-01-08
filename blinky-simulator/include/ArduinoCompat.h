#pragma once
/**
 * ArduinoCompat.h - Arduino compatibility layer for desktop simulation
 *
 * Provides stubs for Arduino functions used by the blinky-things pipeline.
 * This allows the firmware rendering code to compile and run on desktop.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <new>

// ============================================================================
// Math Constants
// ============================================================================

#ifndef PI
#define PI 3.14159265358979323846f
#endif

#ifndef TWO_PI
#define TWO_PI 6.28318530718f
#endif

#ifndef HALF_PI
#define HALF_PI 1.5707963267948966f
#endif

#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295f
#endif

#ifndef RAD_TO_DEG
#define RAD_TO_DEG 57.29577951308232f
#endif

// ============================================================================
// Timing
// ============================================================================

namespace SimulatorTime {
    // Global simulation time (can be controlled externally)
    inline uint32_t simulatedTimeMs = 0;
    inline bool useSimulatedTime = false;

    inline uint32_t getRealMillis() {
        static auto startTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
        );
    }

    inline void setSimulatedTime(uint32_t ms) {
        simulatedTimeMs = ms;
        useSimulatedTime = true;
    }

    inline void advanceTime(uint32_t deltaMs) {
        simulatedTimeMs += deltaMs;
    }

    inline void useRealTime() {
        useSimulatedTime = false;
    }
}

inline uint32_t millis() {
    if (SimulatorTime::useSimulatedTime) {
        return SimulatorTime::simulatedTimeMs;
    }
    return SimulatorTime::getRealMillis();
}

inline uint32_t micros() {
    return millis() * 1000;
}

inline void delay(uint32_t ms) {
    // In simulation, we just advance simulated time
    if (SimulatorTime::useSimulatedTime) {
        SimulatorTime::advanceTime(ms);
    }
}

inline void delayMicroseconds(uint32_t us) {
    delay(us / 1000);
}

// ============================================================================
// Random Numbers
// ============================================================================

namespace SimulatorRandom {
    inline uint32_t seed = 12345;

    inline void setSeed(uint32_t s) {
        seed = s;
        std::srand(s);
    }
}

inline void randomSeed(uint32_t seed) {
    SimulatorRandom::setSeed(seed);
}

inline long random(long max) {
    if (max <= 0) return 0;
    return std::rand() % max;
}

inline long random(long min, long max) {
    if (max <= min) return min;
    return min + (std::rand() % (max - min));
}

// ============================================================================
// Math Helpers
// ============================================================================

template<typename T>
inline T constrain(T val, T minVal, T maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

// Prevent Windows.h from defining min/max macros
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Arduino-style min/max that work with mixed types
// Using function templates to avoid macro conflicts with std::min/std::max
using std::min;
using std::max;

// Provide mixed-type overloads that Arduino code might use
inline int min(int a, uint16_t b) { return a < static_cast<int>(b) ? a : static_cast<int>(b); }
inline int min(uint16_t a, int b) { return static_cast<int>(a) < b ? static_cast<int>(a) : b; }
inline uint32_t min(uint32_t a, int b) { return a < static_cast<uint32_t>(b) ? a : static_cast<uint32_t>(b); }
inline uint32_t min(int a, uint32_t b) { return static_cast<uint32_t>(a) < b ? static_cast<uint32_t>(a) : b; }
inline int max(int a, uint16_t b) { return a > static_cast<int>(b) ? a : static_cast<int>(b); }
inline int max(uint16_t a, int b) { return static_cast<int>(a) > b ? static_cast<int>(a) : b; }
inline uint32_t max(uint32_t a, int b) { return a > static_cast<uint32_t>(b) ? a : static_cast<uint32_t>(b); }
inline uint32_t max(int a, uint32_t b) { return static_cast<uint32_t>(a) > b ? static_cast<uint32_t>(a) : b; }

inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

inline float mapf(float x, float in_min, float in_max, float out_min, float out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ============================================================================
// String Helpers
// ============================================================================

// F() macro for flash strings - no-op on desktop
#define F(str) str

// ============================================================================
// Serial Stub
// ============================================================================

class SerialClass {
public:
    void begin(long baud) { (void)baud; }
    void end() {}

    template<typename T>
    void print(T val) { std::cout << val; }

    template<typename T>
    void println(T val) { std::cout << val << std::endl; }

    void println() { std::cout << std::endl; }

    int available() { return 0; }
    int read() { return -1; }

    void flush() { std::cout.flush(); }

    operator bool() const { return true; }
};

// Global Serial instance
inline SerialClass Serial;

// ============================================================================
// Type Definitions (Arduino compatibility)
// ============================================================================

using byte = uint8_t;
using word = uint16_t;

// ============================================================================
// Bit Manipulation
// ============================================================================

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))
#define bit(b) (1UL << (b))

#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))

// ============================================================================
// Interrupt Stubs (no-op on desktop)
// ============================================================================

inline void noInterrupts() {}
inline void interrupts() {}
inline void cli() {}
inline void sei() {}
