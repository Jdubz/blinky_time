#include "Presets.h"
#include "../inputs/AdaptiveMic.h"
#include "../music/MusicMode.h"
#include <Arduino.h>
#include <string.h>

// Preset names (must match PresetId enum order)
const char* const PresetManager::PRESET_NAMES[] = {
    "default",
    "quiet",
    "loud",
    "live"
};

/**
 * Built-in preset definitions
 *
 * Parameter Value Rationale:
 * ==========================
 * Values were determined through systematic tuning sessions using the
 * param-tuner tool (December 2024). Key metrics tracked:
 * - Music mode activation rate (target: >90% for musical content)
 * - False positive rate (target: <5% for non-musical content)
 * - Response latency (target: <100ms beat-to-light delay)
 *
 * Threshold Ranges:
 * - hitthresh (1.5-10.0): Multiples of recent average for transient detection
 *   Lower = more sensitive but more false positives
 * - attackmult (1.1-2.0): Required rise ratio for "sudden" detection
 *   Lower = catches gradual rises, higher = only sharp attacks
 * - musicthresh (0.0-1.0): Confidence required for music mode activation
 *   Lower = activates faster but with less certainty
 *
 * Timing Constants:
 * - avgtau (0.1-5.0s): Rolling average time constant
 *   Shorter = faster adaptation, longer = more stability
 * - cooldown (20-500ms): Minimum time between hits
 *   Prevents double-triggering on complex transients
 *
 * Each preset is tuned for a specific audio scenario:
 *
 * DEFAULT: Production defaults - balanced for general use
 *   Values from extended testing across multiple music genres.
 *   Standard thresholds, no adaptive features for predictable behavior.
 *
 * QUIET: For low-level/ambient audio (tuned from manual session 2024-12)
 *   Achieved 90% music mode activation from 27% baseline.
 *   - Lower hitthresh (1.5) for better detection at low levels
 *   - Higher attackmult (1.4) for more sensitive attack detection
 *   - Lower musicthresh (0.4) for easier music mode activation
 *   - Adaptive threshold enabled to scale with signal level
 *   - Faster confidence building (confinc=0.15)
 *   - More lenient beat matching (stablephase=0.25)
 *
 * LOUD: For loud live music (PA systems, concerts)
 *   - Higher hitthresh (2.5) to reject noise from compression
 *   - Lower hwtarget (0.25) for louder signal headroom (avoids clipping)
 *   - Higher musicthresh (0.7) for confident activation only
 *   - No adaptive features (signal is consistently strong)
 *   - Tighter BPM lock (3.0 max change) for stable loud sources
 *
 * LIVE: Balanced for live performance (DJ sets, bands)
 *   - Moderate thresholds for wide dynamic range
 *   - Adaptive features enabled for varying levels
 *   - 4s AGC period for medium responsiveness
 */
