#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../inputs/AdaptiveMic.h"

// Simple configuration storage with flash persistence for nRF52
class ConfigStorage {
public:
    static const uint16_t MAGIC_NUMBER = 0x8F1E;
    static const uint8_t CONFIG_VERSION = 18;  // Config schema v18: added advanced onset parameters (baselineAttackTau, baselineReleaseTau, logCompressionFactor, riseWindowMs)

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
        float hwTarget;           // Target raw input level (±0.01 dead zone)
        // Onset detection thresholds (two-band system)
        float onsetThreshold;     // Multiples of baseline for onset detection
        float riseThreshold;      // Ratio to previous frame for rise detection
        // Advanced onset detection parameters (new)
        float baselineAttackTau;  // Baseline attack time constant
        float baselineReleaseTau; // Baseline release time constant
        float logCompressionFactor; // Log compression factor (0=disabled)
        uint16_t riseWindowMs;    // Multi-frame rise detection window
    };

    struct ConfigData {
        uint16_t magic;
        uint8_t version;
        StoredFireParams fire;
        StoredMicParams mic;
        uint8_t brightness;
    };

    // Compile-time safety checks
    // These verify struct sizes match expected values to catch accidental changes
    // If these fail, you MUST increment CONFIG_VERSION!
    static_assert(sizeof(StoredMicParams) == 34,
        "StoredMicParams size changed! Increment CONFIG_VERSION and update assertion.");
    static_assert(sizeof(ConfigData) <= 80,
        "ConfigData too large! May not fit in flash sector. Review struct padding.");

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
