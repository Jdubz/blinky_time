#pragma once

#include "AudioControl.h"
#include "../hal/PlatformDetect.h"

#if defined(BLINKY_PLATFORM_NRF52840) || defined(BLINKY_PLATFORM_ESP32S3)
// TFLite Micro FrameBeatNN — always active since v68.
// nRF52840: uses CMSIS-NN optimized kernels (~0.2-1ms inference)
// ESP32-S3: uses reference kernels (~3-5ms inference, still within 16ms frame budget)
#include "FrameBeatNN.h"
#else
// Unknown platforms: FrameBeatNN stub.
// All methods are no-ops that report "not ready", so AudioController.cpp
// needs no platform guards — it falls back to energy-envelope ODF.
struct FrameBeatNN {
    static constexpr int INPUT_MEL_BANDS = 26;
    bool begin()                                    { return false; }
    bool isReady() const                            { return false; }
    bool isRhythmReady() const                      { return false; }
    bool hasDownbeatOutput() const                  { return false; }
    float inferOnset(const float*)                  { return 0.0f; }
    bool inferRhythm()                              { return false; }
    float getLastBeat() const                       { return 0.0f; }
    float getLastDownbeat() const                   { return 0.0f; }
    float getLastRhythmBeat() const                 { return 0.0f; }
    void setProfileEnabled(bool)                    {}
    void printDiagnostics() const                   {}
};
#endif
#include "SharedSpectralAnalysis.h"
#include "../inputs/AdaptiveMic.h"
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"

// ============================================================================
// Autocorrelation Peak (used for multi-peak extraction in runAutocorrelation)
// ============================================================================

struct AutocorrPeak {
    int lag;
    float correlation;
    float normCorrelation;
};

// ============================================================================
// Comb Filter Bank (Independent Tempo Validation)
// ============================================================================

/**
 * CombFilterBank - Independent tempo validation using parallel comb filter resonators
 *
 * Theory: A bank of comb filters at different tempos (60-198 BPM) provides
 * independent tempo validation without depending on autocorrelation being correct.
 * Each filter accumulates energy when the input has periodicity at its tempo.
 * The filter with maximum energy indicates the most likely tempo.
 *
 * Key improvements over single comb filter:
 * - Independent tempo detection (doesn't follow autocorrelation)
 * - Proper Scheirer (1998) equation: y[n] = (1-α)·x[n] + α·y[n-L]
 * - Complex phase extraction (not peak detection)
 * - Tempo prior weighting to reduce half-time/double-time confusion
 *
 * Memory: ~5.3 KB total (resonatorDelay_[20][66] = 5,280 bytes + state)
 * CPU: ~2% (20 filters × simple math, phase every 4 frames)
 */
class CombFilterBank {
public:
    static constexpr int MAX_LAG = 66;  // 60 BPM at 66 Hz (66*60/66)
    static constexpr int MIN_LAG = 20;  // 198 BPM at 66 Hz (66*60/20)
    // 20 filters linearly interpolated from MIN_LAG to MAX_LAG (60-198 BPM).
    // Proven at F1=0.519. 47 bins tested (v61, every integer lag) but A/B showed
    // no benefit (+12 KB RAM wasted). Reverted to 20.
    static constexpr int NUM_FILTERS = 20;

    // === TUNING PARAMETERS ===
    float feedbackGain = 0.92f;       // Resonance strength (0.85-0.98)
    // NOTE: No tempo prior - comb bank uses raw resonator energy
    // This provides independent tempo validation (autocorr applies prior separately)

    // === METHODS ===

    /**
     * Initialize the filter bank (compute filter lags/BPMs)
     */
    void init(float frameRate = 66.0f);

    /**
     * Reset all state (delay line, resonators, energy)
     */
    void reset();

    /**
     * Process one sample of onset strength
     * Updates all 20 resonators and finds peak tempo
     */
    void process(float input);

    /**
     * Get detected tempo in BPM
     */
    float getPeakBPM() const { return peakBPM_; }

    /**
     * Get confidence in tempo estimate (0-1)
     * Based on peak-to-mean energy ratio
     */
    float getPeakConfidence() const { return peakConfidence_; }

    /**
     * Get phase at detected tempo (0-1)
     * Extracted via complex exponential (Fourier method)
     */
    float getPhaseAtPeak() const { return peakPhase_; }

    /**
     * Get peak filter index (for debugging)
     */
    int getPeakFilterIndex() const { return peakFilterIdx_; }

    /**
     * Get resonator energy for a specific filter (for debugging)
     */
    float getFilterEnergy(int idx) const {
        if (idx >= 0 && idx < NUM_FILTERS) return resonatorEnergy_[idx];
        return 0.0f;
    }

    /**
     * Get BPM for a specific filter (for debugging)
     */
    float getFilterBPM(int idx) const {
        if (idx >= 0 && idx < NUM_FILTERS) return filterBPMs_[idx];
        return 0.0f;
    }

private:
    // Per-filter output delay lines for IIR feedback
    // Each filter stores its own output history so y[n-L] feeds back correctly.
    // Memory: 20 filters × 66 samples × 4 bytes = 5,280 bytes
    float resonatorDelay_[NUM_FILTERS][MAX_LAG] = {{0}};
    int writeIdx_ = 0;

