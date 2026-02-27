#pragma once

#include "AudioControl.h"
#include "EnsembleDetector.h"
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
 * Theory: A bank of comb filters at different tempos (60-180 BPM) provides
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
 * Memory: ~5.4 KB total (4.8 KB per-filter delay lines + 0.6 KB state)
 * CPU: ~2% (20 filters × simple math, phase every 4 frames)
 */
class CombFilterBank {
public:
    // 20 filters: 60-180 BPM at ~6 BPM resolution
    // At 60 Hz: lag range = 20-60 samples
    static constexpr int NUM_FILTERS = 20;
    static constexpr int MAX_LAG = 60;  // 60 BPM at 60 Hz
    static constexpr int MIN_LAG = 20;  // 180 BPM at 60 Hz

    // === TUNING PARAMETERS ===
    float feedbackGain = 0.92f;       // Resonance strength (0.85-0.98)
    // NOTE: No tempo prior - comb bank uses raw resonator energy
    // This provides independent tempo validation (autocorr applies prior separately)

    // === METHODS ===

    /**
     * Initialize the filter bank (compute filter lags/BPMs)
     */
    void init(float frameRate = 60.0f);

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
    // Memory: 20 filters × 60 samples × 4 bytes = 4800 bytes
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
    int peakFilterIdx_ = 10;  // Middle filter (approx 120 BPM)

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
 *   EnsembleDetector (detectors + fusion)
 *        |
 *   OSS Buffer (6s) --> Autocorrelation --> BPM(T)
 *        |                                    |
 *        +-----> CBSS Buffer ----> Beat Counter --> Phase = (now-lastBeat)/T
 *        |                                            |
 *   Ensemble Transient --> Pulse (visual only)        |
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

    // Ensemble detector configuration
    void setDetectorEnabled(DetectorType type, bool enabled);
    void setDetectorWeight(DetectorType type, float weight);
    void setDetectorThreshold(DetectorType type, float threshold);

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
    float bpmMin = 60.0f;               // Minimum BPM to detect (affects autocorr lag range)
    float bpmMax = 200.0f;              // Maximum BPM to detect (affects autocorr lag range)

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

    // === ADAPTIVE BAND WEIGHTING ===
    // Dynamically adjusts band weights based on which frequency bands show strongest periodicity
    // When enabled, bands with stronger rhythmic content get higher weights
    // Uses SuperFlux-style max filtering, cross-band correlation, and peakiness detection
    // to distinguish real beats from vibrato/tremolo in sustained content
    bool adaptiveBandWeightEnabled = true;  // Enable/disable adaptive weighting
    float bassBandWeight = 0.5f;    // Bass band weight (when adaptive disabled)
    float midBandWeight = 0.3f;     // Mid band weight (when adaptive disabled)
    float highBandWeight = 0.2f;    // High band weight (when adaptive disabled)

    // === AUTOCORRELATION TIMING ===
    // Controls how often BPM is re-estimated via autocorrelation
    uint16_t autocorrPeriodMs = 250;  // Run autocorr every N ms (default 250ms for faster adaptation)

    // === COMB FILTER BANK (Independent Tempo Validation) ===
    // Bank of 20 comb filters at 60-180 BPM for independent tempo detection
    bool combBankEnabled = true;    // Enable comb filter bank
    float combBankFeedback = 0.92f; // Bank resonance strength (0.85-0.98)

    // (combCrossValMinConf/MinCorr removed — comb bank feeds Bayesian fusion directly)

    // === CBSS BEAT TRACKING ===
    // Cumulative Beat Strength Signal with counter-based beat prediction
    // Phase is derived deterministically from beat counter — no drift, no jitter
    float cbssAlpha = 0.9f;              // CBSS weighting (0.8-0.95, higher = more predictive)
    float cbssTightness = 5.0f;           // Log-Gaussian tightness (higher=stricter tempo adherence)
    float beatConfidenceDecay = 0.98f;   // Per-frame confidence decay when no beat detected
    float beatTimingOffset = 5.0f;       // Beat prediction advance in frames (compensates ODF+CBSS delay)
    float phaseCorrectionStrength = 0.0f; // Phase correction toward transients (0=off, 1=full snap) — disabled: hurts syncopated tracks
    float cbssThresholdFactor = 1.0f;    // CBSS adaptive threshold: beat fires only if CBSS > factor * cbssMean (0=off)

    // === AUTOCORRELATION TUNING ===
    uint8_t odfSmoothWidth = 5;          // ODF smooth window size (3-11, odd). Affects CBSS delay and noise rejection

    // === IOI HISTOGRAM (enables per-bin observation in Bayesian fusion) ===
    bool ioiEnabled = true;              // Enable IOI histogram observation

    // === ODF MEAN SUBTRACTION (BTrack-style detrending) ===
    // Subtracts the local mean from OSS buffer before autocorrelation.
    // Removes DC bias that makes all lags appear somewhat correlated,
    // helping the true tempo peak stand out vs sub-harmonics.
    bool odfMeanSubEnabled = true;       // Enable ODF mean subtraction before autocorrelation

