#pragma once

#include "AudioControl.h"
#include "BeatActivationNN.h"
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
 * Memory: ~12.4 KB total (resonatorDelay_[47][66] = 12,408 bytes + state)
 * CPU: ~3% (47 filters × simple math, phase every 4 frames)
 */
class CombFilterBank {
public:
    static constexpr int MAX_LAG = 66;  // 60 BPM at 66 Hz (66*60/66)
    static constexpr int MIN_LAG = 20;  // 198 BPM at 66 Hz (66*60/20)
    // 47 filters: every integer lag from MIN_LAG to MAX_LAG (60-198 BPM)
    // Lag-domain uniform spacing gives natural ~2 BPM resolution at 120 BPM
    // (vs old 20-bin system with ~7 BPM resolution and only 2 bins at 120-140 BPM).
    // v29 tested 40 bins but failed due to BPM-space Gaussian transition matrix;
    // v43 fixed this with lag-space Gaussian + column normalization.
    // Reference: madmom uses 82 lag bins, BTrack uses 41, BeatNet uses 300.
    static constexpr int NUM_FILTERS = MAX_LAG - MIN_LAG + 1;  // 47

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
     * Updates all 47 resonators and finds peak tempo
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
    // Memory: 47 filters × 66 samples × 4 bytes = 12,408 bytes
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

    // === BAND WEIGHTING ===
    // Fixed band weights for spectral flux computation (bass/mid/high)
    float bassBandWeight = 0.5f;    // Bass band weight
    float midBandWeight = 0.3f;     // Mid band weight
    float highBandWeight = 0.2f;    // High band weight

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
    float cbssContrast = 1.0f;           // Power-law ODF contrast before CBSS (BTrack uses 2.0; higher = sharper beat peaks)
    uint8_t cbssWarmupBeats = 0;         // CBSS warmup: lower alpha for first N beats (0=disabled; tested 8, increased variance 5.5x)
    uint8_t onsetSnapWindow = 8;         // Snap beat anchor to strongest OSS in last N frames (0=disabled; default 8 frames)

    // === BEAT-BOUNDARY TEMPO UPDATES (Phase 2.1) ===
    // Defers beatPeriodSamples_ changes to the next beat fire, synchronizing
    // tempo and beat timing like BTrack. Prevents mid-beat period discontinuities.
    bool beatBoundaryTempo = true;       // Defer tempo changes to beat boundaries (BTrack-style)

    // === UNIFIED ODF (Phase 2.4) ===
    // Uses BandFlux pre-threshold continuous activation as the ODF for CBSS beat tracking,
    // instead of the separate computeSpectralFluxBands(). Ensures transient detection and
    // beat tracking see the same signal. BTrack uses a single ODF for both.
    bool unifiedOdf = true;              // Use BandFlux pre-threshold as CBSS ODF (BTrack-style)

    // === NN BEAT ACTIVATION ===
    // Replaces BandFlux ODF with a learned causal CNN beat activation function.
    // Requires ENABLE_NN_BEAT_ACTIVATION compile flag and valid model in beat_model_data.h.
    // When enabled and model loads successfully, overrides unifiedOdf for the ODF source.
    // BandFlux still runs for transient detection (sparks/effects); only the ODF changes.
    bool nnBeatActivation = true;        // Use NN beat activation as ODF (A/B tested, 11/18 wins)

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
    // Priority: odfSource > onsetTrainOdf > odfDiffMode > default (only one active)
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
    bool downwardCorrectEnabled = false;  // Downward harmonic correction (experimental: overcorrects on mid-tempo, disabled by default)
    bool octaveCheckEnabled = true;      // Shadow CBSS octave check (v32: enabled, +13% F1)
    uint8_t octaveCheckBeats = 2;        // Check every N beats (v32: aggressive, was 4)
    float octaveScoreRatio = 1.3f;       // T/2 must score this much better to switch (v32: was 1.5)