    // Per-filter resonator state
    float resonatorOutput_[NUM_FILTERS] = {0};
    float resonatorEnergy_[NUM_FILTERS] = {0};  // Smoothed energy

    // Lag and BPM for each filter (pre-computed in init())
    int filterLags_[NUM_FILTERS] = {0};
    float filterBPMs_[NUM_FILTERS] = {0};

    // Results
    float peakBPM_ = 120.0f;
    float peakConfidence_ = 0.0f;
    float peakPhase_ = 0.0f;
    int peakFilterIdx_ = NUM_FILTERS / 2;  // Middle filter (approx 120 BPM)

    // Resonator history at peak filter for phase extraction
    float resonatorHistory_[MAX_LAG] = {0};
    int historyIdx_ = 0;

    // Frame counter for phase extraction throttling
    int frameCount_ = 0;

    // Frame rate for BPM calculations
    float frameRate_ = 60.0f;

    // Initialization flag
    bool initialized_ = false;

    /**
     * Extract phase using complex exponential (Fourier method)
     * Called every 4 frames to save CPU
     */
    void extractPhase();
};

// ============================================================================
// AudioController
// ============================================================================

/**
 * AudioController - Unified audio analysis and control signal synthesis
 *
 * Combines microphone input processing and rhythm analysis into a single
 * interface that outputs a simple 4-parameter AudioControl struct.
 *
 * Rhythm Tracking (CBSS beat tracking):
 *   1. Buffer up to 6 seconds of onset strength (spectral flux)
 *   2. Run autocorrelation to find periodicity and tempo (BPM)
 *      Progressive startup: begins after 1s (60 samples), full range after 2s
 *   3. Build CBSS (Cumulative Beat Strength Signal) from OSS + tempo
 *   4. Counter-based beat detection: expect beat at lastBeat + period
 *   5. Phase derived deterministically: (now - lastBeat) / period
 *
 * Key Design Decision:
 *   Phase is DERIVED from the beat counter, not accumulated with corrections.
 *   No drift, no jitter, no fighting correction systems.
 *   Transient detection drives VISUAL PULSE output only.
 *
 * Architecture:
 *   PDM Microphone
 *        |
 *   AdaptiveMic (level normalization, gain control)
 *        |
 *   SharedSpectralAnalysis (FFT → compressor → whitening → mel bands)
 *        |
 *   FrameBeatNN (frame-level FC, primary ODF) / mic level fallback
 *        |
 *   OSS Buffer (6s) --> Autocorrelation --> BPM(T)
 *        |                                    |
 *        +-----> CBSS Buffer ----> Beat Counter --> Phase = (now-lastBeat)/T
 *        |                                            |
 *   ODF-derived Pulse --> Pulse (visual only)         |
 *        |                                            |
 *   AudioControl { energy, pulse, phase, rhythmStrength }
 *        |
 *   Generators
 *
 * FRAME RATE ASSUMPTION:
 *   The OSS buffer and autocorrelation calculations assume ~60 Hz frame rate.
 *   - OSS_BUFFER_SIZE = 360 samples = 6 seconds @ 60 Hz
 *   - BPM-to-lag conversion: lag = 60 / bpm * 60 (assumes 60 Hz)
 *   If frame rate drops significantly (e.g., due to serial flooding or
 *   heavy LED updates), BPM estimates will drift. Phase tracking uses
 *   actual dt values, so phase remains accurate even with variable frame rate.
 *
 * Memory: ~6.5 KB (AdaptiveMic + 360-sample OSS buffer)
 * CPU: ~5-6% @ 64 MHz with FFT enabled
 */
class AudioController {
public:
    // === CONSTRUCTION ===
    AudioController(IPdmMic& pdm, ISystemTime& time);
    ~AudioController();

    // === LIFECYCLE ===
    bool begin(uint32_t sampleRate = 16000);
    void end();

    // === UPDATE (call every frame) ===
    /**
     * Update all audio analysis and return synthesized control signal.
     * Call this once per frame with delta time in seconds.
     */
    const AudioControl& update(float dt);

    // === OUTPUT ===
    /**
     * Get current control signal (read-only).
     * Valid after update() is called.
     */
    const AudioControl& getControl() const { return control_; }

    // === CONFIGURATION ===

    // BPM range constraints (set directly via bpmMin/bpmMax members + SettingsRegistry)
    float getBpmMin() const { return bpmMin; }
    float getBpmMax() const { return bpmMax; }

    // Get current BPM estimate (for debugging/display)
    float getCurrentBpm() const { return bpm_; }

    // Hardware gain control (for testing)
    void lockHwGain(int gain);
    void unlockHwGain();
    bool isHwGainLocked() const;
    int getHwGain() const;

    // === TUNING PARAMETERS ===
    // Exposed for SerialConsole tuning