const PresetParams PresetManager::PRESETS[] = {
    // DEFAULT - Production defaults
    {
        .hitthresh = 2.0f,
        .attackmult = 1.2f,
        .avgtau = 0.8f,
        .cooldown = 30,
        .adaptiveThresholdEnabled = false,
        .adaptiveMinRaw = 0.1f,
        .adaptiveMaxScale = 0.6f,
        .adaptiveBlendTau = 5.0f,
        .hwtarget = 0.35f,
        .fastAgcEnabled = true,
        .fastAgcThreshold = 0.15f,
        .fastAgcPeriodMs = 5000,
        .fastAgcTrackingTau = 5.0f,
        .musicthresh = 0.6f,
        .confinc = 0.1f,
        .stablephase = 0.2f,
        .bpmLockThreshold = 0.7f,
        .bpmLockMaxChange = 5.0f,
        .bpmUnlockThreshold = 0.4f,
    },

    // QUIET - Optimized for low-level/ambient audio
    {
        .hitthresh = 1.5f,
        .attackmult = 1.4f,
        .avgtau = 0.8f,
        .cooldown = 30,
        .adaptiveThresholdEnabled = true,
        .adaptiveMinRaw = 0.1f,
        .adaptiveMaxScale = 0.5f,
        .adaptiveBlendTau = 3.0f,           // Faster adaptation for dynamic quiet sources
        .hwtarget = 0.35f,
        .fastAgcEnabled = true,
        .fastAgcThreshold = 0.12f,          // Lower threshold to trigger fast AGC sooner
        .fastAgcPeriodMs = 3000,            // Faster calibration (3s vs 5s)
        .fastAgcTrackingTau = 3.0f,         // Faster tracking
        .musicthresh = 0.4f,
        .confinc = 0.15f,
        .stablephase = 0.25f,
        .bpmLockThreshold = 0.7f,
        .bpmLockMaxChange = 5.0f,
        .bpmUnlockThreshold = 0.4f,
    },

    // LOUD - Optimized for loud sources
    {
        .hitthresh = 2.5f,
        .attackmult = 1.2f,
        .avgtau = 0.5f,
        .cooldown = 40,
        .adaptiveThresholdEnabled = false,
        .adaptiveMinRaw = 0.1f,
        .adaptiveMaxScale = 0.6f,
        .adaptiveBlendTau = 5.0f,
        .hwtarget = 0.25f,
        .fastAgcEnabled = false,            // No fast AGC needed for loud sources
        .fastAgcThreshold = 0.15f,
        .fastAgcPeriodMs = 5000,
        .fastAgcTrackingTau = 5.0f,
        .musicthresh = 0.7f,
        .confinc = 0.08f,
        .stablephase = 0.15f,
        .bpmLockThreshold = 0.75f,
        .bpmLockMaxChange = 3.0f,           // Tighter BPM lock for stable loud music
        .bpmUnlockThreshold = 0.5f,
    },

    // LIVE - Balanced for live performance
    {
        .hitthresh = 2.0f,
        .attackmult = 1.3f,
        .avgtau = 0.6f,
        .cooldown = 35,
        .adaptiveThresholdEnabled = true,
        .adaptiveMinRaw = 0.12f,
        .adaptiveMaxScale = 0.7f,
        .adaptiveBlendTau = 4.0f,
        .hwtarget = 0.30f,
        .fastAgcEnabled = true,
        .fastAgcThreshold = 0.15f,
        .fastAgcPeriodMs = 4000,
        .fastAgcTrackingTau = 4.0f,
        .musicthresh = 0.5f,
        .confinc = 0.12f,
        .stablephase = 0.2f,
        .bpmLockThreshold = 0.7f,
        .bpmLockMaxChange = 5.0f,
        .bpmUnlockThreshold = 0.4f,
    },
};

bool PresetManager::applyPreset(PresetId id, AdaptiveMic& mic, MusicMode& music) {
    if (id >= PresetId::NUM_PRESETS) {
        Serial.println(F("Invalid preset ID"));
        return false;
    }

    const PresetParams& p = PRESETS[static_cast<uint8_t>(id)];

    // Apply transient detection parameters
    mic.transientThreshold = p.hitthresh;
    mic.attackMultiplier = p.attackmult;
    mic.averageTau = p.avgtau;
    mic.cooldownMs = p.cooldown;

    // Apply adaptive threshold parameters
    mic.adaptiveThresholdEnabled = p.adaptiveThresholdEnabled;
    mic.adaptiveMinRaw = p.adaptiveMinRaw;
    mic.adaptiveMaxScale = p.adaptiveMaxScale;
    mic.adaptiveBlendTau = p.adaptiveBlendTau;

    // Apply AGC parameters
    mic.hwTarget = p.hwtarget;
    mic.fastAgcEnabled = p.fastAgcEnabled;
    mic.fastAgcThreshold = p.fastAgcThreshold;
    mic.fastAgcPeriodMs = p.fastAgcPeriodMs;
    mic.fastAgcTrackingTau = p.fastAgcTrackingTau;

    // Apply music mode parameters
    music.activationThreshold = p.musicthresh;
    music.confidenceIncrement = p.confinc;
    music.stablePhaseThreshold = p.stablephase;
    music.bpmLockThreshold = p.bpmLockThreshold;
    music.bpmLockMaxChange = p.bpmLockMaxChange;
    music.bpmUnlockThreshold = p.bpmUnlockThreshold;

    Serial.print(F("Applied preset: "));
    Serial.println(getPresetName(id));
    return true;
}

const char* PresetManager::getPresetName(PresetId id) {
    if (id >= PresetId::NUM_PRESETS) {
        return "unknown";
    }
    return PRESET_NAMES[static_cast<uint8_t>(id)];
}

PresetId PresetManager::parsePresetName(const char* name) {
    if (name == nullptr) {
        return PresetId::NUM_PRESETS;
    }

    // Case-insensitive comparison
    for (uint8_t i = 0; i < static_cast<uint8_t>(PresetId::NUM_PRESETS); i++) {
        if (strcasecmp(name, PRESET_NAMES[i]) == 0) {
            return static_cast<PresetId>(i);
        }
    }

    return PresetId::NUM_PRESETS;  // Not found
}

const PresetParams* PresetManager::getPresetParams(PresetId id) {
    if (id >= PresetId::NUM_PRESETS) {
        return nullptr;
    }
    return &PRESETS[static_cast<uint8_t>(id)];
}
