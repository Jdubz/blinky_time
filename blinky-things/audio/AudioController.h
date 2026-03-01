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
 * Memory: ~10.2 KB total (9.6 KB per-filter delay lines + 0.6 KB state)
 * CPU: ~3% (40 filters × simple math, phase every 4 frames)
 */
class CombFilterBank {
public:
    // 20 filters: 60-200 BPM (~7 BPM resolution, broad musical tempo coverage)
    // At 60 Hz: lag range = 18-60 samples
    // 40 bins created systematic posterior drift toward low BPM due to
    // non-uniform BPM spacing on the lag-uniform grid (more bins per BPM
    // at low tempos → probability accumulation). 20 bins proven at F1=0.519.
    static constexpr int NUM_FILTERS = 20;
    static constexpr int MAX_LAG = 60;  // 60 BPM at 60 Hz
    static constexpr int MIN_LAG = 18;  // 200 BPM at 60 Hz

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
     * Updates all 40 resonators and finds peak tempo
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
    // Memory: 40 filters × 60 samples × 4 bytes = 9600 bytes
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
    float cbssTightness = 5.0f;           // Log-Gaussian tightness (higher=stricter tempo adherence)
    float beatConfidenceDecay = 0.98f;   // Per-frame confidence decay when no beat detected
    float beatTimingOffset = 5.0f;       // Beat prediction advance in frames (compensates ODF+CBSS delay)
    float phaseCorrectionStrength = 0.0f; // Phase correction toward transients (0=off, 1=full snap) — disabled: hurts syncopated tracks
    float cbssThresholdFactor = 1.0f;    // CBSS adaptive threshold: beat fires only if CBSS > factor * cbssMean (0=off)
    float cbssContrast = 1.0f;           // Power-law ODF contrast before CBSS (BTrack uses 2.0; higher = sharper beat peaks)
    uint8_t cbssWarmupBeats = 0;         // CBSS warmup: lower alpha for first N beats (0=disabled; tested 8, increased variance 5.5x)
    uint8_t onsetSnapWindow = 4;         // Snap beat anchor to strongest OSS in last N frames (0=disabled, 4≈67ms at 60Hz)

    // === BEAT-BOUNDARY TEMPO UPDATES (Phase 2.1) ===
    // Defers beatPeriodSamples_ changes to the next beat fire, synchronizing
    // tempo and beat timing like BTrack. Prevents mid-beat period discontinuities.
    bool beatBoundaryTempo = true;       // Defer tempo changes to beat boundaries (BTrack-style)

    // === UNIFIED ODF (Phase 2.4) ===
    // Uses BandFlux pre-threshold continuous activation as the ODF for CBSS beat tracking,
    // instead of the separate computeSpectralFluxBands(). Ensures transient detection and
    // beat tracking see the same signal. BTrack uses a single ODF for both.
    bool unifiedOdf = true;              // Use BandFlux pre-threshold as CBSS ODF (BTrack-style)

    // === AUTOCORRELATION TUNING ===
    uint8_t odfSmoothWidth = 5;          // ODF smooth window size (3-11, odd). Affects CBSS delay and noise rejection

    // === IOI HISTOGRAM (enables per-bin observation in Bayesian fusion) ===
    bool ioiEnabled = false;             // IOI histogram observation (disabled: O(n²), unnormalized counts)

    // === ADAPTIVE ODF THRESHOLD (BTrack-style local mean + HWR) ===
    // Applies local-mean subtraction with half-wave rectification to the OSS
    // buffer before autocorrelation. Removes arrangement-level dynamics
    // (verse/chorus energy changes) so ACF sees cleaner periodicity peaks.
    // BTrack applies this twice; we apply once (compressor+whitening handle the rest).
    bool adaptiveOdfThresh = false;      // Enable local-mean ODF threshold before autocorrelation (off by default for A/B testing)
    uint8_t odfThreshWindow = 15;        // Half-window size for adaptive ODF threshold (samples each side, 5-30)

    // === ONSET-TRAIN ODF (binary onset events for ACF) ===
    // Feeds post-threshold transient events to OSS buffer instead of continuous flux.
    // ACF of onset events finds inter-onset periodicity (immune to enclosure resonance).
    // CBSS and comb bank remain on continuous ODF.
    bool onsetTrainOdf = false;          // Binary onset-train ODF for ACF (off by default for A/B testing)