    // Rhythm activation threshold (periodicity strength needed to activate)
    float activationThreshold = 0.4f;

    // Beat alignment pulse modulation (visual effect only)
    float pulseBoostOnBeat = 1.3f;      // Boost factor for on-beat transients
    float pulseSuppressOffBeat = 0.6f;  // Suppress factor for off-beat transients

    // Energy boost during rhythm lock
    float energyBoostOnBeat = 0.3f;     // Energy boost near predicted beats

    // (maxBpmChangePerSec removed — Bayesian fusion handles tempo stability)

    // Beat proximity thresholds for pulse modulation
    float pulseNearBeatThreshold = 0.2f;    // Phase distance < this = boost transients
    float pulseFarFromBeatThreshold = 0.3f; // Phase distance > this = suppress transients

    // BPM range for autocorrelation tempo detection
    float bpmMin = 60.0f;               // Minimum BPM
    float bpmMax = 200.0f;              // Maximum BPM

    // (Tempo prior params removed — replaced by bayesPriorCenter in Bayesian fusion)
    float tempoPriorWidth = 50.0f;      // Width (sigma) of Gaussian prior (BPM) — used by Bayesian static prior

    // === BEAT STABILITY TRACKING ===
    // Measures consistency of inter-beat intervals for confidence modulation
    float stabilityWindowBeats = 8.0f;  // Number of beats to track for stability

    // === BEAT LOOKAHEAD (anticipatory effects) ===
    // Predicts next beat time for zero-latency visual sync
    float beatLookaheadMs = 50.0f;      // How far ahead to predict beats (ms)

    // === CONTINUOUS TEMPO ESTIMATION ===
    // Kalman-like smoothing for gradual tempo changes
    float tempoSmoothingFactor = 0.85f; // Higher = smoother, slower adaptation (0-1)
    float tempoChangeThreshold = 0.1f;  // Min BPM change ratio to trigger update

    // === AUTOCORRELATION TIMING ===
    // Controls how often BPM is re-estimated via autocorrelation
    uint16_t autocorrPeriodMs = 250;  // Run autocorr every N ms (default 250ms for faster adaptation)

    // === COMB FILTER BANK (Independent Tempo Validation) ===
    // IIR resonator bank (Scheirer 1998): continuous real-time resonance at candidate
    // tempos. Provides phase-sensitive evidence that complements ACF (which is recomputed
    // every 250ms). BTrack uses only ACF with harmonic summation (no IIR bank), but our
    // comb bank may help with faster tempo lock. A/B testable via serial.
    bool combBankEnabled = true;    // Enable comb filter bank
    float combBankFeedback = 0.92f; // Bank resonance strength (0.85-0.98)

    // (combCrossValMinConf/MinCorr removed — comb bank feeds Bayesian fusion directly)

    // === CBSS BEAT TRACKING ===
    // Cumulative Beat Strength Signal with counter-based beat prediction
    // Phase is derived deterministically from beat counter — no drift, no jitter
    float cbssAlpha = 0.9f;              // CBSS weighting (0.8-0.95, higher = more predictive)
    float cbssTightness = 8.0f;           // Log-Gaussian tightness (higher=stricter tempo adherence, v40: raised from 5.0, +24% avg F1)
    float beatConfidenceDecay = 0.98f;   // Per-frame confidence decay when no beat detected
    float beatTimingOffset = 5.0f;       // Beat prediction advance in frames (compensates ODF+CBSS delay)
    float phaseCorrectionStrength = 0.0f; // Phase correction toward transients (0=off, 1=full snap) — disabled: hurts syncopated tracks
    float cbssThresholdFactor = 1.0f;    // CBSS adaptive threshold: beat fires only if CBSS > factor * cbssMean (0=off)
    float cbssContrast = 2.0f;           // Power-law ODF contrast before CBSS (BTrack-style squaring; A/B tested 10-6 win vs 1.0)
    uint8_t cbssWarmupBeats = 0;         // CBSS warmup: lower alpha for first N beats (0=disabled; tested 8, increased variance 5.5x)
    uint8_t onsetSnapWindow = 8;         // Snap beat anchor to strongest OSS in last N frames (0=disabled; default 8 frames)

    // === BEAT-BOUNDARY TEMPO UPDATES (Phase 2.1) ===
    // Defers beatPeriodSamples_ changes to the next beat fire, synchronizing
    // tempo and beat timing like BTrack. Prevents mid-beat period discontinuities.
    bool beatBoundaryTempo = true;       // Defer tempo changes to beat boundaries (BTrack-style)

    // === NN BEAT ACTIVATION ===
    // Frame-level FC beat/downbeat activation (sole ODF source for CBSS).
    bool nnProfile = false;              // Enable profiling output to Serial

    // === AUTOCORRELATION TUNING ===
    uint8_t odfSmoothWidth = 5;          // ODF smooth window size (3-11, odd). Affects CBSS delay and noise rejection

    // (ioiEnabled removed v52 — dead code since v28)

