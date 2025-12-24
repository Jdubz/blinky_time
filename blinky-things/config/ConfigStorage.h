#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../inputs/AdaptiveMic.h"

// Simple configuration storage with flash persistence for nRF52
class ConfigStorage {
public:
    static const uint16_t MAGIC_NUMBER = 0x8F1E;
    static const uint8_t CONFIG_VERSION = 12;  // Phase 3: Frequency-specific detection + ZCR + reliability fixes

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
        float globalGain;
        // Software AGC time constants (secondary - fine adjustments, range 0.1-10x)
        float agcAttackTau;       // Peak envelope attack
        float agcReleaseTau;      // Peak envelope release
        float agcGainTau;         // Gain adjustment speed
        // Hardware AGC parameters (primary - optimizes ADC signal quality)
        float hwTargetLow;        // Raw input below this → increase HW gain
        float hwTargetHigh;       // Raw input above this → decrease HW gain
        float hwTrackingTau;      // Time constant for tracking raw input
        // Timing parameters
        uint32_t transientCooldownMs;
        uint32_t hwCalibPeriodMs;
        // Frequency-specific detection thresholds (always enabled)
        float kickThreshold;
        float snareThreshold;
        float hihatThreshold;
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