    // === METRICAL CONTRAST CHECK (v48) ===
    // Compares raw onset strength at beat positions vs midpoints.
    // Weak contrast indicates possible octave error — triggers checkOctaveAlternative().
    bool metricalCheckEnabled = false;      // Enable metrical contrast check (v48)
    float metricalMinRatio = 1.5f;          // Min beat/midpoint strength ratio (v48, 1.0-5.0)
    uint8_t metricalCheckBeats = 4;         // Check every N beats (v48, 2-8)

    // (phaseCheckEnabled removed v44 — net-negative on 18-track validation)
    // (plpPhaseEnabled/plpCorrectionStrength/plpMinConfidence removed v44 — zero effect, redundant with onset snap)

    // === BTRACK-STYLE TEMPO PIPELINE ===
    // Replaces multiplicative Bayesian fusion with BTrack's sequential pipeline:
    // Adaptive threshold on comb-on-ACF + Viterbi max-product.
    // Skips FT/IOI/IIR comb observations and post-hoc harmonic disambiguation.
    bool btrkPipeline = true;            // BTrack pipeline (v33: enabled, replaces multiplicative fusion)
    uint8_t btrkThreshWindow = 0;        // Adaptive threshold half-window (0=off, 1-5 bins each side)

    // === PHASE TRACKER BEAT DETECTION (v46b, formerly bar-pointer HMM) ===
    // Single-tempo phase tracker with continuous ODF observation model.
    // Bayesian fusion handles tempo; this tracks phase within the best period.
    // Beat detection via position-0 wrap (detectHmmBeat).
    // Joint HMM (updateHmmForward) removed v53 — position-wrap doesn't work
    // across 20 tempo bins (argmax jumps between bins).
    bool barPointerHmm = false;          // Enable phase tracker beat detection (A/B vs CBSS)
    bool fwdPhaseOnly = false;           // Hybrid: phase tracker for phase, CBSS for beats (v58)
    float hmmContrast = 2.0f;            // ODF power-law contrast (higher = sharper beat/non-beat)
    // (hmmTempoNorm removed v53 — only used by dead joint HMM updateHmmForward)
    // (hmmLambda removed v53 — only used by dead joint HMM buildHmmTransitionMatrix)
    float fwdObsLambda = 8.0f;           // Continuous ODF observation strength (v49: higher=sharper beat/non-beat)
    float fwdObsFloor = 0.01f;           // Observation probability floor (v52)
    float fwdWrapFraction = 0.25f;       // Wrap detection zone fraction (v52)
    // (hmmBayesBias removed v53 — only used by dead joint HMM updateHmmForward)

    // === PARTICLE FILTER BEAT TRACKING (v38) ===
    // Maintains 100 tempo/phase hypotheses; octave variants injected at resampling
    // compete on equal footing via observation model. Most principled octave
    // disambiguation within CPU/RAM budget. A/B testable vs CBSS+Bayesian.
    bool particleFilterEnabled = false;    // Enable PF (A/B vs CBSS+Bayesian)
    float pfNoise = 0.08f;                // Period diffusion noise (fraction of period, applied at beat boundaries only)
    float pfBeatSigma = 0.05f;            // Beat kernel width (fraction of period) — unused in v39 madmom model, kept for serial compat
    float pfOctaveInjectRatio = 0.10f;    // Fraction of particles to replace with octave variants
    float pfBeatThreshold = 0.25f;        // Weighted fraction near phase=0 to trigger beat
    float pfNeffRatio = 0.5f;             // Resample when Neff < ratio * N
    float pfContrast = 1.0f;              // ODF power-law contrast for PF likelihood
    float pfInfoGate = 0.10f;             // Information gate: floor ODF below this to 0.03 (BeatNet-style, 0=off)
    uint8_t pfObsLambda = 8;              // Observation model lambda: beat region = 1/lambda of period (madmom-style, 2-32)

    // (ftEnabled removed v52 — dead code since v28)

