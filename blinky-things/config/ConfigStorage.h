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
    // Versions 8-11 were intermediate experimental builds during CBSS development (never released)
    // Version 13: Added comb bank cross-validation params (combCrossValMinConf, combCrossValMinCorr)
    // Version 14: Added hpsEnabled (Harmonic Product Spectrum)
    // Version 15: Added pulseTrainEnabled, pulseTrainCandidates
    // Version 16: Added IOI histogram cross-validation (ioiEnabled, ioiMinPeakRatio, ioiMinAutocorr)
    // Version 17: Added ODF mean subtraction + Fourier tempogram (odfMeanSubEnabled, ftEnabled, ftMinMagnitudeRatio, ftMinAutocorr)
    // Version 18: Bayesian tempo fusion (replaced 17 sequential-override params with 6 Bayesian weights)
    // Version 19: Added bayesPriorWeight (ongoing static prior strength)
    // Version 20: Added cbssThresholdFactor (CBSS adaptive threshold)
    // Versions 21-24 were development iterations on the staging branch (Feb 2026).
    // No field devices ever persisted v21-23 settings — they existed only during
    // active tuning sessions and were reset between firmware uploads.
    // Version 21: Bayesian weight tuning (FT/IOI disabled, ACF 1.0→0.3, cbssThresh 0.4→1.0, lambda 0.1→0.15)
    // Version 22: Combined Bayesian validation (bayesacf=0.3, cbssthresh=1.0 — 4-device validated defaults)
    // Version 23: Spectral processing (adaptive whitening + soft-knee compressor)
    // Version 24: Post-spectral Bayesian re-tuning (bayesft=2.0, bayesioi=2.0 — re-enabled by spectral processing)
    // Version 25: BTrack-style octave error fixes (harmonic comb ACF, Rayleigh prior, tighter lambda, bidirectional disambig)
    // Version 26: BlinkyAssert visible errors, per-param validation, VALIDATE_INT macro
    // Version 27: Removed legacy detection params from StoredMicParams, prefixed water/lightning settings
    // Version 28 (shipped as 29): Phase 1a — FT+IOI disabled by default (bayesft=0, bayesioi=0, ftEnabled=false, ioiEnabled=false)
    //             Phase 2.1 — Beat-boundary tempo updates (pendingBeatPeriod_)
    //             Phase 2.6 — Dual-threshold peak picking (local-max confirmation)
    //             Phase 2.4 — Unified ODF (BandFlux pre-threshold feeds CBSS)
    //             Phase 2.2 — Increased tempo bins (20→40)
    // Version 29: All BandFlux detector params persisted (StoredBandFluxParams added to ConfigData)
    //             peakPickEnabled first persisted (in StoredBandFluxParams)
    static const uint8_t SETTINGS_VERSION = 29;  // Settings schema (fire, water, lightning, mic, music, bandflux params)

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
        // Particle variety
        float fastSparkRatio;
        float thermalForce;       // Thermal buoyancy strength (LEDs/sec^2)
        // Lifecycle
        uint8_t maxParticles;
        uint8_t defaultLifespan;
        uint8_t intensityMin;
        uint8_t intensityMax;
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

        // Fast AGC parameters
        float fastAgcThreshold;   // Raw level threshold to trigger fast AGC
        float fastAgcTrackingTau; // Tracking tau in fast mode (seconds)
        uint16_t fastAgcPeriodMs; // Calibration period in fast mode (ms)
        bool fastAgcEnabled;      // Enable fast AGC when signal is low
    };

    struct StoredMusicParams {
        // Basic rhythm parameters
        float activationThreshold;
        float bpmMin;
        float bpmMax;
        float cbssAlpha;       // CBSS weighting (0.8-0.95, higher = more predictive)

        // Tempo prior width (used by Bayesian static prior)
        float tempoPriorWidth;     // Width (sigma) of prior

        // Pulse modulation
        float pulseBoostOnBeat;      // Boost factor for on-beat transients
        float pulseSuppressOffBeat;  // Suppress factor for off-beat transients
        float energyBoostOnBeat;     // Energy boost near predicted beats

        // Stability and smoothing
        float stabilityWindowBeats;   // Number of beats for stability tracking
        float beatLookaheadMs;        // How far ahead to predict beats (ms)
        float tempoSmoothingFactor;   // Higher = smoother tempo changes
        float tempoChangeThreshold;   // Min BPM change ratio to trigger update

        // CBSS beat tracking
        float cbssTightness;            // Log-Gaussian tightness (higher=stricter tempo)
        float beatConfidenceDecay;      // Per-frame confidence decay when no beat detected
        float beatTimingOffset;         // Beat prediction advance in frames (ODF+CBSS delay compensation)
        float phaseCorrectionStrength;  // Phase correction toward transients (0=off, 1=full snap)
        float cbssThresholdFactor;      // CBSS adaptive threshold: beat fires only if CBSS > factor * mean (0=off)

        // Bayesian tempo fusion (v18)
        float bayesLambda;              // Transition tightness (0.01=rigid, 1.0=loose)
        float bayesPriorCenter;         // Static prior center BPM (Gaussian)
        float bayesPriorWeight;         // Ongoing static prior strength (0=off, 1=standard, 2=strong)
        float bayesAcfWeight;           // Autocorrelation observation weight
        float bayesFtWeight;            // Fourier tempogram observation weight
        float bayesCombWeight;          // Comb filter bank observation weight
        float bayesIoiWeight;           // IOI histogram observation weight

        uint8_t odfSmoothWidth;         // ODF smooth window (3-11, odd)
        bool ioiEnabled;                // Enable IOI histogram observation
        bool odfMeanSubEnabled;         // Enable ODF mean subtraction before autocorrelation
        bool ftEnabled;                 // Enable Fourier tempogram observation
        bool beatBoundaryTempo;         // Defer tempo changes to beat boundaries (v28)
        bool unifiedOdf;                // Use BandFlux pre-threshold as CBSS ODF (v28)

        // Spectral processing (v23+)
        bool whitenEnabled;             // Per-bin spectral whitening
        bool compressorEnabled;         // Soft-knee compressor
        float whitenDecay;              // Peak decay per frame (~5s at 0.997)
        float whitenFloor;              // Noise floor for whitening
        float compThresholdDb;          // Compressor threshold (dB)
        float compRatio;                // Compression ratio (e.g., 3:1)
        float compKneeDb;              // Soft knee width (dB)
        float compMakeupDb;            // Makeup gain (dB)
        float compAttackTau;           // Attack time constant (seconds)
        float compReleaseTau;          // Release time constant (seconds)
    };

    struct StoredBandFluxParams {
        // Core ODF parameters
        float gamma;                // Log compression strength (1-100)
        float bassWeight;           // Bass band weight (0-5)
        float midWeight;            // Mid band weight (0-5)
        float highWeight;           // High band weight (0-2)
        float minOnsetDelta;        // Min flux jump for onset (0-2, pad rejection)
        float perBandThreshMult;    // Per-band threshold multiplier (0.5-5)
        // Experimental gates (all 0.0 = disabled)
        float bandDominanceGate;    // Band-dominance ratio gate (0-1)
        float decayRatioThreshold;  // Post-onset decay confirmation (0-1)
        float crestGate;            // Spectral crest factor gate (0-20)
        // Integer params
        uint8_t maxBin;             // Max FFT bin to analyze (16-128)
        uint8_t confirmFrames;      // Decay check frames (0-6)
        uint8_t diffFrames;         // Temporal reference depth (1-3)
        // Feature toggles
        bool perBandThreshEnabled;  // Per-band independent detection
        bool hiResBassEnabled;      // Hi-res bass via Goertzel
        bool peakPickEnabled;       // Local-max peak picking (Phase 2.6)
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
        StoredBandFluxParams bandflux;
        uint8_t brightness;
    };

    // Compile-time safety checks
    // These verify struct sizes match expected values to catch accidental changes
    // Note: Struct sizes depend on compiler padding. Sizes below are for ARM GCC.
    //
    // VERSION BUMPING RULES:
    // - StoredDeviceConfig changes -> bump DEVICE_VERSION (rare, wipes device identity)
    // - Any other struct changes -> bump SETTINGS_VERSION (preserves device config)
    static_assert(sizeof(StoredFireParams) == 64,
        "StoredFireParams size changed! Increment SETTINGS_VERSION and update assertion. (64 bytes = 14 floats + 5 uint8 + padding)");
    static_assert(sizeof(StoredWaterParams) == 64,
        "StoredWaterParams size changed! Increment SETTINGS_VERSION and update assertion. (64 bytes = 14 floats + 6 uint8 + padding)");
    static_assert(sizeof(StoredLightningParams) == 32,
        "StoredLightningParams size changed! Increment SETTINGS_VERSION and update assertion. (32 bytes = 6 floats + 8 uint8)");
    static_assert(sizeof(StoredMicParams) == 24,
        "StoredMicParams size changed! Increment SETTINGS_VERSION and update assertion. (24 bytes = 5 floats + 1 uint16 + 1 bool + padding)");
    static_assert(sizeof(StoredMusicParams) == 136,
        "StoredMusicParams size changed! Increment SETTINGS_VERSION and update assertion. (136 bytes = 32 floats + 1 uint8 + 7 bools + padding)");
    static_assert(sizeof(StoredBandFluxParams) == 44,
        "StoredBandFluxParams size changed! Increment SETTINGS_VERSION and update assertion. (44 bytes = 9 floats + 3 uint8 + 3 bools + padding)");
    static_assert(sizeof(StoredDeviceConfig) <= 160,
        "StoredDeviceConfig size changed! Increment DEVICE_VERSION and update assertion. (Limit: 160 bytes)");
    // ConfigData: ~541 bytes (4+160+64+64+32+24+136+44+1 + padding). Allocated in last 4KB flash page.
    // Tight bound (640) catches accidental struct bloat. Raise when genuinely needed + bump SETTINGS_VERSION.
    static_assert(sizeof(ConfigData) <= 640,
        "ConfigData exceeds 640 bytes! Current estimate ~541B. Check for unintended struct growth.");

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
