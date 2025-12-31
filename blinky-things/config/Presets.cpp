#include "Presets.h"
#include "../inputs/AdaptiveMic.h"
#include "../audio/AudioController.h"
#include <Arduino.h>
#include <string.h>

// Preset names (must match PresetId enum order)
const char* const PresetManager::PRESET_NAMES[] = {
    "default"
};

/**
 * Built-in preset definitions
 *
 * NOTE: Only DEFAULT preset exists. Quiet/loud mode adaptation is handled
 * AUTOMATICALLY by the AGC system based on gain levels (inFastAgcMode_).
 *
 * The AGC automatically enters "fast mode" when gain is maxed out and
 * signal is persistently low, providing the quiet-source optimization
 * without manual preset selection.
 *
 * Parameter Value Rationale:
 * ==========================
 * Values were determined through systematic tuning sessions using the
 * param-tuner tool (December 2024). Key metrics tracked:
 * - Rhythm tracking activation rate (target: >90% for musical content)
 * - False positive rate (target: <5% for non-musical content)
 * - Response latency (target: <100ms beat-to-light delay)
 */
const PresetParams PresetManager::PRESETS[] = {
    // DEFAULT - Production defaults (tuned via fast-tune 2025-12-30)
    {
        .hitthresh = 2.813f,                // Hybrid-optimal threshold (conservative)
        .attackmult = 1.1f,                 // 10% sudden rise required
        .avgtau = 0.8f,
        .cooldown = 80,                     // Increased from 40 to reduce false positives
        .adaptiveThresholdEnabled = false,
        .adaptiveMinRaw = 0.1f,
        .adaptiveMaxScale = 0.6f,
        .adaptiveBlendTau = 5.0f,
        .hwtarget = 0.35f,
        .fastAgcEnabled = true,             // Auto quiet-mode when gain maxed
        .fastAgcThreshold = 0.15f,
        .fastAgcPeriodMs = 5000,
        .fastAgcTrackingTau = 5.0f,
        .musicthresh = 0.4f,
    },
};

bool PresetManager::applyPreset(PresetId id, AdaptiveMic& mic, AudioController* audioCtrl) {
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

    // Apply audio controller rhythm parameters (if available)
    if (audioCtrl) {
        audioCtrl->activationThreshold = p.musicthresh;
    }

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
