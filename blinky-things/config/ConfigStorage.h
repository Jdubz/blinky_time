#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../generators/Water.h"
#include "../generators/Lightning.h"
#include "../inputs/AdaptiveMic.h"

class AudioTracker;

// AudioController removed v74 — replaced by AudioTracker.
// AudioTracker params persisted via StoredTrackerParams (v74).

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
    // Version 30: posteriorFloor/disambigNudge/harmonicTransWeight; ACF lag normalization; MIN_LAG 20→18; NUM_FILTERS 40→20
    // Version 31: Adaptive ODF threshold, onset-density octave discriminator, shadow CBSS octave checker (internal dev, never deployed)
    // Version 32: ODF mean sub disabled, density octave + octave checker defaults (first deployed with v31 features)
    // Version 33: BTrack-style tempo pipeline (Viterbi max-product + comb-on-ACF adaptive threshold)
    // Version 34: Bar-pointer HMM beat tracking (Phase 3.1, joint tempo-phase)
    // Version 35: odfDiffMode, densityPenaltyExp, densityTarget (ODF experiments)
    // Version 36: bassEnergyOdf (bass energy envelope for ACF tempo estimation)
    // Version 37: cbssContrast, cbssWarmupBeats, onsetSnapWindow, odfThreshWindow,
    //             onsetTrainOdf, odfDiffMode, odfSource, densityPenaltyExp,
    //             densityTarget, phaseCheck{Enabled,Beats,Ratio}, HMM params
    // Version 38: Particle filter beat tracking (100 particles, octave injection)
    // Version 39: Bar-pointer PF (beat-boundary diffusion, madmom obs model, info gate, phase-coherent octave)
    // Version 40: cbssTightness 5→8 (+24% avg Beat F1 in 3-track sweep)
    // Version 41: downwardCorrectEnabled toggle (disabled by default), deprecated PF params
    // Version 42: PLP phase extraction (plpPhaseEnabled, plpCorrectionStrength, plpMinConfidence)
    //            v43 algorithmic fixes (no new settings): removed double inverse-lag, full-res comb,
    //            octave folding, lag-space transition matrix. Struct reordered (bool padding fix).
    // Version 43: 128 BPM gravity well fixes (rayleighBpm, tempoNudge, fold32, sesquicheck, bisnap, harmonicSesqui)
    // Version 44: Remove proven-detrimental features (harmonicSesqui, PLP phase, phase check)
    // Version 45: Percival ACF harmonic pre-enhancement, PLL phase correction, adaptive CBSS tightness
    // Version 46: HMM beat detection (position-0 wrap replaces CBSS countdown when hmm=1)
    // Version 47: Pre-whitening BandFlux path + bass whitening bypass (signal chain decompression)
    // Version 48: Multi-agent beat tracking, anti-harmonic 3rd comb, metrical contrast check
    // Version 49: Continuous ODF observation model for phase tracker (fwdObsLambda, replaces Bernoulli)
    // Version 50: Rhythmic pattern templates + beat critic subbeat alternation (octave disambiguation)
    // Version 51: Hidden calibration constants exposed as serial settings (10 new params)
    // Version 52: Joint forward filter fix (fwdObsFloor, fwdWrapFraction) + remove FT/IOI dead code
    // Version 53: Remove joint HMM dead code (hmmTempoNorm, hmmLambda, hmmBayesBias)
    // Version 54: Add nnBeatActivation (NN beat ODF toggle)
    // Version 55: Conservative AGC (hwGainMaxSignal 60→40, hwTarget 0.35→0.20, tracking tau 30→60s)
    // Version 56: Spectral noise estimation (noiseEst*, 5 new params) — same commit as v55
    // Version 57: Joint tempo-phase forward filter (forwardFilter*, 5 new params)
    // Version 58: Hybrid phase tracker (fwdPhaseOnly), nnBeatActivation default ON
    // Version 59: Bayesian posterior bias for forward filter (fwdBayesBias)
    // Version 60: Asymmetric non-beat observation model for forward filter (fwdAsymmetry)
    // Version 61: rayleighBpm default 120→140 (sweep: fewer octave errors), nnProfile setting
    // Versions 62-63: skipped (never released; reserved during development)
    // Version 64: Removed dead features: forward filter, particle filter, HMM phase tracker,
    //   multi-agent, template/subbeat/metrical checks, ODF sources 1-5, legacy spectral flux.
    //   All A/B tested: zero or negative benefit vs CBSS baseline. Saves ~1500 lines, ~40 KB flash.
    //   Devices on v61 or earlier will factory-reset on first boot with v64 firmware.
    // Version 65: Replace BeatActivationNN/BeatSyncNN/SpectralAccumulator with FrameOnsetNN.
    //   Mel-CNN (79-98ms) and beat-sync hybrid both closed. Frame-level FC approach.
    // Version 66: cbssContrast default 1.0→2.0 (A/B tested 10-6 win). Bump forces
    //   devices with saved v65 settings to pick up the new default on first boot.
    // Version 67: Removed BandFlux/EnsembleDetector pipeline (StoredBandFluxParams removed,
    //   unifiedOdf/onsetTrainOdf/odfDiffMode removed from StoredMusicParams). FrameOnsetNN is
    //   sole ODF source. Pulse detection inlined in AudioController.
    // Version 68: Removed nnBeatActivation toggle and ENABLE_NN_BEAT_ACTIVATION ifdef.
    //   FrameOnsetNN is always compiled in and always active. TFLite is a required dependency.
    // Version 69: Dimension-independent generator params (maxParticles/burstSparks float).
    // Version 70: Persist v65 runtime-only params (pllWarmupBeats, snapHysteresis,
    //   dbEmaAlpha, dbThreshold, dbDecay). Previously tunable via serial but lost on reboot.
    // Version 71: Fix fastAgcPeriodMs/fastAgcTrackingTau defaults to match AdaptiveMic.h
    // Version 72: Remove AGC — hardware gain fixed at platform optimal. StoredMicParams
    //   reduced to peakTau + releaseTau only. Window/range normalization handles all adaptation.
    // Version 73: AudioController replaced by AudioTracker (ACF+Comb+PLL, ~10 params).
    //   AudioController's ~56 runtime params no longer read from StoredMusicParams.
    //   StoredMusicParams struct preserved in flash layout for version compatibility
    //   (devices with v73 flash won't factory-reset). Pulse baseline tracking fix.
    // Version 74: AudioTracker params persisted (StoredTrackerParams added to ConfigData).
    //   Previously serial-only (~15 params). Also exposes hardcoded PLL/pulse/energy
    //   constants as tunable params (~18 new params). Total: ~35 tracker params persisted.
    static const uint8_t SETTINGS_VERSION = 89;  // v89: pulseNNGate — NN activation gate for pulse detection

    // Fields ordered by size to minimize padding (floats, uint16, uint8/int8)
    struct StoredFireParams {
        // Spawn behavior
        float baseSpawnChance;
        float audioSpawnBoost;
        // Physics
        float windVariation;
        float drag;
        // Spark appearance
        float sparkVelocityMin;
        float sparkVelocityMax;
        float sparkSpread;
        // Audio reactivity
        float organicTransientMin;
        float thermalForce;       // × traversalDim → buoyancy LEDs/sec^2
        float maxParticles;       // Fraction of numLeds (pool sized at begin() only)
        float burstSparks;        // × crossDim → sparks per burst
        // Fluid dynamics grid (v84)
        float gridCoolRate;       // Heat grid decay per frame (default 0.88)
        float buoyancyCoupling;   // Grid heat → upward force multiplier (default 1.0)
        float pressureCoupling;   // Lateral gradient → clustering multiplier (default 0.5)
        // Lifecycle
        uint8_t defaultLifespan;
        uint8_t intensityMin;
        uint8_t intensityMax;
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
        float splashParticles;    // × crossDim → particles per splash
        // Audio reactivity
        float organicTransientMin;
        // Background
        float backgroundIntensity;
        float maxParticles;       // Fraction of numLeds (clamped to pool)
        // Lifecycle
        uint8_t defaultLifespan;
        uint8_t intensityMin;
        uint8_t intensityMax;
        uint8_t splashIntensity;
    };

    struct StoredLightningParams {
        // Spawn behavior
        float baseSpawnChance;
        float audioSpawnBoost;
        // Branching
        float branchAngleSpread;
        // Audio reactivity
        float organicTransientMin;
        // Background
        float backgroundIntensity;
        float maxParticles;       // Fraction of numLeds (clamped to pool)
        // Lifecycle
        uint8_t defaultLifespan;
        uint8_t intensityMin;
        uint8_t intensityMax;
        uint8_t fadeRate;
        uint8_t branchChance;
        uint8_t branchCount;
        uint8_t branchIntensityLoss;
    };

    struct StoredMicParams {
        // Window/Range normalization parameters (v72: AGC removed, only these remain)
        float peakTau;            // Peak adaptation speed (attack time, seconds)
        float releaseTau;         // Peak release speed (release time, seconds)
    };

    // StoredMusicParams REMOVED v76.
    // Was 312 bytes of AudioController params (CBSS, Bayesian, octave checks, etc.).
    // AudioController replaced by AudioTracker in v74. All active params now in StoredTrackerParams.
    // See git history for full struct definition (v23-v75).

    // (StoredBandFluxParams removed v67 — BandFlux pipeline removed, FrameOnsetNN is sole ODF source)

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

        // Reserved: was FireDefaults (removed v71 — old heat-diffusion model, never read by particle Fire)
        // Keep same byte layout to avoid DEVICE_VERSION bump and device config wipe.
        uint8_t _reserved_fire[4];      // was baseCooling, sparkHeatMin, sparkHeatMax, bottomRowsForSparks
        float _reserved_fire2[2];       // was sparkChance, audioSparkBoost
        int8_t _reserved_fire3;         // was coolingAudioBias

        // Validity flag
        bool isValid;               // Is this config populated and ready to use?

        // Reserved for future expansion
        uint8_t reserved[8];

        // Total: ~160 bytes (see static_assert enforcing sizeof(StoredDeviceConfig) <= 160)
    };

    // AudioTracker params (v74: first persisted, previously serial-only)
    struct StoredTrackerParams {
        // Core tempo params
        float bpmMin;
        float bpmMax;
        float tempoSmoothing;
        uint16_t acfPeriodMs;

        // Rhythm activation
        float activationThreshold;

        // PLP (Predominant Local Pulse) — v79, replaces PLL
        float plpActivation;
        float plpConfAlpha;
        float plpNovGain;
        float plpSignalFloor;       // v81

        // Spectral flux contrast
        float odfContrast;

        // Pulse detection
        float pulseThresholdMult;
        float pulseMinLevel;
        float pulseOnsetFloor;
        float pulseNNGate;

        // (Percival ACF + comb filter bank removed v80)

        // ODF baseline tracking
        float baselineFastDrop;
        float baselineSlowRise;
        float odfPeakHoldDecay;

        // Energy synthesis
        float energyMicWeight;
        float energyMelWeight;
        float energyOdfWeight;

        // Spectral flux band weights
        float bassFluxWeight;
        float midFluxWeight;
        float highFluxWeight;

        // Pattern slot cache (v82, replaces v77 pattern memory)
        float slotSwitchThreshold;
        float slotNewThreshold;
        float slotUpdateRate;
        float slotSaveMinConf;
        float slotSeedBlend;
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
        // (StoredMusicParams removed v76 — 312 bytes of dead AudioController params)
        StoredTrackerParams tracker;
        // (StoredBandFluxParams bandflux removed v67)
        uint8_t brightness;
    };

    // Compile-time safety checks
    // These verify struct sizes match expected values to catch accidental changes
    // Note: Struct sizes depend on compiler padding. Sizes below are for ARM GCC.
    //
    // VERSION BUMPING RULES:
    // - StoredDeviceConfig changes -> bump DEVICE_VERSION (rare, wipes device identity)
    // - Any other struct changes -> bump SETTINGS_VERSION (preserves device config)
    static_assert(sizeof(StoredFireParams) == 60,
        "StoredFireParams size changed! Increment SETTINGS_VERSION and update assertion. (60 bytes = 14 floats + 3 uint8 + padding)");
    static_assert(sizeof(StoredWaterParams) == 64,
        "StoredWaterParams size changed! Increment SETTINGS_VERSION and update assertion. (64 bytes = 15 floats + 4 uint8 + padding)");
    static_assert(sizeof(StoredLightningParams) == 32,
        "StoredLightningParams size changed! Increment SETTINGS_VERSION and update assertion. (32 bytes = 6 floats + 7 uint8 + padding)");
    static_assert(sizeof(StoredMicParams) == 8,
        "StoredMicParams size changed! Increment SETTINGS_VERSION and update assertion. (8 bytes = 2 floats)");
    // (StoredMusicParams static_assert removed v76 — struct deleted)
    static_assert(sizeof(StoredTrackerParams) == 112,
        "StoredTrackerParams size changed! Increment SETTINGS_VERSION and update assertion. (112 bytes = 27 floats + 1 uint16 + padding)");
    // (StoredBandFluxParams static_assert removed v67 — struct removed)
    static_assert(sizeof(StoredDeviceConfig) <= 160,
        "StoredDeviceConfig size changed! Increment DEVICE_VERSION and update assertion. (Limit: 160 bytes)");
    // ConfigData: allocated in last 4KB flash page (4096 bytes available).
    // Tight bound catches accidental struct bloat. Raise when genuinely needed + bump SETTINGS_VERSION.
    static_assert(sizeof(ConfigData) <= 650,
        "ConfigData exceeds 650 bytes! Update this limit or reduce struct sizes. Flash page is 4096 bytes. (v76: ~312 bytes saved by removing StoredMusicParams)");

    ConfigStorage();
    void begin();
    void end();   // Close NVS handle — call before ESP.restart() on ESP32
    bool isValid() const { return valid_; }

    // Device configuration accessors (v28+)
    const StoredDeviceConfig& getDeviceConfig() const { return data_.device; }
    void setDeviceConfig(const StoredDeviceConfig& config) {
        data_.device = config;
        markDirty();
    }
    bool isDeviceConfigValid() const { return data_.device.isValid; }

    /**
     * Load/save all persisted generator, mic, and tracker parameters.
     * AudioTracker params persisted v74 (StoredTrackerParams).
     */
    void loadConfiguration(FireParams& fireParams, WaterParams& waterParams, LightningParams& lightningParams,
                          AdaptiveMic& mic, AudioTracker* tracker = nullptr);
    void saveConfiguration(const FireParams& fireParams, const WaterParams& waterParams, const LightningParams& lightningParams,
                          const AdaptiveMic& mic, AudioTracker* tracker = nullptr);
    void factoryReset();

    // Auto-save support
    void markDirty() { dirty_ = true; }
    void saveIfDirty(const FireParams& fireParams, const WaterParams& waterParams, const LightningParams& lightningParams,
                     const AdaptiveMic& mic, AudioTracker* tracker = nullptr);

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