    // === BAYESIAN TEMPO FUSION ===
    // Fuses autocorrelation, Fourier tempogram, comb filter bank, and IOI histogram
    // into a unified posterior distribution over 40 tempo bins (60-180 BPM).
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
    float rayleighBpm = 120.0f;          // Rayleigh prior peak BPM (60-180)
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

    // === JOINT TEMPO-PHASE FORWARD FILTER (v57, Krebs/Böck 2015) ===
    // Tracks tempo AND phase jointly via forward algorithm with continuous ODF observation.
    // 20 tempo bins × variable phase positions (~860 states). Each state accumulates
    // likelihood at every frame. Tempo changes only at beat boundaries (phase wrap).
    // Replaces CBSS countdown + Bayesian tempo when enabled.
    bool forwardFilterEnabled = false;      // Enable joint forward filter (A/B vs CBSS+Bayesian)
    float fwdTransSigma = 3.0f;            // Tempo transition width in lag units (1-10, tighter=less octave jump)
    float fwdFilterContrast = 2.0f;        // ODF power-law contrast (1=linear, 2-4=sharper discrimination)
    float fwdFilterLambda = 8.0f;          // Beat zone = 1/lambda of period (4-32, higher=narrower beat zone)
    float fwdFilterFloor = 0.01f;          // Observation probability floor (prevents zero-out)
    float fwdBayesBias = 0.2f;             // Bayesian posterior modulation strength (0=off, 1=full posterior, v59)
    float fwdAsymmetry = 0.8f;             // Asymmetric non-beat penalty by tempo (0=off, 0.8=optimal, v60)

    // === MULTI-AGENT BEAT TRACKING (v48) ===
    // 8 beat agents at different phases compete; best-scoring agent fires beats.
    // Replaces single CBSS countdown when enabled. CBSS still provides the cumulative
    // beat strength signal; agents use it for onset quality scoring.
    bool multiAgentEnabled = false;         // Enable multi-agent phase competition (A/B vs single CBSS)
    float agentDecay = 0.85f;              // Agent score EMA decay (0.7-0.95, lower = faster adaptation)
    uint8_t agentInitBeats = 3;            // Initialize agents after N beats (2-8)

    // === RHYTHMIC PATTERN TEMPLATES (v50, Krebs/Böck/Widmer ISMIR 2013) ===
    // Bins OSS into 16 bar-phase slots at candidate tempos, correlates against
    // EDM templates. Switches tempo if alternative scores better by templateScoreRatio.
    bool templateCheckEnabled = false;         // Enable template-based octave check
    float templateScoreRatio = 1.3f;           // Min score ratio to switch (1.0-3.0)
    uint8_t templateCheckBeats = 4;            // Check every N beats (2-8)

    // === BEAT CRITIC SUBBEAT ALTERNATION (v50, Davies ISMIR 2010) ===
    // Divides beats into subbeatBins subbeat bins, compares even vs odd energy.
    // High alternation at T but low at T/2 → switch to T/2.
    bool subbeatCheckEnabled = false;          // Enable subbeat alternation check
    float alternationThresh = 1.2f;            // Odd/even ratio threshold (0.3-3.0)
    uint8_t subbeatCheckBeats = 4;             // Check every N beats (2-8)

    // === HIDDEN CALIBRATION CONSTANTS (v51, exposed for parameter sweeps) ===
    float templateMinScore = 0.1f;         // Min Pearson correlation to consider tempo switch
    uint8_t subbeatBins = 8;              // Number of subbeat bins for alternation check (even, 4-16)
    uint8_t templateHistBars = 2;         // Template history depth in bars (1-4)
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

    EnsembleDetector& getEnsemble() { return ensemble_; }
    const EnsembleDetector& getEnsemble() const { return ensemble_; }

    const BeatActivationNN& getBeatActivationNN() const { return beatActivationNN_; }

    // Get last ensemble output for debugging
    const EnsembleOutput& getLastEnsembleOutput() const { return ensemble_.getLastOutput(); }

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

