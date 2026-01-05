#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../inputs/AdaptiveMic.h"

// Forward declarations
class AudioController;

/**
 * ConfigStorage - Flash-based configuration persistence for nRF52
 *
 * Version Migration Policy:
 *   When CONFIG_VERSION changes, all older configs are intentionally DISCARDED
 *   and factory defaults are loaded. This is by design:
 *   - Config schema changes often make old data invalid or misinterpretable
 *   - Parameters can be re-tuned via serial console after upgrade
 *   - Safer than attempting complex migration logic on embedded device
 *
 *   If you need to preserve user settings across versions, manually save
 *   current tuning values before upgrading, then restore via serial console.
 */
class ConfigStorage {
public:
    static const uint16_t MAGIC_NUMBER = 0x8F1E;
    static const uint8_t CONFIG_VERSION = 26;  // Config schema v26: particle-based generators

    // Fields ordered by size to minimize padding (floats, uint16, uint8/int8)
    struct StoredFireParams {
        // Spawn behavior
        float baseSpawnChance;
        float audioSpawnBoost;
        // Physics
        float gravity;
        float windBase;
        float windVariation;
        float drag;
        // Spark appearance
        float sparkVelocityMin;
        float sparkVelocityMax;
        float sparkSpread;
        // Audio reactivity
        float musicSpawnPulse;
        float organicTransientMin;
        // Lifecycle
        uint8_t maxParticles;
        uint8_t defaultLifespan;
        uint8_t intensityMin;
        uint8_t intensityMax;
        // Heat trail
        uint8_t trailHeatFactor;
        uint8_t trailDecay;
        uint8_t burstSparks;
    };

    struct StoredMicParams {
        // Window/Range normalization parameters
        float peakTau;            // Peak adaptation speed (attack time, seconds)
        float releaseTau;         // Peak release speed (release time, seconds)
        // Hardware AGC parameters
        float hwTarget;           // Target raw input level (±0.01 dead zone)

        // Fast AGC parameters (new in v24+)
        float fastAgcThreshold;   // Raw level threshold to trigger fast AGC
        float fastAgcTrackingTau; // Tracking tau in fast mode (seconds)
        uint16_t fastAgcPeriodMs; // Calibration period in fast mode (ms)
        bool fastAgcEnabled;      // Enable fast AGC when signal is low

        // LEGACY: Detection parameters (kept for backward compatibility)
        // These are now handled by EnsembleDetector, not AdaptiveMic
        float transientThreshold; // [LEGACY] Hit threshold
        float attackMultiplier;   // [LEGACY] Attack multiplier
        float averageTau;         // [LEGACY] Recent average tracking time
        float bassFreq;           // [LEGACY] Filter cutoff frequency
        float bassQ;              // [LEGACY] Filter Q factor
        float bassThresh;         // [LEGACY] Bass detection threshold
        float hfcWeight;          // [LEGACY] HFC weighting factor
        float hfcThresh;          // [LEGACY] HFC detection threshold
        float fluxThresh;         // [LEGACY] Spectral flux threshold
        float hybridFluxWeight;   // [LEGACY] Weight when only flux detects
        float hybridDrumWeight;   // [LEGACY] Weight when only drummer detects
        float hybridBothBoost;    // [LEGACY] Multiplier when both agree
        uint16_t cooldownMs;      // [LEGACY] Cooldown between hits (ms)
        uint8_t detectionMode;    // [LEGACY] 0=drummer, 1=bass, 2=hfc, 3=flux, 4=hybrid
        uint8_t fluxBins;         // [LEGACY] FFT bins to analyze
    };

    struct StoredMusicParams {
        // Basic rhythm parameters
        float activationThreshold;
        float bpmMin;
        float bpmMax;
        float phaseAdaptRate;  // How quickly phase adapts to autocorrelation

        // Tempo prior (CRITICAL for correct BPM tracking)
        float tempoPriorCenter;    // Center of Gaussian prior (BPM)
        float tempoPriorWidth;     // Width (sigma) of prior
        float tempoPriorStrength;  // Blend: 0=no prior, 1=full prior
        bool tempoPriorEnabled;    // Enable tempo prior weighting

        // Pulse modulation
        float pulseBoostOnBeat;      // Boost factor for on-beat transients
        float pulseSuppressOffBeat;  // Suppress factor for off-beat transients
        float energyBoostOnBeat;     // Energy boost near predicted beats

        // Stability and smoothing
        float stabilityWindowBeats;   // Number of beats for stability tracking
        float beatLookaheadMs;        // How far ahead to predict beats (ms)
        float tempoSmoothingFactor;   // Higher = smoother tempo changes
        float tempoChangeThreshold;   // Min BPM change ratio to trigger update

        // Total: 14 floats (56 bytes) + 1 bool (1 byte) + 3 padding = 60 bytes
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
    static_assert(sizeof(StoredMicParams) == 76,
        "StoredMicParams size changed! Increment CONFIG_VERSION and update assertion. (76 bytes = 17 floats + 2 uint16 + 2 uint8 + 1 bool + padding)");
    static_assert(sizeof(StoredMusicParams) == 60,
        "StoredMusicParams size changed! Increment CONFIG_VERSION and update assertion. (60 bytes = 14 floats + 1 bool + padding)");
    static_assert(sizeof(ConfigData) <= 230,
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
