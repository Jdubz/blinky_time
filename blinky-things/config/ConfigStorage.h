#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../generators/Water.h"
#include "../generators/Lightning.h"
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

    // Separate versioning: device config changes rarely, settings change often
    // Bumping SETTINGS_VERSION preserves device config (LED layout, device name, etc.)
    // Bumping DEVICE_VERSION only needed when StoredDeviceConfig struct changes
    static const uint8_t DEVICE_VERSION = 1;    // Device config schema (LED layout, pins, etc.)
    static const uint8_t SETTINGS_VERSION = 2;  // Settings schema (fire, water, lightning, mic, music params)

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
        // Background
        float backgroundIntensity;
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

    struct StoredWaterParams {
        // Spawn behavior
        float baseSpawnChance;
        float audioSpawnBoost;
        // Physics
        float gravity;
        float windBase;
        float windVariation;
        float drag;
        // Drop appearance
        float dropVelocityMin;
        float dropVelocityMax;
        float dropSpread;
        // Splash behavior
        float splashVelocityMin;
        float splashVelocityMax;
        // Audio reactivity
        float musicSpawnPulse;
        float organicTransientMin;
        // Background
        float backgroundIntensity;
        // Lifecycle
        uint8_t maxParticles;
        uint8_t defaultLifespan;
        uint8_t intensityMin;
        uint8_t intensityMax;
        uint8_t splashParticles;
        uint8_t splashIntensity;
    };

    struct StoredLightningParams {
        // Spawn behavior
        float baseSpawnChance;
        float audioSpawnBoost;
        // Bolt appearance
        float boltVelocityMin;
        float boltVelocityMax;
        // Branching
        float branchAngleSpread;
        // Audio reactivity
        float musicSpawnPulse;
        float organicTransientMin;
        // Background
        float backgroundIntensity;
        // Lifecycle
        uint8_t maxParticles;
        uint8_t defaultLifespan;
        uint8_t intensityMin;
        uint8_t intensityMax;
        uint8_t fadeRate;
        uint8_t branchChance;
        uint8_t branchCount;
        uint8_t branchIntensityLoss;
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

    /**
     * StoredDeviceConfig - Serializable device configuration for flash storage
     *
     * This structure enables runtime device selection without recompilation.
     * The entire device config (LED layout, pins, battery settings, etc.) is
     * stored in flash and loaded at boot time.
     *
     * Note: Uses fixed-size fields (no pointers) for reliable flash serialization.
     */
    struct StoredDeviceConfig {
        char deviceName[32];        // Human-readable device name
        char deviceId[16];          // Unique device identifier (e.g., "hat_v1")

        // Matrix/LED configuration
        uint8_t ledWidth;           // LED matrix width (or total count for linear)
        uint8_t ledHeight;          // LED matrix height (1 for linear)
        uint8_t ledPin;             // GPIO pin for LED data
        uint8_t brightness;         // Default brightness (0-255)
        uint32_t ledType;           // NeoPixel type (e.g., NEO_GRB + NEO_KHZ800)
        uint8_t orientation;        // 0=HORIZONTAL, 1=VERTICAL
        uint8_t layoutType;         // 0=MATRIX, 1=LINEAR, 2=RANDOM

        // Charging configuration
        bool fastChargeEnabled;
        float lowBatteryThreshold;
        float criticalBatteryThreshold;
        float minVoltage;
        float maxVoltage;

        // IMU configuration
        float upVectorX;
        float upVectorY;
        float upVectorZ;
        float rotationDegrees;
        bool invertZ;
        bool swapXY;
        bool invertX;
        bool invertY;

        // Serial configuration
        uint32_t baudRate;
        uint16_t initTimeoutMs;

        // Microphone configuration
        uint16_t sampleRate;
        uint8_t bufferSize;

        // Fire effect defaults (legacy - may be deprecated in future)
        uint8_t baseCooling;
        uint8_t sparkHeatMin;
        uint8_t sparkHeatMax;
        uint8_t bottomRowsForSparks;
        float sparkChance;
        float audioSparkBoost;
        int8_t coolingAudioBias;

        // Validity flag
        bool isValid;               // Is this config populated and ready to use?

        // Reserved for future expansion
        uint8_t reserved[8];

        // Total: ~160 bytes (see static_assert enforcing sizeof(StoredDeviceConfig) <= 160)
    };

    struct ConfigData {
        uint16_t magic;
        uint8_t deviceVersion;          // Version for device config (rarely changes)
        uint8_t settingsVersion;        // Version for settings (changes more often)
        StoredDeviceConfig device;      // Device identity, LED layout, pins
        StoredFireParams fire;
        StoredWaterParams water;
        StoredLightningParams lightning;
        StoredMicParams mic;
        StoredMusicParams music;
        uint8_t brightness;
    };

    // Compile-time safety checks
    // These verify struct sizes match expected values to catch accidental changes
    // Note: Struct sizes depend on compiler padding. Sizes below are for ARM GCC.
    //
    // VERSION BUMPING RULES:
    // - StoredDeviceConfig changes -> bump DEVICE_VERSION (rare, wipes device identity)
    // - Any other struct changes -> bump SETTINGS_VERSION (preserves device config)
    static_assert(sizeof(StoredFireParams) == 56,
        "StoredFireParams size changed! Increment SETTINGS_VERSION and update assertion. (56 bytes = 12 floats + 7 uint8 + padding)");
    static_assert(sizeof(StoredWaterParams) == 64,
        "StoredWaterParams size changed! Increment SETTINGS_VERSION and update assertion. (64 bytes = 14 floats + 6 uint8 + padding)");
    static_assert(sizeof(StoredLightningParams) == 40,
        "StoredLightningParams size changed! Increment SETTINGS_VERSION and update assertion. (40 bytes = 8 floats + 8 uint8)");
    static_assert(sizeof(StoredMicParams) == 76,
        "StoredMicParams size changed! Increment SETTINGS_VERSION and update assertion. (76 bytes = 17 floats + 2 uint16 + 2 uint8 + 1 bool + padding)");
    static_assert(sizeof(StoredMusicParams) == 60,
        "StoredMusicParams size changed! Increment SETTINGS_VERSION and update assertion. (60 bytes = 14 floats + 1 bool + padding)");
    static_assert(sizeof(StoredDeviceConfig) <= 160,
        "StoredDeviceConfig size changed! Increment DEVICE_VERSION and update assertion. (Limit: 160 bytes)");
    static_assert(sizeof(ConfigData) <= 512,
        "ConfigData too large! May not fit in flash sector. Review struct padding. (Limit: 512 bytes)");

    ConfigStorage();
    void begin();
    bool isValid() const { return valid_; }

    // Device configuration accessors (v28+)
    const StoredDeviceConfig& getDeviceConfig() const { return data_.device; }
    void setDeviceConfig(const StoredDeviceConfig& config) {
        data_.device = config;
        markDirty();
    }
    bool isDeviceConfigValid() const { return data_.device.isValid; }

    /**
     * BREAKING CHANGE (v27): API now requires all 3 generator params
     *
     * Migration from v26:
     *   OLD: loadConfiguration(fireParams, mic, audioCtrl)
     *   NEW: loadConfiguration(fireParams, waterParams, lightningParams, mic, audioCtrl)
     *
     * Rationale: Unified particle system requires persisting all generators,
     * not just Fire. This ensures Water and Lightning settings survive reboots.
     */
    void loadConfiguration(FireParams& fireParams, WaterParams& waterParams, LightningParams& lightningParams,
                          AdaptiveMic& mic, AudioController* audioCtrl = nullptr);
    void saveConfiguration(const FireParams& fireParams, const WaterParams& waterParams, const LightningParams& lightningParams,
                          const AdaptiveMic& mic, const AudioController* audioCtrl = nullptr);
    void factoryReset();

    // Auto-save support
    void markDirty() { dirty_ = true; }
    void saveIfDirty(const FireParams& fireParams, const WaterParams& waterParams, const LightningParams& lightningParams,
                     const AdaptiveMic& mic, const AudioController* audioCtrl = nullptr);

private:
    ConfigData data_;
    bool valid_;
    bool dirty_;
    uint32_t lastSaveMs_;

    bool loadFromFlash();
    void saveToFlash();
    void loadDefaults();
    void loadDeviceDefaults();    // Reset only device config (LED layout, pins)
    void loadSettingsDefaults();  // Reset only settings (fire, water, lightning, mic, music)
};