    // === FOURIER TEMPOGRAM (enables per-bin observation in Bayesian fusion) ===
    bool ftEnabled = true;               // Enable Fourier tempogram observation

    // === BAYESIAN TEMPO FUSION ===
    // Fuses autocorrelation, Fourier tempogram, comb filter bank, and IOI histogram
    // into a unified posterior distribution over 20 tempo bins (60-180 BPM).
    // Each signal provides an observation likelihood; the posterior = prior × Π(observations).
    float bayesLambda = 0.07f;           // Transition tightness (0.01=rigid, 1.0=loose)
    float bayesPriorCenter = 128.0f;     // Static prior center BPM (Gaussian)
    float bayesPriorWeight = 0.0f;       // Ongoing static prior strength (0=off, 1=standard, 2=strong)
    float bayesAcfWeight = 0.8f;         // Autocorrelation observation weight (high: harmonic comb makes ACF reliable)
    float bayesFtWeight = 2.0f;          // Fourier tempogram observation weight (re-enabled by spectral processing v24)
    float bayesCombWeight = 0.7f;        // Comb filter bank observation weight
    float bayesIoiWeight = 2.0f;         // IOI histogram observation weight (re-enabled by spectral processing v24)

    // === ADVANCED ACCESS (for debugging/tuning only) ===

    AdaptiveMic& getMicForTuning() { return mic_; }
    const AdaptiveMic& getMic() const { return mic_; }

    EnsembleDetector& getEnsemble() { return ensemble_; }
    const EnsembleDetector& getEnsemble() const { return ensemble_; }

    // Get last ensemble output for debugging
    const EnsembleOutput& getLastEnsembleOutput() const { return ensemble_.getLastOutput(); }

    // Debug getters
    float getPeriodicityStrength() const { return periodicityStrength_; }
    float getBeatStability() const { return beatStability_; }
    float getTempoVelocity() const { return tempoVelocity_; }
    uint32_t getNextBeatMs() const { return nextBeatMs_; }
    // (getLastTempoPriorWeight removed — Bayesian fusion replaces tempo prior)

    // Adaptive band weight debug getters
    const float* getAdaptiveBandWeights() const { return adaptiveBandWeights_; }
    const float* getBandPeriodicityStrength() const { return bandPeriodicityStrength_; }

    // Bayesian tempo state debug getters
    int getBayesBestBin() const { return bayesBestBin_; }
    float getBayesBestConf() const;
    float getBayesFtObs() const;
    float getBayesCombObs() const;
    float getBayesIoiObs() const;

    // Comb filter bank debug getters
    float getCombBankBPM() const { return combFilterBank_.getPeakBPM(); }
    float getCombBankConfidence() const { return combFilterBank_.getPeakConfidence(); }
    float getCombBankPhase() const { return combFilterBank_.getPhaseAtPeak(); }
    const CombFilterBank& getCombFilterBank() const { return combFilterBank_; }

    // Onset density debug getter
    float getOnsetDensity() const { return onsetDensity_; }

    // IOI histogram debug getter
    int getIOIOnsetCount() const { return ioiOnsetCount_; }

    // (lastFtMagRatio_ removed — Bayesian fusion exposes per-bin observations)

    // CBSS beat tracking debug getters
    float getCbssConfidence() const { return cbssConfidence_; }
    uint16_t getBeatCount() const { return beatCount_; }
    int getBeatPeriodSamples() const { return beatPeriodSamples_; }
    float getCurrentCBSS() const { return cbssBuffer_[(sampleCounter_ > 0 ? sampleCounter_ - 1 : 0) % OSS_BUFFER_SIZE]; }
    float getLastOnsetStrength() const { return lastSmoothedOnset_; }
    int getTimeToNextBeat() const { return timeToNextBeat_; }
    bool wasLastBeatPredicted() const { return lastFiredBeatPredicted_; }

private:
    // === HAL REFERENCES ===
    ISystemTime& time_;

    // === MICROPHONE ===
    AdaptiveMic mic_;

    // === ENSEMBLE DETECTOR ===
    EnsembleDetector ensemble_;
    EnsembleOutput lastEnsembleOutput_;

    // === RHYTHM TRACKING STATE ===

    // Onset Strength Signal buffer (6 seconds at 60 Hz frame rate)
    static constexpr int OSS_BUFFER_SIZE = 360;
    float ossBuffer_[OSS_BUFFER_SIZE] = {0};
    uint32_t ossTimestamps_[OSS_BUFFER_SIZE] = {0};  // Track actual timestamps for adaptive lag
    int ossWriteIdx_ = 0;
    int ossCount_ = 0;

    // Spectral flux: previous frame magnitudes for frame-to-frame comparison
    static constexpr int SPECTRAL_BINS = 128;  // FFT_SIZE / 2
    float prevMagnitudes_[SPECTRAL_BINS] = {0};
    bool prevMagnitudesValid_ = false;  // First frame has no previous

