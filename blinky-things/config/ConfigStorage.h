#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../inputs/AdaptiveMic.h"

// Simple configuration storage with flash persistence for nRF52
class ConfigStorage {
public:
    static const uint16_t MAGIC_NUMBER = 0x8F1E;
    static const uint8_t CONFIG_VERSION = 16;  // Config schema v16: added missing persisted params (emberHeatMax, bottomRowsForSparks, burstSparks), reordered for memory efficiency

    // Fields ordered by size to minimize padding (floats, uint16, uint8/int8)
    struct StoredFireParams {
        float sparkChance;
        float audioSparkBoost;
        float heatDecay;
        float emberNoiseSpeed;
        uint16_t suppressionMs;
        uint8_t baseCooling;
        uint8_t sparkHeatMin;
        uint8_t sparkHeatMax;
        int8_t coolingAudioBias;
        uint8_t spreadDistance;
        uint8_t emberHeatMax;
        uint8_t bottomRowsForSparks;
        uint8_t burstSparks;
    };

    struct StoredMicParams {
        // Window/Range normalization parameters
        float peakTau;            // Peak adaptation speed (attack time, seconds)
        float releaseTau;         // Peak release speed (release time, seconds)
        // Hardware AGC parameters (primary - optimizes ADC signal quality)
        float hwTargetLow;        // Raw input below this → increase HW gain
        float hwTargetHigh;       // Raw input above this → decrease HW gain
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
