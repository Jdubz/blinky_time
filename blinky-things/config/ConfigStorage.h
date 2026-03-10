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
    static const uint8_t SETTINGS_VERSION = 61;

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
        uint8_t hwGainMaxSignal;  // Max HW gain for AGC (0-80)
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
        float cbssContrast;             // Power-law ODF contrast before CBSS (v37: 1.0=linear, 2.0=BTrack-style square)
        uint8_t cbssWarmupBeats;        // CBSS warmup beats: lower alpha for first N beats (v37: 0=disabled)
        uint8_t onsetSnapWindow;       // Snap beat to strongest OSS in last N frames (v37: 4, 0=disabled)

        // Bayesian tempo fusion (v18)
        float bayesLambda;              // Transition tightness (0.01=rigid, 1.0=loose)
        float bayesPriorCenter;         // Static prior center BPM (Gaussian)
        float bayesPriorWeight;         // Ongoing static prior strength (0=off, 1=standard, 2=strong)
        float bayesAcfWeight;           // Autocorrelation observation weight
        // (bayesFtWeight/bayesIoiWeight removed v52 — dead code since v28)
        float bayesCombWeight;          // Comb filter bank observation weight
        float posteriorFloor;           // Posterior uniform floor (0=off, 0.05=5% mixing)
        float disambigNudge;            // Posterior nudge on disambiguation correction (0=off)
        float harmonicTransWeight;      // Transition matrix harmonic shortcut weight (0=off, 0.3=default)

        // Onset-density octave discriminator (v31)
        float densityMinPerBeat;        // Min plausible transients per beat (0.5)
        float densityMaxPerBeat;        // Max plausible transients per beat (5.0)
        float octaveScoreRatio;         // Shadow CBSS score ratio for octave switch (1.5)

        uint8_t odfSmoothWidth;         // ODF smooth window (3-11, odd)
        uint8_t octaveCheckBeats;       // Check octave every N beats (4)
        // (ioiEnabled/ftEnabled removed v52 — dead code since v28)
        bool odfMeanSubEnabled;         // Enable ODF mean subtraction before autocorrelation
        bool beatBoundaryTempo;         // Defer tempo changes to beat boundaries (v28)
        bool unifiedOdf;                // Use BandFlux pre-threshold as CBSS ODF (v28)
        bool adaptiveOdfThresh;         // Local-mean ODF threshold before autocorrelation (v31)
        uint8_t odfThreshWindow;        // Adaptive ODF threshold half-window (samples each side, 5-30)
        bool onsetTrainOdf;             // Binary onset-train ODF for ACF (v34)
        bool odfDiffMode;               // HWR first-difference ODF for ACF (v35)
        uint8_t odfSource;              // Alternative ODF source for ACF (v36: 0=default, 1=bass energy, 2=mic level, 3=bass flux, 4=centroid, 5=bass ratio)
        bool densityOctaveEnabled;      // Onset-density octave penalty (v31)
        float densityPenaltyExp;        // Density penalty Gaussian exponent (v35)
        float densityTarget;            // Target transients/beat (0=disabled, v35)
        bool downwardCorrectEnabled;    // Downward harmonic correction 3:2/2:1 (v41: disabled by default)
        bool octaveCheckEnabled;        // Shadow CBSS octave check (v31)
        // (phaseCheck fields removed v44 — net-negative)
        // (plpCorrectionStrength/plpMinConfidence removed v44 — zero effect)

        // 128 BPM gravity well fixes (v44)
        float rayleighBpm;              // Rayleigh prior peak BPM (60-180, default 120)
        float tempoNudge;               // switchTempo posterior mass transfer (0-1, default 0.8)

        // (plpPhaseEnabled removed v44 — zero effect)

        bool btrkPipeline;              // BTrack-style tempo pipeline (v33: Viterbi + comb-on-ACF)
        uint8_t btrkThreshWindow;       // Adaptive threshold half-window (0=off, 1-5)
        bool barPointerHmm;            // Phase tracker beat detection (v34, v52: single-tempo forward filter)
        float hmmContrast;             // Phase tracker ODF power-law contrast (v34)
        // (hmmTempoNorm removed v53 — only used by dead joint HMM)
        // (hmmLambda removed v53 — only used by dead joint HMM)
        float fwdObsLambda;            // Continuous ODF observation strength (v49: 2-32, default 8)
        float fwdObsFloor;             // Observation probability floor (v52: 0.001-0.1, default 0.01)
        float fwdWrapFraction;         // Wrap detection zone fraction (v52: 0.1-0.4, default 0.25)
        // (hmmBayesBias removed v53 — only used by dead joint HMM)

        // Particle filter beat tracking (v38)
        bool particleFilterEnabled;    // Enable PF (A/B vs CBSS+Bayesian)
        float pfNoise;                 // Period diffusion noise (fraction of period/frame)
        float pfBeatSigma;             // Beat kernel width (fraction of period)
        float pfOctaveInjectRatio;     // Fraction of particles replaced with octave variants
        float pfBeatThreshold;         // Weighted fraction near phase=0 to trigger beat
        float pfNeffRatio;             // Resample when Neff < ratio * N
        float pfContrast;              // ODF power-law contrast for PF likelihood
        float pfInfoGate;              // Information gate: floor ODF below this to 0.03 (v39, 0=off)
        uint8_t pfObsLambda;           // Observation lambda: beat region = 1/lambda of period (v39, 2-32)

        // Spectral processing (v23+)
        bool whitenEnabled;             // Per-bin spectral whitening
        bool compressorEnabled;         // Soft-knee compressor
        bool whitenBassBypass;          // Skip whitening for bass bins 1-6 (v47)
        float whitenDecay;              // Peak decay per frame (~5s at 0.997)
        float whitenFloor;              // Noise floor for whitening
        float compThresholdDb;          // Compressor threshold (dB)
        float compRatio;                // Compression ratio (e.g., 3:1)
        float compKneeDb;              // Soft knee width (dB)
        float compMakeupDb;            // Makeup gain (dB)
        float compAttackTau;           // Attack time constant (seconds)
        float compReleaseTau;          // Release time constant (seconds)

        // 128 BPM gravity well fixes (v44)
        bool fold32Enabled;             // 3:2 octave folding (v44)
        bool sesquiCheckEnabled;        // 3:2 shadow octave check (v44)
        bool bidirectionalSnap;         // Bidirectional onset snap (v44)
        // (harmonicSesqui removed v44 — catastrophic on fast tracks)

        // Percival ACF harmonic pre-enhancement (v45)
        float percivalWeight2;          // 2nd harmonic fold weight (0-1, default 0.5)
        float percivalWeight4;          // 4th harmonic fold weight (0-1, default 0.25)
        bool percivalEnhance;           // Enable harmonic pre-enhancement

        // PLL phase correction (v45)
        float pllKp;                    // Proportional gain (0-1, default 0.15)
        float pllKi;                    // Integral gain (0-0.1, default 0.005)
        bool pllEnabled;                // Enable PLL phase correction

        // Adaptive CBSS tightness (v45)
        float tightnessLowMult;         // Multiplier when onset confidence HIGH (0.3-1.0)
        float tightnessHighMult;        // Multiplier when onset confidence LOW (1.0-3.0)
        float tightnessConfThreshHigh;  // OSS/mean ratio for high confidence (1.5-10)
        float tightnessConfThreshLow;   // OSS/mean ratio for low confidence (0.5-3.0)
        bool adaptiveTightnessEnabled;  // Enable adaptive tightness

        // Anti-harmonic 3rd comb (v48)
        float percivalWeight3;          // 3rd harmonic SUBTRACT weight (0-1, default 0)

        // Multi-agent beat tracking (v48)
        float agentDecay;               // Agent score EMA decay (0.7-0.95)
        float metricalMinRatio;         // Min beat/midpoint strength ratio (1.0-5.0)
        bool multiAgentEnabled;         // Enable multi-agent phase competition
        bool metricalCheckEnabled;      // Enable metrical contrast check
        uint8_t agentInitBeats;         // Initialize agents after N beats (2-8)
        uint8_t metricalCheckBeats;     // Check every N beats (2-8)

        // Rhythmic pattern templates (v50)
        float templateScoreRatio;       // Min score ratio to switch tempo (1.0-3.0)
        bool templateCheckEnabled;      // Enable template-based octave check
        uint8_t templateCheckBeats;     // Check every N beats (2-8)

        // Beat critic subbeat alternation (v50)
        float alternationThresh;        // Odd/even ratio threshold (0.3-3.0)
        bool subbeatCheckEnabled;       // Enable subbeat alternation check
        uint8_t subbeatCheckBeats;      // Check every N beats (2-8)

        // Hidden constants exposed (v51)
        float templateMinScore;         // Min Pearson correlation to consider tempo switch
        float cbssMeanAlpha;            // CBSS running mean EMA alpha
        float harmonic2xThresh;         // ACF half-lag ratio for 2x BPM correction
        float harmonic15xThresh;        // ACF 2/3-lag ratio for 1.5x BPM correction
        float pllSmoother;              // PLL phase integral leaky decay
        float beatConfBoost;            // Confidence increment per beat fire
        float rhythmBlend;              // Periodicity weight in rhythmStrength
        float periodicityBlend;         // Periodicity strength EMA coefficient
        float onsetDensityBlend;        // Onset density EMA coefficient
        uint8_t subbeatBins;            // Subbeat bin resolution (even, 4-16)
        uint8_t templateHistBars;       // Template history depth in bars (1-4)

        // NN beat activation (v54)
        bool nnBeatActivation;          // Use NN ODF instead of BandFlux (requires ENABLE_NN_BEAT_ACTIVATION)

        // Spectral noise estimation (v56)
        bool noiseEstEnabled;           // Enable minimum statistics noise subtraction
        float noiseSmoothAlpha;         // Power smoothing coefficient (0.9-0.99)
        float noiseReleaseFactor;       // Noise floor release rate (0.99-0.9999)
        float noiseOversubtract;        // Oversubtraction factor (1.0-3.0)
        float noiseFloorRatio;          // Spectral floor as fraction of original (0.001-0.5)

        // Joint forward filter (v57)
        bool forwardFilterEnabled;      // Enable joint tempo-phase forward filter
        float fwdTransSigma;            // Tempo transition width in lag units (1-10)
        float fwdFilterContrast;        // ODF power-law contrast (1-8)
        float fwdFilterLambda;          // Beat zone = 1/lambda of period (4-32)
        float fwdFilterFloor;           // Observation probability floor (0.001-0.1)
        float fwdBayesBias;            // Bayesian posterior modulation strength (v59, 0-1)
        float fwdAsymmetry;            // Asymmetric non-beat penalty by tempo (v60, 0-5)

        // Hybrid phase tracker (v58)
        bool fwdPhaseOnly;             // Phase tracker for phase, CBSS for beats
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
        bool usePreWhitenMags;      // Use raw (pre-compressor, pre-whitening) magnitudes (v47)
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
        "StoredMicParams size changed! Increment SETTINGS_VERSION and update assertion. (24 bytes = 5 floats + 1 uint16 + 1 bool + 1 uint8)");
    static_assert(sizeof(StoredMusicParams) == 408,
        "StoredMusicParams size changed! Increment SETTINGS_VERSION and update assertion. (408 bytes = 84 floats + 14 uint8 + 29 bools + padding)");
    static_assert(sizeof(StoredBandFluxParams) == 44,
        "StoredBandFluxParams size changed! Increment SETTINGS_VERSION and update assertion. (44 bytes = 9 floats + 3 uint8 + 4 bools + padding)");
    static_assert(sizeof(StoredDeviceConfig) <= 160,
        "StoredDeviceConfig size changed! Increment DEVICE_VERSION and update assertion. (Limit: 160 bytes)");
    // ConfigData: allocated in last 4KB flash page (4096 bytes available).
    // Tight bound catches accidental struct bloat. Raise when genuinely needed + bump SETTINGS_VERSION.
    static_assert(sizeof(ConfigData) <= 800,
        "ConfigData exceeds 800 bytes! Update this limit or reduce struct sizes. Flash page is 4096 bytes.");

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