    // Per-band OSS tracking for adaptive weighting
    // Tracks periodicity strength in bass, mid, high bands independently
    static constexpr int BAND_COUNT = 3;  // bass, mid, high
    static constexpr int BAND_OSS_BUFFER_SIZE = 240;  // 4 seconds at 60 Hz (captures 4+ beats at 60 BPM)
    float bandOssBuffers_[BAND_COUNT][BAND_OSS_BUFFER_SIZE] = {{0}};
    int bandOssWriteIdx_ = 0;
    int bandOssCount_ = 0;
    float bandPeriodicityStrength_[BAND_COUNT] = {0};
    float adaptiveBandWeights_[BAND_COUNT] = {0.5f, 0.3f, 0.2f};  // Default weights
    uint32_t lastBandAutocorrMs_ = 0;
    // Cross-band correlation tracking (SuperFlux-inspired)
    // Measures how synchronized the bands are - real beats correlate across bands
    float crossBandCorrelation_[BAND_COUNT] = {0};  // Correlation of each band with others
    float bandSynchrony_ = 0.0f;  // Overall synchrony metric (0-1)

    // Peakiness tracking - distinguishes transient bursts from continuous vibrato
    // Transients: sparse, high peaks (high peakiness)
    // Vibrato: continuous, low-level fluctuations (low peakiness)
    float bandPeakiness_[BAND_COUNT] = {0};

    // Maximum-filtered previous magnitudes for vibrato suppression (SuperFlux style)
    float maxFilteredPrevMags_[SPECTRAL_BINS] = {0};

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

    // IOI histogram onset ring buffer (records sample indices of detected onsets)
    static constexpr int IOI_ONSET_BUFFER_SIZE = 48;
    int ioiOnsetSamples_[IOI_ONSET_BUFFER_SIZE] = {0};
    int ioiOnsetWriteIdx_ = 0;
    int ioiOnsetCount_ = 0;

    // === BAYESIAN TEMPO STATE ===
    // 20 bins matching CombFilterBank resolution (60-180 BPM)
    static constexpr int TEMPO_BINS = CombFilterBank::NUM_FILTERS;  // 20
    float tempoStatePrior_[TEMPO_BINS] = {0};     // Previous posterior (becomes prior)
    float tempoStatePost_[TEMPO_BINS] = {0};      // Current posterior after update
    float tempoStaticPrior_[TEMPO_BINS] = {0};    // Fixed Gaussian prior (ongoing pull toward bayesPriorCenter)
    float rayleighWeight_[TEMPO_BINS] = {0};      // Rayleigh tempo prior peaked at ~120 BPM (BTrack-style)
    float tempoBinBpms_[TEMPO_BINS] = {0};        // BPM value for each bin
    int tempoBinLags_[TEMPO_BINS] = {0};          // Lag value for each bin (at ~60 Hz)
    float transMatrix_[TEMPO_BINS][TEMPO_BINS] = {{0}};  // Precomputed Gaussian transition probabilities
    float transMatrixLambda_ = -1.0f;             // bayesLambda used to compute transMatrix_ (-1 = uninitialized)
    bool tempoStateInitialized_ = false;
    int bayesBestBin_ = TEMPO_BINS / 2;              // Best bin from last fusion (for debug)
    float lastFtObs_[TEMPO_BINS] = {0};           // Last FT observations (for debug)
    float lastCombObs_[TEMPO_BINS] = {0};         // Last comb observations (for debug)
    float lastIoiObs_[TEMPO_BINS] = {0};          // Last IOI observations (for debug)

    // === SYNTHESIZED OUTPUT ===
    AudioControl control_;

    // === INTERNAL METHODS ===

    // Rhythm tracking
    void addOssSample(float onsetStrength, uint32_t timestampMs);
    void runAutocorrelation(uint32_t nowMs);

    // CBSS beat tracking
    void updateCBSS(float onsetStrength);
    void detectBeat();
    void predictBeat();

    // ODF smoothing
    float smoothOnsetStrength(float raw);

    // Log-Gaussian weight computation
    void recomputeLogGaussianWeights(int T);

    // Onset strength computation
    float computeSpectralFluxBands(const float* magnitudes, int numBins,
                                    float& bassFlux, float& midFlux, float& highFlux);

    // Adaptive band weighting
    void addBandOssSamples(float bassFlux, float midFlux, float highFlux);
    void updateBandPeriodicities(uint32_t nowMs);
    float computeBandAutocorrelation(int band);
    void computeCrossBandCorrelation();
    void computeBandPeakiness();

    // Bayesian tempo fusion
    // Note: initTempoState() uses bayesPriorCenter and tempoPriorWidth to build
    // the static prior. If these params change at runtime, call initTempoState()
    // again (or reboot) for the new values to take effect.
    void initTempoState();
    void runBayesianTempoFusion(float* correlationAtLag, int correlationSize,
                                int minLag, int maxLag, float avgEnergy,
                                float samplesPerMs, bool debugPrint,
                                int harmonicCorrelationSize);
    void computeFTObservations(float* ftObs, int numBins);
    void computeIOIObservations(float* ioiObs, int numBins);
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