    // === ADAPTIVE ODF THRESHOLD (BTrack-style local mean + HWR) ===
    // Applies local-mean subtraction with half-wave rectification to the OSS
    // buffer before autocorrelation. Removes arrangement-level dynamics
    // (verse/chorus energy changes) so ACF sees cleaner periodicity peaks.
    // BTrack applies this twice; we apply once (compressor+whitening handle the rest).
    bool adaptiveOdfThresh = false;      // Enable local-mean ODF threshold before autocorrelation (off by default for A/B testing)
    uint8_t odfThreshWindow = 15;        // Half-window size for adaptive ODF threshold (samples each side, 5-30)

    // (onsetTrainOdf/odfDiffMode removed v67 — BandFlux pipeline removed, NN is sole ODF source)

    // === ODF MEAN SUBTRACTION (BTrack-style detrending) ===
    // Subtracts the local mean from OSS buffer before autocorrelation.
    // Removes DC bias that makes all lags appear somewhat correlated,
    // helping the true tempo peak stand out vs sub-harmonics.
    bool odfMeanSubEnabled = false;      // ODF mean subtraction (v32: disabled — raw ODF +70% F1)

    // === ONSET-DENSITY OCTAVE DISCRIMINATOR ===
    // Penalizes implausible tempos in the Bayesian posterior using onset density.
    // If BPM implies < densityMinPerBeat or > densityMaxPerBeat transients/beat,
    // applies a Gaussian penalty. Helps escape half-time lock on dance music.
    bool densityOctaveEnabled = true;    // Onset-density octave penalty (v32: enabled, +13% F1)
    float densityMinPerBeat = 0.5f;      // Min plausible transients per beat
    float densityMaxPerBeat = 5.0f;      // Max plausible transients per beat
    float densityPenaltyExp = 2.0f;      // Gaussian exponent for density penalty (higher = sharper cutoff)
    float densityTarget = 0.0f;          // Target transients/beat (0=disabled, >0=Gaussian centered here)

    // === SHADOW CBSS OCTAVE CHECKER ===
    // Every N beats, compares CBSS score at current tempo T vs double-time T/2.
    // If T/2 scores significantly better, switches to double-time.
    // Inspired by BeatNet's "tempo investigators" — provides escape from octave lock.
    bool downwardCorrectEnabled = false;  // Downward harmonic correction (experimental: overcorrects on mid-tempo, disabled by default)
    bool octaveCheckEnabled = true;      // Shadow CBSS octave check (v32: enabled, +13% F1)
    uint8_t octaveCheckBeats = 2;        // Check every N beats (v32: aggressive, was 4)
    float octaveScoreRatio = 1.3f;       // T/2 must score this much better to switch (v32: was 1.5)

    // (metricalCheckEnabled/metricalMinRatio/metricalCheckBeats removed v64 — no octave disambiguation benefit)
    // (phaseCheckEnabled removed v44 — net-negative on 18-track validation)
    // (plpPhaseEnabled/plpCorrectionStrength/plpMinConfidence removed v44 — zero effect, redundant with onset snap)

    // === BTRACK-STYLE TEMPO PIPELINE ===
    // Replaces multiplicative Bayesian fusion with BTrack's sequential pipeline:
    // Adaptive threshold on comb-on-ACF + Viterbi max-product.
    // Skips FT/IOI/IIR comb observations and post-hoc harmonic disambiguation.
    bool btrkPipeline = true;            // BTrack pipeline (v33: enabled, replaces multiplicative fusion)
    uint8_t btrkThreshWindow = 0;        // Adaptive threshold half-window (0=off, 1-5 bins each side)

    // (barPointerHmm/fwdPhaseOnly/hmmContrast/fwdObsLambda/fwdObsFloor/fwdWrapFraction removed v64 — HMM phase tracker never outperformed CBSS)
    // (particleFilterEnabled and all pf* params removed v64 — never outperformed CBSS)
    // (ftEnabled removed v52 — dead code since v28)

    // === BAYESIAN TEMPO FUSION ===
    // Fuses autocorrelation, Fourier tempogram, comb filter bank, and IOI histogram
    // into a unified posterior distribution over 20 tempo bins (~60-198 BPM).
    // Each signal provides an observation likelihood; the posterior = prior × Π(observations).
    float bayesLambda = 0.60f;           // Transition tightness (0.01=rigid, 1.0=loose)
    float bayesPriorCenter = 128.0f;     // Static prior center BPM (Gaussian)
    float bayesPriorWeight = 0.0f;       // Ongoing static prior strength (0=off, 1=standard, 2=strong)
    float bayesAcfWeight = 0.8f;         // Autocorrelation observation weight (high: harmonic comb makes ACF reliable)
    // (bayesFtWeight/bayesIoiWeight removed v52 — dead code since v28)
    float bayesCombWeight = 0.7f;        // Comb filter bank observation weight
    float posteriorFloor = 0.05f;        // Uniform mixing ratio to prevent mode lock (0=off, 0.05=5% floor)
    float disambigNudge = 0.15f;         // Posterior mass transfer when disambiguation corrects (0=off)
    float harmonicTransWeight = 0.30f;   // Transition matrix harmonic shortcut weight (0=off, 0.3=default)
    float rayleighBpm = 140.0f;          // Rayleigh prior peak BPM (v63: 120→140, fewer octave errors on fast tracks)
    bool fold32Enabled = false;           // 3:2 octave folding: fold evidence from 2L/3 into L (v44, OFF — no net benefit)
    bool sesquiCheckEnabled = false;      // 3:2 shadow octave check: test 3T/2 and 2T/3 alternatives (v44, OFF — no net benefit)
    bool bidirectionalSnap = true;        // Delay beat by 3 frames for bidirectional onset snap (v44)
    float tempoNudge = 0.8f;              // switchTempo posterior mass transfer fraction (0-1, v44)
    // (harmonicSesqui removed v44 — catastrophic regression on fast tracks)