    // === HWR FIRST-DIFFERENCE ODF ===
    // Feeds max(0, odf[n] - odf[n-1]) to OSS buffer instead of raw odf[n].
    // Emphasizes onset ATTACKS (~30x larger than continuous modulation), suppressing
    // enclosure-induced periodic fluctuations. BTrack uses this approach.
    bool odfDiffMode = false;            // HWR first-difference ODF for ACF (off by default)

    // === ALTERNATIVE ODF SOURCE FOR ACF ===
    // Selects which signal feeds the OSS buffer (and thus ACF tempo estimation).
    // CBSS and comb bank always use the smoothed onset strength (BandFlux).
    // Options:
    //   0: Default (smoothed BandFlux combined flux, same as CBSS)
    //   1: Bass energy (sum of whitened bass mags bins 1-6, 62.5-375 Hz)
    //   2: Mic level (broadband time-domain RMS from AdaptiveMic)
    //   3: Bass-only flux (BandFlux bass band flux, no mid/high)
    //   4: Spectral centroid (tracks spectral SHAPE, robust to uniform energy modulation)
    //   5: Bass ratio (bass energy / total energy, kick=high, snare=low)
    uint8_t odfSource = 0;               // Alternative ODF source for ACF (0=default)

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
    bool octaveCheckEnabled = true;      // Shadow CBSS octave check (v32: enabled, +13% F1)
    uint8_t octaveCheckBeats = 2;        // Check every N beats (v32: aggressive, was 4)
    float octaveScoreRatio = 1.3f;       // T/2 must score this much better to switch (v32: was 1.5)

    // === PHASE ALIGNMENT CHECKER ===
    // Every N beats, compares raw onset strength (OSS) at current beat phase vs
    // phase shifted by T/2. If the shifted phase has consistently stronger onsets,
    // the CBSS has locked to the wrong phase (anti-phase). Corrects by shifting
    // lastBeatSample_ to re-anchor beats at the stronger onset positions.
    // Uses OSS (raw onset detection) not CBSS (which is self-reinforcing at wrong phase).
    bool phaseCheckEnabled = false;       // Phase alignment check (v37: disabled — net-negative on 18-track validation)
    uint8_t phaseCheckBeats = 4;          // Check every N beats (accumulate evidence)
    float phaseCheckRatio = 1.2f;         // Shifted phase must score this much better to correct

    // === BTRACK-STYLE TEMPO PIPELINE ===
    // Replaces multiplicative Bayesian fusion with BTrack's sequential pipeline:
    // Adaptive threshold on comb-on-ACF + Viterbi max-product.
    // Skips FT/IOI/IIR comb observations and post-hoc harmonic disambiguation.
    bool btrkPipeline = true;            // BTrack pipeline (v33: enabled, replaces multiplicative fusion)
    uint8_t btrkThreshWindow = 0;        // Adaptive threshold half-window (0=off, 1-5 bins each side)

    // === BAR-POINTER HMM BEAT TRACKING (Phase 3.1, v34) ===
    // Joint tempo-phase tracking via bar-pointer HMM (Bock/madmom 2016).
    // State = (tempo_bin, position_within_beat). Phase advances deterministically
    // each frame; tempo can only change at beat boundaries. Replaces CBSS + detectBeat.
    // Reuses transMatrix_ and tempoBinLags_ from Bayesian tempo fusion.
    bool barPointerHmm = false;          // Enable HMM beat tracking (A/B vs CBSS)
    float hmmContrast = 2.0f;            // ODF power-law contrast (higher = sharper beat/non-beat)
    bool hmmTempoNorm = true;            // Normalize argmax by period (prevents slow-tempo bias)

    // === FOURIER TEMPOGRAM (enables per-bin observation in Bayesian fusion) ===
    bool ftEnabled = false;              // Fourier tempogram observation (disabled: no ref system uses FT for real-time beat tracking)

