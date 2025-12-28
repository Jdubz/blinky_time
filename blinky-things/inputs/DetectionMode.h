#pragma once
#include <stdint.h>

/**
 * Detection Mode - Selects which onset detection algorithm is active
 *
 * Each algorithm has different strengths:
 * - DRUMMER: Time-domain amplitude spikes (fast, simple, current default)
 * - BASS_BAND: Biquad lowpass filter focusing on kick frequencies (60-200Hz)
 * - HFC: High Frequency Content emphasizes percussive transients
 * - SPECTRAL_FLUX: FFT-based, compares magnitude spectra between frames
 * - HYBRID: Combines DRUMMER + SPECTRAL_FLUX for confidence scoring
 */
enum class DetectionMode : uint8_t {
    DRUMMER = 0,        // Current "Drummer's Algorithm" - amplitude-based
    BASS_BAND = 1,      // Biquad lowpass focus on kicks
    HFC = 2,            // High frequency content for percussive
    SPECTRAL_FLUX = 3,  // FFT-based spectral difference
    HYBRID = 4,         // Combined drummer + spectral flux (confidence scoring)
    MODE_COUNT = 5      // Number of modes (for bounds checking)
};

/**
 * Detection mode names for serial console display
 */
inline const char* getDetectionModeName(DetectionMode mode) {
    switch (mode) {
        case DetectionMode::DRUMMER:      return "drummer";
        case DetectionMode::BASS_BAND:    return "bass";
        case DetectionMode::HFC:          return "hfc";
        case DetectionMode::SPECTRAL_FLUX: return "flux";
        case DetectionMode::HYBRID:       return "hybrid";
        default:                          return "unknown";
    }
}

/**
 * Parse detection mode from string (case-insensitive)
 * Returns true if valid, false otherwise
 */
inline bool parseDetectionMode(const char* str, DetectionMode& mode) {
    if (!str) return false;

    // Check numeric values first
    if (str[0] >= '0' && str[0] <= '4' && str[1] == '\0') {
        mode = static_cast<DetectionMode>(str[0] - '0');
        return true;
    }

    // Check string names (case-insensitive first char for speed)
    char c = str[0];
    if (c >= 'A' && c <= 'Z') c += 32;  // tolower

    if (c == 'd') { mode = DetectionMode::DRUMMER; return true; }
    if (c == 'b') { mode = DetectionMode::BASS_BAND; return true; }
    if (c == 'f' || c == 's') { mode = DetectionMode::SPECTRAL_FLUX; return true; }
    if (c == 'y') { mode = DetectionMode::HYBRID; return true; }
    // 'h' could be HFC or hybrid - check second char
    if (c == 'h') {
        if (str[1] == 'y' || str[1] == 'Y') {
            mode = DetectionMode::HYBRID;
        } else {
            mode = DetectionMode::HFC;
        }
        return true;
    }

    return false;
}

/**
 * Clamp detection mode to valid range
 * Returns 0 (DRUMMER) if out of range - safe default
 */
inline uint8_t clampDetectionMode(uint8_t mode) {
    return (mode < static_cast<uint8_t>(DetectionMode::MODE_COUNT)) ? mode : 0;
}