    // === PERCIVAL ACF HARMONIC PRE-ENHANCEMENT (v45) ===
    // Folds 2nd and 4th harmonic ACF values into fundamental lag before comb-on-ACF.
    // Gives fundamental a unique advantage: double-time at L/2 does NOT get the same
    // boost because its harmonics (L, 3L/2) are at different positions.
    // Source: Percival & Tzanetakis 2014, Essentia percivalenhanceharmonics.cpp
    bool percivalEnhance = true;           // Enable harmonic pre-enhancement (v45)
    float percivalWeight2 = 0.5f;          // 2nd harmonic fold weight (v45)
    float percivalWeight4 = 0.25f;         // 4th harmonic fold weight (v45)
    float percivalWeight3 = 0.0f;          // 3rd harmonic SUBTRACT weight (v48, 0=off, 0.3=recommended for anti-harmonic)

    // === PLL PHASE CORRECTION (v45) ===
    // Proportional+integral phase correction applied at each beat fire.
    // Measures phase error between predicted and actual onset position,
    // nudges lastBeatSample_ to reduce drift over time.
    // Source: Kim 2007 (PLL-style proportional correction for beat tracking)
    bool pllEnabled = true;                // Enable PLL phase correction (v45)
    float pllKp = 0.15f;                   // Proportional gain (v45)
    float pllKi = 0.005f;                  // Integral gain (v45)
    // v65 params (persisted in StoredMusicParams since v70)
    uint8_t pllWarmupBeats = 5;            // Beats before tightening PLL clamp from ±T/2 to ±T/4 (v65)

    // === ONSET SNAP HYSTERESIS (v65) ===
    float snapHysteresis = 0.8f;           // Prefer previous snap if >this fraction of best (v65, 0=off)

    // === DOWNBEAT CALIBRATION (v65) ===
    float dbEmaAlpha = 0.3f;              // Downbeat EMA smoothing alpha (v65)
    float dbThreshold = 0.5f;             // Smoothed downbeat activation threshold to fire (v65)
    float dbDecay = 0.85f;                // Per-frame downbeat decay between beats (v65)

    // === ADAPTIVE CBSS TIGHTNESS (v45) ===
    // Modulates cbssTightness based on onset confidence (OSS/mean ratio).
    // Strong onsets → looser tightness (allow phase correction).
    // Weak onsets → tighter (resist noise-driven drift).
    // Source: BTrack adaptation for noisy microphone input
    bool adaptiveTightnessEnabled = true;  // Enable adaptive tightness (v45)
    float tightnessLowMult = 0.7f;         // Multiplier when onset confidence HIGH (v45)
    float tightnessHighMult = 1.3f;        // Multiplier when onset confidence LOW (v45)
    float tightnessConfThreshHigh = 3.0f;  // OSS/mean ratio above this = high confidence (v45)
    float tightnessConfThreshLow = 1.5f;   // OSS/mean ratio below this = low confidence (v45)

    // (forwardFilterEnabled and all fwd* params removed v64 — A/B tested: severe half-time bias, 17/18 octave errors)
    // (multiAgentEnabled/agentDecay/agentInitBeats removed v64 — never outperformed single CBSS)
    // (templateCheckEnabled/templateScoreRatio/templateCheckBeats removed v64 — A/B tested: baseline wins 10/18)
    // (subbeatCheckEnabled/alternationThresh/subbeatCheckBeats removed v64 — A/B tested: no net benefit)
    // (templateMinScore/subbeatBins/templateHistBars removed v64 — associated features removed)

    // === CALIBRATION CONSTANTS (v51, exposed for parameter sweeps) ===
    float cbssMeanAlpha = 0.008f;         // CBSS running mean EMA alpha (tau ~2s at 66Hz)
    float harmonic2xThresh = 0.5f;        // ACF ratio at half-lag for 2x BPM correction
    float harmonic15xThresh = 0.6f;       // ACF ratio at 2/3-lag for 1.5x BPM correction
    float pllSmoother = 0.95f;            // PLL phase integral leaky decay (0.8-0.99)
    float beatConfBoost = 0.15f;          // Confidence increment per beat fire (0.01-0.5)
    float rhythmBlend = 0.6f;             // Periodicity weight in rhythmStrength (1-x = CBSS)
    float periodicityBlend = 0.7f;        // Periodicity strength EMA coefficient
    float onsetDensityBlend = 0.7f;       // Onset density EMA coefficient

