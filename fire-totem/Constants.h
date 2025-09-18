#pragma once

// Timing constants
namespace Constants {
    // Frame timing
    constexpr float DEFAULT_FRAME_TIME = 0.016f;  // 60 FPS default
    constexpr float MIN_FRAME_TIME = 0.001f;      // 1ms minimum
    constexpr float MAX_FRAME_TIME = 0.1f;        // 100ms maximum

    // Battery monitoring
    constexpr unsigned long BATTERY_CHECK_INTERVAL_MS = 30000;  // 30 seconds

    // Status LED timing
    constexpr unsigned long STATUS_BLINK_INTERVAL_MS = 1000;    // 1 second

    // LED clearing
    constexpr uint32_t LED_OFF = 0;  // All LEDs off color
}