    // Particle filter debug getters (v38)
    bool isParticleFilterActive() const { return particleFilterEnabled && pfInitialized_; }
    float getPfNeff() const { return pfNeff_; }
    float getPfBeatFraction() const { return pfBeatFraction_; }

private:
    // === HAL REFERENCES ===
    ISystemTime& time_;

    // === MICROPHONE ===
    AdaptiveMic mic_;

    // === ENSEMBLE DETECTOR ===
    EnsembleDetector ensemble_;
    EnsembleOutput lastEnsembleOutput_;

    // === NN BEAT ACTIVATION ===
    BeatActivationNN beatActivationNN_;

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

    // Spectral flux: previous frame magnitudes for frame-to-frame comparison
    static constexpr int SPECTRAL_BINS = 128;  // FFT_SIZE / 2
    float prevMagnitudes_[SPECTRAL_BINS] = {0};
    bool prevMagnitudesValid_ = false;  // First frame has no previous

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
    uint16_t beatsSinceMetricalCheck_ = 0; // Beats since last metrical contrast check (v48)
    uint16_t beatsSinceTemplateCheck_ = 0; // Beats since last template match check (v50)
    uint16_t beatsSinceSubbeatCheck_ = 0;  // Beats since last subbeat alternation check (v50)

    // (beatsSincePhaseCheck_ removed v44 — phase check feature removed)
    // (plpPhase_/plpConfidence_ removed v44 — PLP feature removed)

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
    // 47 bins matching CombFilterBank resolution (60-198 BPM, lag-domain uniform)
    static constexpr int TEMPO_BINS = CombFilterBank::NUM_FILTERS;  // 47
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

    // === PHASE TRACKER STATE (v46b) ===
    // Used by updatePhaseTracker() and detectHmmBeat().
    // Joint HMM arrays (hmmAlpha_, hmmStateOffsets_, hmmTransMatrix_, etc.) removed v53.
    int hmmBestTempo_ = 0;                       // Best tempo bin (from Bayesian via phase tracker)
    int hmmBestPosition_ = 0;                    // Best position within beat
    int hmmPrevBestPosition_ = -1;               // Previous frame's best position (for phase wrap detection)
    int hmmPrevBestTempo_ = 0;                   // Previous frame's best tempo (spurious wrap detection)

    // === PHASE-ONLY TRACKER (v46b) ===
    // Single-tempo circular distribution — Bayesian handles tempo, this tracks phase.
    // All probability mass stays in one period, unlike joint HMM where mass spreads
    // across tempo bins leaving the Bayesian bin's positions unreliable.
    static constexpr int PHASE_MAX_PERIOD = CombFilterBank::MAX_LAG + 1;  // 67
    float phaseAlpha_[PHASE_MAX_PERIOD] = {0};  // Circular probability distribution
    int phasePeriod_ = 0;                        // Current tracked period (from Bayesian)
    int phaseFramesSinceBeat_ = 999;             // Frames since last phase-tracker beat (wrap cooldown)
    void updatePhaseTracker(float odf);

    // === JOINT FORWARD FILTER STATE (v57) ===
    // 47 tempo bins with variable phase positions (= lag per bin, 20-66 frames).
    // Total states: sum of all lags = 2021. Uses existing tempoBinLags_[] for periods.
    static constexpr int FWD_MAX_STATES = 2100;  // Sum of lags 20..66 = 2021 + margin
    float fwdAlpha_[FWD_MAX_STATES] = {0};       // Forward probabilities
    int fwdBinOffset_[TEMPO_BINS] = {0};          // Start index of each bin in fwdAlpha_
    int fwdTotalStates_ = 0;                      // Actual total states
    float fwdTransMatrix_[TEMPO_BINS][TEMPO_BINS] = {{0}};  // Tempo transition probabilities
    float fwdTransSigmaLast_ = -1.0f;            // Last sigma used to build transition matrix
    int fwdMinPeriod_ = 10;                      // Cached min period across tempo bins
    bool fwdInitialized_ = false;
    int fwdBestBin_ = TEMPO_BINS / 2;            // Best tempo bin (~120 BPM)
    int fwdBestPos_ = 0;                          // Best phase position
    int fwdPrevBestBin_ = TEMPO_BINS / 2;
    int fwdPrevBestPos_ = -1;
    int fwdFramesSinceBeat_ = 999;