    // === ADVANCED ACCESS (for debugging/tuning only) ===

    AdaptiveMic& getMicForTuning() { return mic_; }
    const AdaptiveMic& getMic() const { return mic_; }

    SharedSpectralAnalysis& getSpectral() { return spectral_; }
    const SharedSpectralAnalysis& getSpectral() const { return spectral_; }

    const FrameBeatNN& getFrameBeatNN() const { return frameBeatNN_; }

    // Get last pulse strength for debugging/streaming
    float getLastPulseStrength() const { return lastPulseStrength_; }

    // Debug getters
    float getPeriodicityStrength() const { return periodicityStrength_; }
    float getBeatStability() const { return beatStability_; }
    float getTempoVelocity() const { return tempoVelocity_; }
    uint32_t getNextBeatMs() const { return nextBeatMs_; }
    // (getLastTempoPriorWeight removed — Bayesian fusion replaces tempo prior)

    // Bayesian tempo state debug getters
    int getBayesBestBin() const { return bayesBestBin_; }
    float getBayesBestConf() const;
    float getBayesCombObs() const;

    // Comb filter bank debug getters
    float getCombBankBPM() const { return combFilterBank_.getPeakBPM(); }
    float getCombBankConfidence() const { return combFilterBank_.getPeakConfidence(); }
    float getCombBankPhase() const { return combFilterBank_.getPhaseAtPeak(); }
    const CombFilterBank& getCombFilterBank() const { return combFilterBank_; }

    // Onset density debug getter
    float getOnsetDensity() const { return onsetDensity_; }

    // (lastFtMagRatio_ removed — Bayesian fusion exposes per-bin observations)

    // CBSS beat tracking debug getters
    float getCbssConfidence() const { return cbssConfidence_; }
    uint16_t getBeatCount() const { return beatCount_; }
    int getBeatPeriodSamples() const { return beatPeriodSamples_; }
    float getCurrentCBSS() const { return cbssBuffer_[(sampleCounter_ > 0 ? sampleCounter_ - 1 : 0) % OSS_BUFFER_SIZE]; }
    float getLastOnsetStrength() const { return lastSmoothedOnset_; }
    int getTimeToNextBeat() const { return timeToNextBeat_; }
    bool wasLastBeatPredicted() const { return lastFiredBeatPredicted_; }
    uint32_t getLastBeatTimeMs() const { return lastBeatMs_; }

    // (PLP phase getters removed v44 — feature removed)
    // (PF debug getters removed v64 — particle filter removed)

private:
    // === HAL REFERENCES ===
    ISystemTime& time_;

    // === MICROPHONE ===
    AdaptiveMic mic_;

    // === SPECTRAL ANALYSIS ===
    SharedSpectralAnalysis spectral_;

    // === PULSE DETECTION (derived from ODF) ===
    float lastPulseStrength_ = 0.0f;
    uint32_t lastPulseMs_ = 0;

    // Pulse detection parameters
    static constexpr float PULSE_MIN_LEVEL = 0.025f;    // Noise gate
    static constexpr float PULSE_THRESHOLD_MULT = 2.0f;  // ODF must exceed mean × this
    static constexpr uint16_t PULSE_MIN_COOLDOWN_MS = 40;
    static constexpr uint16_t PULSE_MAX_COOLDOWN_MS = 150;

    // === NN BEAT ACTIVATION ===
    FrameBeatNN frameBeatNN_;
    bool nnActive_ = false;      // Cached per-update: frameBeatNN_.isReady() (OnsetNN)
    bool rhythmActive_ = false;  // Cached per-update: frameBeatNN_.isRhythmReady()
    uint8_t rhythmFrameCounter_ = 0;  // Counts frames for RhythmNN scheduling (every 2nd)

    // === RHYTHM TRACKING STATE ===

    // Onset Strength Signal buffer (~5.5 seconds at 66 Hz frame rate)
    // Measured empirically: PDM 16kHz / FFT-256 = 62.5 theoretical, ~66 actual on nRF52840
    static constexpr int OSS_FRAME_RATE = 66;  // OSS samples per second (measured mic frame rate)
    static constexpr float OSS_FRAMES_PER_MIN = OSS_FRAME_RATE * 60.0f;  // 3960.0 — lag-to-BPM conversion
    static constexpr int OSS_BUFFER_SIZE = 360;
    float ossBuffer_[OSS_BUFFER_SIZE] = {0};
    uint32_t ossTimestamps_[OSS_BUFFER_SIZE] = {0};  // Track actual timestamps for adaptive lag
    int ossWriteIdx_ = 0;
    int ossCount_ = 0;

