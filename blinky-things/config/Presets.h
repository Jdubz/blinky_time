#pragma once

#include <stdint.h>

// Forward declarations
class AdaptiveMic;
class AudioController;

/**
 * Audio parameter presets for quick configuration changes
 *
 * Presets store tuned parameter sets for different audio scenarios:
 * - DEFAULT: Production defaults (balanced for most situations)
 * - QUIET: Optimized for low-level/ambient audio
 * - LOUD: Optimized for loud live music
 * - LIVE: Balanced for live performance
 *
 * Usage:
 *   PresetManager::applyPreset(PresetId::QUIET, mic, music);
 */

enum class PresetId : uint8_t {
    DEFAULT = 0,    // Production defaults
    QUIET = 1,      // Optimized for quiet/ambient audio
    LOUD = 2,       // Optimized for loud sources
    LIVE = 3,       // Balanced for live performance
    NUM_PRESETS
};

/**
 * Preset parameter values
 * Contains all tunable parameters affected by presets
 *
 * Naming convention: Field names use abbreviated serial command names
 * (e.g., 'hitthresh') for consistency with the serial API, while
 * AdaptiveMic member variables use descriptive names (e.g.,
 * 'transientThreshold') for code clarity. This is intentional to
 * maintain backwards compatibility with existing serial commands
 * while keeping internal code self-documenting.
 */
struct PresetParams {
    // Transient detection (serial: hitthresh, attackmult, avgtau, cooldown)
    float hitthresh;         // Maps to AdaptiveMic::transientThreshold
    float attackmult;
    float avgtau;
    uint16_t cooldown;

    // Adaptive threshold
    bool adaptiveThresholdEnabled;
    float adaptiveMinRaw;
    float adaptiveMaxScale;
    float adaptiveBlendTau;

    // AGC
    float hwtarget;
    bool fastAgcEnabled;
    float fastAgcThreshold;
    uint16_t fastAgcPeriodMs;
    float fastAgcTrackingTau;

    // Music mode
    float musicthresh;
    float confinc;
    float stablephase;

    // BPM locking
    float bpmLockThreshold;
    float bpmLockMaxChange;
    float bpmUnlockThreshold;
};

/**
 * Preset manager - applies and queries audio presets
 */
class PresetManager {
public:
    /**
     * Apply a preset to the audio system
     * @param id Preset to apply
     * @param mic AdaptiveMic instance to configure
     * @param audioCtrl AudioController instance to configure (optional)
     * @return true if preset was applied successfully, false if invalid preset ID
     */
    static bool applyPreset(PresetId id, AdaptiveMic& mic, AudioController* audioCtrl = nullptr);

    /**
     * Get the name of a preset
     * @param id Preset ID
     * @return Human-readable name (e.g., "quiet", "loud")
     */
    static const char* getPresetName(PresetId id);

    /**
     * Parse a preset name to ID
     * @param name Name string (case-insensitive)
     * @return Preset ID, or NUM_PRESETS if not found
     */
    static PresetId parsePresetName(const char* name);

    /**
     * Get preset parameters (for inspection/debugging)
     * @param id Preset ID
     * @return Pointer to preset params, or nullptr if invalid
     */
    static const PresetParams* getPresetParams(PresetId id);

    /**
     * Get number of available presets
     */
    static constexpr uint8_t getPresetCount() { return static_cast<uint8_t>(PresetId::NUM_PRESETS); }

private:
    // Built-in preset definitions
    static const PresetParams PRESETS[];
    static const char* const PRESET_NAMES[];
};