    // === PARTICLE FILTER STATE (v38) ===
    static constexpr int PF_NUM_PARTICLES = 100;
    static constexpr float PF_INFO_GATE_ODF_FLOOR = 0.03f;  // Floor value for gated ODF during silence
    static constexpr float PF_LIKELIHOOD_EPSILON = 0.01f;    // Small epsilon to avoid zero likelihoods
    struct BeatParticle {
        float period;    // MIN_LAG-MAX_LAG frames (60-200 BPM at 66 Hz)
        float position;  // 0.0 to period
        float weight;    // Unnormalized likelihood
    };
    BeatParticle pfParticles_[PF_NUM_PARTICLES];
    BeatParticle pfResampleBuf_[PF_NUM_PARTICLES];  // Scratch for resampling
    bool pfInitialized_ = false;
    float pfNeff_ = 0.0f;
    float pfSmoothedPeriod_ = 33.0f;  // EMA-smoothed consensus period (init ~120 BPM at 66 Hz)
    float pfBeatFraction_ = 0.0f;
    float pfPrevBeatFraction_ = 0.0f;
    uint32_t pfRngState_ = 0x12345678;
    int pfCooldown_ = 0;  // Beat cooldown counter (frames)

    // === PLL PHASE CORRECTION STATE (v45) ===
    float pllPhaseIntegral_ = 0.0f;  // PLL integral accumulator

    // === ADAPTIVE TIGHTNESS STATE (v45) ===
    float effectiveTightness_ = 8.0f;  // Current effective tightness (modulated by onset confidence)

    // === MULTI-AGENT BEAT TRACKING STATE (v48) ===
    static constexpr int NUM_BEAT_AGENTS = 8;
    struct BeatAgent {
        int countdown;        // Frames until next predicted beat
        float score;          // EMA quality score (higher = better onset alignment)
        int lastBeatSample;   // Onset-snapped beat anchor
        bool justFired;       // Flag: this agent's countdown hit 0 this frame
    };
    BeatAgent beatAgents_[NUM_BEAT_AGENTS];
    int bestAgentIdx_ = 0;
    bool agentsInitialized_ = false;
    int agentPeriod_ = 30;      // Cached period used by agents

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

    // Phase tracker beat detection (v46b, joint HMM removed v53)
    void detectHmmBeat();             // v46: position-0 wrap beat detection

    // Joint forward filter (v57)
    void initForwardFilter();
    void updateForwardFilter(float odf);
    void detectForwardFilterBeat();

    // Multi-agent beat tracking (v48)
    void initBeatAgents();
    void detectBeatMultiAgent();
    void checkMetricalContrast();
    void checkTemplateMatch();         // Rhythmic pattern template check (v50)
    void checkSubbeatAlternation();    // Beat critic subbeat alternation (v50)

    // Particle filter beat tracking (v38)
    void initParticleFilter();
    void pfUpdate(float odf);
    void pfPredict();
    void pfUpdateWeights(float odf);
    void pfResample();
    void pfDetectBeat();
    void pfExtractConsensus();
    float pfRandom();           // LCG uniform 0 to 1
    float pfGaussianRandom();   // Box-Muller

    // ODF smoothing
    float smoothOnsetStrength(float raw);

    // Log-Gaussian weight computation
    void recomputeLogGaussianWeights(int T);

    // Onset strength computation
    float computeSpectralFluxBands(const float* magnitudes, int numBins,
                                    float& bassFlux, float& midFlux, float& highFlux);

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
