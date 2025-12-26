#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../inputs/AdaptiveMic.h"

// Simple configuration storage with flash persistence for nRF52
class ConfigStorage {
public:
    static const uint16_t MAGIC_NUMBER = 0x8F1E;
    static const uint8_t CONFIG_VERSION = 14;  // Removed peakTarget, timing constants now compile-time

    struct StoredFireParams {
        uint8_t baseCooling;
        uint8_t sparkHeatMin;
        uint8_t sparkHeatMax;
        float sparkChance;
        float audioSparkBoost;
        uint8_t audioHeatBoostMax;
        int8_t coolingAudioBias;
        uint8_t transientHeatMax;
        uint8_t spreadDistance;
        float heatDecay;
        uint16_t suppressionMs;
    };

    struct StoredMicParams {
        float noiseGate;
        // Window/Range normalization parameters
        // (Peak tracks actual signal - no target, follows loudness naturally)
        float peakTau;            // Peak adaptation speed (attack time, seconds)
        float releaseTau;         // Peak release speed (release time, seconds)
        // Hardware AGC parameters (primary - optimizes ADC signal quality)
        float hwTargetLow;        // Raw input below this → increase HW gain
        float hwTargetHigh;       // Raw input above this → decrease HW gain
        // Frequency-specific detection thresholds (always enabled)
        float kickThreshold;
        float snareThreshold;
        float hihatThreshold;
        // Note: Timing constants (transient cooldown, hw calib, hw tracking) are now compile-time constants in MicConstants
    };

    struct ConfigData {
        uint16_t magic;
        uint8_t version;
        StoredFireParams fire;
        StoredMicParams mic;
        uint8_t brightness;
    };

    ConfigStorage();
    void begin();
    bool isValid() const { return valid_; }

    void loadConfiguration(FireParams& fireParams, AdaptiveMic& mic);
    void saveConfiguration(const FireParams& fireParams, const AdaptiveMic& mic);
    void factoryReset();

    // Auto-save support
    void markDirty() { dirty_ = true; }
    void saveIfDirty(const FireParams& fireParams, const AdaptiveMic& mic);

private:
    ConfigData data_;
    bool valid_;
    bool dirty_;
    uint32_t lastSaveMs_;

    bool loadFromFlash();
    void saveToFlash();
    void loadDefaults();
};