    // === BAYESIAN TEMPO FUSION ===
    // Fuses autocorrelation, Fourier tempogram, comb filter bank, and IOI histogram
    // into a unified posterior distribution over 40 tempo bins (60-180 BPM).
    // Each signal provides an observation likelihood; the posterior = prior × Π(observations).
    float bayesLambda = 0.60f;           // Transition tightness (0.01=rigid, 1.0=loose)
    float bayesPriorCenter = 128.0f;     // Static prior center BPM (Gaussian)
    float bayesPriorWeight = 0.0f;       // Ongoing static prior strength (0=off, 1=standard, 2=strong)
    float bayesAcfWeight = 0.8f;         // Autocorrelation observation weight (high: harmonic comb makes ACF reliable)
    float bayesFtWeight = 0.0f;          // Fourier tempogram observation weight (disabled: suspected flat observation vectors)
    float bayesCombWeight = 0.7f;        // Comb filter bank observation weight
    float bayesIoiWeight = 0.0f;         // IOI histogram observation weight (disabled: O(n²) complexity, unnormalized counts)
    float posteriorFloor = 0.05f;        // Uniform mixing ratio to prevent mode lock (0=off, 0.05=5% floor)
    float disambigNudge = 0.15f;         // Posterior mass transfer when disambiguation corrects (0=off)
    float harmonicTransWeight = 0.30f;   // Transition matrix harmonic shortcut weight (0=off, 0.3=default)

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
    static constexpr int OSS_FRAME_RATE = 60;  // OSS samples per second (tied to mic frame rate)
    static constexpr float OSS_FRAMES_PER_MIN = OSS_FRAME_RATE * 60.0f;  // 3600.0 — lag-to-BPM conversion
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
    float prevOdfForDiff_ = 0.0f;       // Previous ODF value for HWR first-difference mode
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

    // Octave check state (Phase 3)
    uint16_t beatsSinceOctaveCheck_ = 0; // Beats since last octave check

    // Phase alignment check state (v37)
    uint16_t beatsSincePhaseCheck_ = 0;  // Beats since last phase alignment check

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
    // 40 bins matching CombFilterBank resolution (60-180 BPM, ~3 BPM/bin)
    static constexpr int TEMPO_BINS = CombFilterBank::NUM_FILTERS;  // 40
    float tempoStatePrior_[TEMPO_BINS] = {0};     // Previous posterior (becomes prior)
    float tempoStatePost_[TEMPO_BINS] = {0};      // Current posterior after update
    float tempoStaticPrior_[TEMPO_BINS] = {0};    // Fixed Gaussian prior (ongoing pull toward bayesPriorCenter)
    float rayleighWeight_[TEMPO_BINS] = {0};      // Rayleigh tempo prior peaked at ~120 BPM (BTrack-style)
    float tempoBinBpms_[TEMPO_BINS] = {0};        // BPM value for each bin
    int tempoBinLags_[TEMPO_BINS] = {0};          // Lag value for each bin (at ~60 Hz)
    float transMatrix_[TEMPO_BINS][TEMPO_BINS] = {{0}};  // Precomputed Gaussian transition probabilities
    float transMatrixLambda_ = -1.0f;             // bayesLambda used to compute transMatrix_ (-1 = uninitialized)
    float transMatrixHarmonic_ = -1.0f;          // harmonicTransWeight used to compute transMatrix_
    bool tempoStateInitialized_ = false;
    int bayesBestBin_ = TEMPO_BINS / 2;              // Best bin from last fusion (for debug)
    float lastFtObs_[TEMPO_BINS] = {0};           // Last FT observations (for debug)
    float lastCombObs_[TEMPO_BINS] = {0};         // Last comb observations (for debug)
    float lastIoiObs_[TEMPO_BINS] = {0};          // Last IOI observations (for debug)

    // === BAR-POINTER HMM STATE (Phase 3.1, v34) ===
    // State space: (tempo_bin, position) where position ∈ [0, period[tempo_bin])
    // Total states = sum of all periods ≈ 780 for 20 bins (lags 18-60)
    static constexpr int MAX_HMM_STATES = 900;  // Upper bound for 20 bins
    float hmmAlpha_[MAX_HMM_STATES] = {0};      // Forward probability vector
    int hmmStateOffsets_[TEMPO_BINS + 1] = {0};  // Cumulative offset per tempo bin
    int hmmPeriods_[TEMPO_BINS] = {0};           // Period (frames/beat) per tempo bin
    int totalHmmStates_ = 0;                     // Actual total states
    int hmmBestTempo_ = 0;                       // Best tempo bin (from argmax)
    int hmmBestPosition_ = 0;                    // Best position within beat
    int hmmPrevBestPosition_ = -1;               // Previous frame's best position (for phase wrap detection)
    bool hmmInitialized_ = false;

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
    void checkOctaveAlternative();
    void checkPhaseAlignment();
    void switchTempo(int newPeriodSamples);

    // Bar-pointer HMM beat tracking (Phase 3.1)
    void initHmmState();
    void updateHmmForward(float onsetStrength);
    void detectBeatHmm();

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
    void buildTransitionMatrix();
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