    // (prevMagnitudes_/maxFilteredPrevMags_ removed v64 — computeSpectralFluxBands removed)

    // Comb filter bank (independent tempo validation)
    CombFilterBank combFilterBank_;

    // Current tempo estimate
    float bpm_ = 120.0f;
    float beatPeriodMs_ = 500.0f;
    float periodicityStrength_ = 0.0f;

    // Phase tracking
    float phase_ = 0.0f;                // Current beat phase (0-1, derived from CBSS beat counter)

    // CBSS (Cumulative Beat Strength Signal) state
    float cbssBuffer_[OSS_BUFFER_SIZE] = {0};  // Same size as OSS buffer
    int lastBeatSample_ = 0;            // Sample index of last detected beat
    int beatPeriodSamples_ = 30;        // Beat period in samples (~120 BPM at 60Hz)
    int sampleCounter_ = 0;             // Total samples processed
    uint16_t beatCount_ = 0;            // Total beats detected
    float cbssConfidence_ = 0.0f;       // Beat tracking confidence (0-1)
    float cbssMean_ = 0.0f;             // Running EMA of CBSS values for adaptive threshold
                                         // Starts at 0 — threshold is intentionally inactive during
                                         // warmup (~2s) so LEDs respond immediately to music onset
    float lastSmoothedOnset_ = 0.0f;    // Last smoothed onset strength (for observability)
    bool lastBeatWasPredicted_ = false; // Whether predictBeat() ran since last beat (working flag)
    bool lastFiredBeatPredicted_ = false; // Whether the most recently fired beat was predicted (stable for streaming)
    int lastTransientSample_ = -1;      // Sample index of most recent strong transient (-1 = none)

    // ODF smoothing (causal moving average, runtime-tunable width)
    static constexpr int ODF_SMOOTH_MAX = 11;  // Max buffer size (allocated once)
    float odfSmoothBuffer_[ODF_SMOOTH_MAX] = {0};
    int odfSmoothIdx_ = 0;

    // Log-Gaussian transition weights (precomputed for current beat period)
    static constexpr int MAX_BEAT_PERIOD = 90; // ~40 BPM at 60Hz (covers full range)
    float logGaussianWeights_[MAX_BEAT_PERIOD * 2] = {0}; // Covers T/2 to 2T range
    int logGaussianWeightsSize_ = 0;
    int logGaussianLastT_ = 0;       // Last T used to compute weights
    float logGaussianLastTight_ = 0.0f;  // Last cbssTightness used (invalidate on change)

    // Predict+countdown state (BTrack-style)
    int timeToNextBeat_ = 15;          // Countdown frames until next beat
    int timeToNextPrediction_ = 10;    // Countdown frames until next prediction

    // Beat-boundary tempo update state (Phase 2.1)
    int pendingBeatPeriod_ = -1;       // Pending beat period (applied at next beat fire, -1=none)

    // Octave check state
    uint16_t beatsSinceOctaveCheck_ = 0; // Beats since last octave check
    // (beatsSinceMetricalCheck_/beatsSinceTemplateCheck_/beatsSinceSubbeatCheck_ removed v64)

    // Beat expectation Gaussian (precomputed for current beat period)
    float beatExpectationWindow_[MAX_BEAT_PERIOD] = {0};
    int beatExpectationSize_ = 0;
    int beatExpectationLastT_ = 0;

    // Beat stability tracking
    static constexpr int STABILITY_BUFFER_SIZE = 16;
    float interBeatIntervals_[STABILITY_BUFFER_SIZE] = {0};
    int ibiWriteIdx_ = 0;
    int ibiCount_ = 0;
    uint32_t lastBeatMs_ = 0;           // Timestamp of last detected beat
    float beatStability_ = 0.0f;        // Current stability (0-1, 1=perfectly stable)

    // Continuous tempo estimation
    float tempoVelocity_ = 0.0f;        // Rate of tempo change (BPM/second)
    float prevBpm_ = 120.0f;            // Previous BPM for velocity calculation

    // Beat lookahead
    uint32_t nextBeatMs_ = 0;           // Predicted timestamp of next beat

    // (lastTempoPriorWeight_ removed — Bayesian fusion replaces tempo prior)

    // Autocorrelation timing
    uint32_t lastAutocorrMs_ = 0;

    // Silence detection
    uint32_t lastSignificantAudioMs_ = 0;

    // Onset density tracking
    float onsetDensity_ = 0.0f;            // Smoothed onsets/second (EMA)
    uint16_t onsetCountInWindow_ = 0;      // Onsets detected in current 1s window
    uint32_t onsetDensityWindowStart_ = 0; // Window start timestamp (ms)

    // (IOI onset buffer removed v52 — dead code since v28)

