#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../inputs/AdaptiveMic.h"

// Forward declarations
class AudioController;

// Simple configuration storage with flash persistence for nRF52
class ConfigStorage {
public:
    static const uint16_t MAGIC_NUMBER = 0x8F1E;
    static const uint8_t CONFIG_VERSION = 23;  // Config schema v23: simplified rhythm tracking (removed PLL)

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
        // Hardware AGC parameters
        float hwTarget;           // Target raw input level (±0.01 dead zone)
        // Shared transient detection parameters
        float transientThreshold; // Hit threshold (multiples of recent average)
        float attackMultiplier;   // Attack multiplier (sudden rise ratio)
        float averageTau;         // Recent average tracking time (seconds)
        // Bass band filter parameters
        float bassFreq;           // Filter cutoff frequency (Hz)
        float bassQ;              // Filter Q factor
        float bassThresh;         // Bass detection threshold
        // HFC parameters
        float hfcWeight;          // HFC weighting factor
        float hfcThresh;          // HFC detection threshold
        // Spectral flux parameters
        float fluxThresh;         // Spectral flux threshold
        // Hybrid mode parameters (mode 4) - tuned 2024-12
        float hybridFluxWeight;   // Weight when only flux detects
        float hybridDrumWeight;   // Weight when only drummer detects
        float hybridBothBoost;    // Multiplier when both agree
        // uint16_t members
        uint16_t cooldownMs;      // Cooldown between hits (ms)
        // uint8_t members
        uint8_t detectionMode;    // 0=drummer, 1=bass, 2=hfc, 3=flux, 4=hybrid
        uint8_t fluxBins;         // FFT bins to analyze
        // Total: 15 floats (60) + 1 uint16 (2) + 2 uint8 (2) = 64 bytes
    };

    struct StoredMusicParams {
        float activationThreshold;
        float bpmMin;
        float bpmMax;
        float phaseAdaptRate;  // How quickly phase adapts to autocorrelation
        // Total: 4 floats = 16 bytes
    };

    struct ConfigData {
        uint16_t magic;
        uint8_t version;
        StoredFireParams fire;
        StoredMicParams mic;
        StoredMusicParams music;
        uint8_t brightness;
    };

    // Compile-time safety checks
    // These verify struct sizes match expected values to catch accidental changes
    // If these fail, you MUST increment CONFIG_VERSION!
    static_assert(sizeof(StoredMicParams) == 64,
        "StoredMicParams size changed! Increment CONFIG_VERSION and update assertion. (64 bytes = 15 floats + 1 uint16 + 2 uint8)");
    static_assert(sizeof(StoredMusicParams) == 16,
        "StoredMusicParams size changed! Increment CONFIG_VERSION and update assertion. (16 bytes = 4 floats)");
    static_assert(sizeof(ConfigData) <= 164,
        "ConfigData too large! May not fit in flash sector. Review struct padding.");

    ConfigStorage();
    void begin();
    bool isValid() const { return valid_; }

    void loadConfiguration(FireParams& fireParams, AdaptiveMic& mic, AudioController* audioCtrl = nullptr);
    void saveConfiguration(const FireParams& fireParams, const AdaptiveMic& mic, const AudioController* audioCtrl = nullptr);
    void factoryReset();

    // Auto-save support
    void markDirty() { dirty_ = true; }
    void saveIfDirty(const FireParams& fireParams, const AdaptiveMic& mic, const AudioController* audioCtrl = nullptr);

private:
    ConfigData data_;
    bool valid_;
    bool dirty_;
    uint32_t lastSaveMs_;

    bool loadFromFlash();
    void saveToFlash();
    void loadDefaults();
};