    // === BAYESIAN TEMPO STATE ===
    // 20 bins matching CombFilterBank resolution (60-198 BPM, linearly interpolated lags)
    static constexpr int TEMPO_BINS = CombFilterBank::NUM_FILTERS;  // 20
    float tempoStatePrior_[TEMPO_BINS] = {0};     // Previous posterior (becomes prior)
    float tempoStatePost_[TEMPO_BINS] = {0};      // Current posterior after update
    float tempoStaticPrior_[TEMPO_BINS] = {0};    // Fixed Gaussian prior (ongoing pull toward bayesPriorCenter)
    float rayleighWeight_[TEMPO_BINS] = {0};      // Rayleigh tempo prior peaked at ~120 BPM (BTrack-style)
    float tempoBinBpms_[TEMPO_BINS] = {0};        // BPM value for each bin
    int tempoBinLags_[TEMPO_BINS] = {0};          // Lag value for each bin (at OSS_FRAME_RATE Hz)
    float transMatrix_[TEMPO_BINS][TEMPO_BINS] = {{0}};  // Precomputed Gaussian transition probabilities
    float transMatrixLambda_ = -1.0f;             // Last bayesLambda used to build transMatrix_
    float transMatrixHarmonic_ = -1.0f;          // Last harmonicTransWeight used to build transMatrix_
    bool transMatrixBtrkPipeline_ = false;       // Last btrkPipeline state used to build transMatrix_
    float rayleighBpm_ = -1.0f;                  // Last rayleighBpm used to compute rayleighWeight_
    bool tempoStateInitialized_ = false;
    int bayesBestBin_ = TEMPO_BINS / 2;              // Best bin from last fusion (for debug)
    float lastCombObs_[TEMPO_BINS] = {0};         // Last comb observations (for debug)

    // (Phase tracker state removed v64 — HMM/phase tracker never outperformed CBSS)
    // (Forward filter state removed v64 — ~5.5 KB: fwdAlpha_[880], fwdTransMatrix_[20][20], etc.)
    // (Particle filter state removed v64 — ~2.4 KB: 100 particles × 2 buffers + RNG state)

    // === PLL PHASE CORRECTION STATE (v45) ===
    float pllPhaseIntegral_ = 0.0f;  // PLL integral accumulator

    // === ONSET SNAP HYSTERESIS (v65) ===
    int lastSnapOffset_ = 0;         // Previous beat's snap offset (for hysteresis)
    uint8_t snapConsistencyCount_ = 0; // Consecutive beats at same snap offset

    // === DOWNBEAT TRACKING (v65) ===
    float downbeatSmoothed_ = 0.0f;  // EMA-smoothed NN downbeat activation
    uint8_t beatInMeasure_ = 0;      // Position in measure (1-4, 0=unknown)

    // === ADAPTIVE TIGHTNESS STATE (v45) ===
    float effectiveTightness_ = 8.0f;  // Current effective tightness (modulated by onset confidence)

    // (Multi-agent state removed v64 — never outperformed single CBSS)

    // === SYNTHESIZED OUTPUT ===
    AudioControl control_;

    // === INTERNAL METHODS ===

    // Rhythm tracking
    void addOssSample(float onsetStrength, uint32_t timestampMs);
    void runAutocorrelation(uint32_t nowMs);

    // Shared counter management
    void renormalizeCounters();  // Prevent sampleCounter_ overflow

    // CBSS beat tracking
    void updateCBSS(float onsetStrength);
    void detectBeat();
    void predictBeat();
    void checkOctaveAlternative();
    // (checkPhaseAlignment removed v44 — net-negative on 18-track validation)
    void switchTempo(int newPeriodSamples);

    // (Phase tracker/forward filter/multi-agent/particle filter declarations removed v64)

    // Pulse detection cooldown (tempo-adaptive, inlined from EnsembleFusion)
    uint16_t effectivePulseCooldownMs() const;

    // ODF smoothing
    float smoothOnsetStrength(float raw);

    // Log-Gaussian weight computation
    void recomputeLogGaussianWeights(int T);

    // (computeSpectralFluxBands removed v64 — legacy ODF path, unreachable since v54)

    // Bayesian tempo fusion
    // Note: initTempoState() uses bayesPriorCenter and tempoPriorWidth to build
    // the static prior. If these params change at runtime, call initTempoState()
    // again (or reboot) for the new values to take effect.
    void initTempoState();
    void buildTransitionMatrix();
    void runBayesianTempoFusion(float* correlationAtLag, int correlationSize,
                                int minLag, int maxLag, float avgEnergy,
                                float samplesPerMs, bool debugPrint,
                                int harmonicCorrelationSize);
    int findClosestTempoBin(float targetBpm) const;

    // Tempo prior and stability
    void updateBeatStability(uint32_t nowMs);
    void updateTempoVelocity(float newBpm, float dt);
    void predictNextBeat(uint32_t nowMs);

    // Output synthesis
    void synthesizeEnergy();
    void synthesizePulse();
    void synthesizePhase();
    void synthesizeRhythmStrength();
    void updateOnsetDensity(uint32_t nowMs);

    // (Old IOI/FT single-peak functions removed — replaced by per-bin observation functions)

    // Utilities
    inline float clampf(float val, float minVal, float maxVal) const {
        return val < minVal ? minVal : (val > maxVal ? maxVal : val);
    }
};
