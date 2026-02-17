#pragma once

#include "AudioControl.h"
#include "EnsembleDetector.h"
#include "PerceptualScaling.h"
#include "../inputs/AdaptiveMic.h"
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"

// ============================================================================
// Multi-Hypothesis Tempo Tracking
// ============================================================================

/**
 * TempoHypothesis - A single tempo tracking hypothesis
 *
 * Represents one possible tempo interpretation of the music.
 * Multiple hypotheses run concurrently to handle tempo ambiguity,
 * tempo changes, and polyrhythmic patterns.
 *
 * Memory: 56 bytes per hypothesis
 */
struct TempoHypothesis {
    // Tempo estimate
    float bpm;                      // BPM estimate (60-200)
    float beatPeriodMs;             // Beat period in milliseconds

    // Phase tracking
    float phase;                    // Current beat phase (0-1)

    // Confidence and evidence
    float strength;                 // Periodicity strength (0-1, from autocorrelation)
    float confidence;               // Overall confidence (0-1, weighted combination)
    float avgPhaseError;            // Running average of phase prediction error

    // Timing and history
    uint32_t lastUpdateMs;          // Last update timestamp
    uint16_t beatCount;             // Number of beats tracked
    uint32_t createdMs;             // Creation timestamp
    float beatsSinceUpdate;         // Fractional beats since last autocorr update (for decay)

    // Autocorrelation evidence
    float correlationPeak;          // Peak autocorrelation value
    int lagSamples;                 // Lag in samples (for verification)

    // State
    bool active;                    // Is this slot in use?
    uint8_t priority;               // 0=primary, 1=secondary, 2=tertiary, 3=candidate

    TempoHypothesis()
        : bpm(120.0f)
        , beatPeriodMs(500.0f)
        , phase(0.0f)
        , strength(0.0f)
        , confidence(0.0f)
        , avgPhaseError(0.0f)
        , lastUpdateMs(0)
        , beatCount(0)
        , createdMs(0)
        , beatsSinceUpdate(0.0f)
        , correlationPeak(0.0f)
        , lagSamples(0)
        , active(false)
        , priority(3)
    {}

    /**
     * Update phase and beat count based on elapsed time
     */
    void updatePhase(float dt);

    /**
     * Apply phrase-aware decay (beat-count based)
     */
    void applyBeatDecay(float minStrengthToKeep);

    /**
     * Apply time-based decay during silence
     */
    void applySilenceDecay(float dt, float minStrengthToKeep);

    /**
     * Compute overall confidence from strength, consistency, and longevity
     */
    float computeConfidence(float strengthWeight, float consistencyWeight, float longevityWeight) const;
};

/**
 * Debug verbosity levels for hypothesis tracking
 */
enum class HypothesisDebugLevel {
    OFF = 0,        // No debug output
    EVENTS = 1,     // Hypothesis creation, promotion, eviction only
    SUMMARY = 2,    // Primary hypothesis status every 2s
    DETAILED = 3    // All hypotheses every 2s
};

/**
 * MultiHypothesisTracker - Manages 4 concurrent tempo hypotheses
 *
 * Tracks multiple tempo interpretations simultaneously, allowing the system
 * to handle tempo changes, ambiguity (half-time vs double-time), and
 * polyrhythmic patterns.
 *
 * Uses LRU eviction strategy with primary-hypothesis protection.
 *
 * Memory: ~240 bytes total
 */
class MultiHypothesisTracker {
public:
    static constexpr int MAX_HYPOTHESES = 4;

    // Hypothesis slots: [0]=primary, [1]=secondary, [2]=tertiary, [3]=candidate
    TempoHypothesis hypotheses[MAX_HYPOTHESES];

    // === TUNING PARAMETERS ===

    // Peak detection
    float minPeakStrength = 0.3f;           // Minimum normalized correlation to create hypothesis
    float minRelativePeakHeight = 0.7f;     // Peak must be >70% of max peak (reject weak secondaries)

    // Hypothesis matching
    float bpmMatchTolerance = 0.05f;        // ±5% BPM tolerance for matching peaks to hypotheses

    // Promotion
    float promotionThreshold = 0.15f;       // Confidence advantage needed to promote (0-1)
    uint16_t minBeatsBeforePromotion = 8;   // Minimum beats before promoting a new hypothesis

    // Decay
    float phraseHalfLifeBeats = 32.0f;      // Half-life in beats (8 bars of 4/4)
    float minStrengthToKeep = 0.1f;         // Deactivate hypotheses below this strength
    uint32_t silenceGracePeriodMs = 3000;   // Grace period before silence decay (ms)
    float silenceDecayHalfLifeSec = 5.0f;   // Half-life during silence (seconds)

    // Confidence weighting
    float strengthWeight = 0.5f;            // Weight of periodicity strength in confidence
    float consistencyWeight = 0.3f;         // Weight of phase consistency in confidence
    float longevityWeight = 0.2f;           // Weight of beat count in confidence

    // Debug level (OFF by default to avoid flooding serial with JSON events)
    HypothesisDebugLevel debugLevel = HypothesisDebugLevel::OFF;

    // === METHODS ===

    /**
     * Create a new hypothesis in the best available slot
     * Returns slot index, or -1 if creation failed
     */
    int createHypothesis(float bpm, float strength, uint32_t nowMs, int lagSamples, float correlation);

    /**
     * Find best slot for new hypothesis (LRU eviction with primary protection)
     */
    int findBestSlot();

    /**
     * Find hypothesis matching a given BPM (within tolerance)
     * Returns slot index, or -1 if no match
     */
    int findMatchingHypothesis(float bpm) const;

    /**
     * Get primary hypothesis (always slot 0)
     */
    TempoHypothesis& getPrimary() { return hypotheses[0]; }
    const TempoHypothesis& getPrimary() const { return hypotheses[0]; }

    /**
     * Update phase, confidence, and decay for a single hypothesis
     */
    void updateHypothesis(int index, float dt, uint32_t nowMs, bool hasSignificantAudio);

    /**
     * Promote best non-primary hypothesis if significantly better
     */
    void promoteBestHypothesis(uint32_t nowMs);

    /**
     * Print debug information based on current debug level
     */
    void printDebug(uint32_t nowMs, const char* eventType = nullptr, int slotIndex = -1) const;

private:
    uint32_t lastDebugPrintMs_ = 0;

    // cppcheck-suppress unusedPrivateFunction ; False positive - used in findMatchingHypothesis()
    inline float absf(float val) const {
        return val < 0.0f ? -val : val;
    }
};

/**
 * Autocorrelation peak structure (used for multi-peak extraction)
 */
struct AutocorrPeak {
    int lag;
    float correlation;
    float normCorrelation;
};

// ============================================================================
// Comb Filter Phase Tracker (Phase 4)
// ============================================================================

/**
 * CombFilterPhaseTracker - Independent phase tracking using comb filter resonance
 *
 * Theory: A comb filter at lag L accumulates energy when the input has
 * periodicity at L samples. The phase of the accumulated signal indicates
 * beat alignment. This provides an independent phase estimate that can be
 * fused with autocorrelation and Fourier phase methods.
 *
 * Memory: ~500 bytes (delay line + state)
 * CPU: < 1% (simple IIR filter)
 */
class CombFilterPhaseTracker {
public:
    static constexpr int MAX_LAG = 120;  // ~2s at 60Hz = 30 BPM min

    // === TUNING PARAMETERS ===
    float feedbackGain = 0.92f;   // Resonance strength (0.85-0.98)

    // === METHODS ===

    /**
     * Reset all state
     */
    void reset();

    /**
     * Set tempo (updates lag from BPM)
     */
    void setTempo(float bpm, float frameRate = 60.0f);

    /**
     * Process one sample of onset strength
     */
    void process(float input);

    /**
     * Get current phase estimate (0-1)
     */
    float getPhase() const { return phase_; }

    /**
     * Get confidence in phase estimate (0-1)
     * Based on resonator amplitude stability
     */
    float getConfidence() const { return confidence_; }

    /**
     * Get current lag in samples
     */
    int getCurrentLag() const { return currentLag_; }

    /**
     * Get resonator output (for debugging)
     */
    float getResonatorOutput() const { return resonatorOutput_; }

private:
    // Delay line
    float delayLine_[MAX_LAG] = {0};
    int writeIdx_ = 0;

    // Current lag (beat period in samples)
    int currentLag_ = 30;  // Default ~120 BPM at 60 Hz

    // Resonator state
    float resonatorOutput_ = 0.0f;
    float prevResonatorOutput_ = 0.0f;

    // Phase tracking via zero-crossing/peak detection
    float phase_ = 0.0f;
    int samplesSincePeak_ = 0;

    // Confidence from amplitude stability
    float confidence_ = 0.0f;
    float peakAmplitude_ = 0.0f;

    // Running stats for confidence
    float runningMax_ = 0.0f;
    float runningMean_ = 0.0f;
    int sampleCount_ = 0;
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
 * Rhythm Tracking Approach (pattern-based, not event-based):
 *   1. Buffer 6 seconds of onset strength (spectral flux)
 *   2. Run autocorrelation to find periodicity and tempo
 *   3. Derive phase from autocorrelation pattern
 *   4. Predict beats ahead of time
 *
 * Key Design Decision:
 *   Transient detection drives VISUAL PULSE output only.
 *   Beat tracking is derived from buffered pattern analysis.
 *   This prevents unreliable transients from disrupting beat sync.
 *
 * Architecture:
 *   PDM Microphone
 *        |
 *   AdaptiveMic (level normalization, gain control)
 *        |
 *   EnsembleDetector (6 detectors + fusion)
 *        |
 *   OSS Buffer (6s) --> Autocorrelation --> Tempo + Phase
 *        |                                      |
 *   Ensemble Transient --> Pulse (visual only)  |
 *        |                                      |
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

    // BPM range constraints
    void setBpmRange(float minBpm, float maxBpm);
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

    // Phase tracking smoothing
    float phaseAdaptRate = 0.15f;       // How quickly phase adapts to autocorrelation (0-1)

    // === TRANSIENT-BASED PHASE CORRECTION (PLL-style) ===
    // Uses detected transients to nudge phase toward actual beat timing
    // Requires 2+ detector agreement to prevent single-detector false positives from drifting phase
    float transientCorrectionRate = 0.15f;  // How fast to apply transient-based correction (0-1)
    float transientCorrectionMin = 0.45f;   // Minimum transient strength to trigger correction

    // Beat proximity thresholds for pulse modulation
    float pulseNearBeatThreshold = 0.2f;    // Phase distance < this = boost transients
    float pulseFarFromBeatThreshold = 0.3f; // Phase distance > this = suppress transients

    // BPM range for autocorrelation tempo detection
    float bpmMin = 60.0f;               // Minimum BPM to detect (affects autocorr lag range)
    float bpmMax = 200.0f;              // Maximum BPM to detect (affects autocorr lag range)

    // === TEMPO PRIOR (reduces half-time/double-time confusion) ===
    // Gaussian prior centered on typical music tempo, weights autocorrelation peaks
    bool tempoPriorEnabled = true;      // Enable tempo prior weighting
    float tempoPriorCenter = 120.0f;    // Center of Gaussian prior (BPM)
    float tempoPriorWidth = 40.0f;      // Width (sigma) of Gaussian prior (BPM)
    float tempoPriorStrength = 0.5f;    // Blend: 0=no prior, 1=full prior weight

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

    // === ONSET STRENGTH SIGNAL (OSS) GENERATION ===
    // Controls how the onset strength signal is computed for autocorrelation
    // Spectral flux captures energy CHANGES, RMS captures absolute levels
    float ossFluxWeight = 1.0f;  // 1.0 = pure spectral flux, 0.0 = pure RMS (legacy)

    // === ADAPTIVE BAND WEIGHTING ===
    // Dynamically adjusts band weights based on which frequency bands show strongest periodicity
    // When enabled, bands with stronger rhythmic content get higher weights
    // Uses SuperFlux-style max filtering, cross-band correlation, and peakiness detection
    // to distinguish real beats from vibrato/tremolo in sustained content
    bool adaptiveBandWeightEnabled = true;  // Enable/disable adaptive weighting

    // === PULSE TRAIN PHASE ESTIMATION ===
    // Uses PLP-inspired Fourier phase extraction at tempo frequency
    // Based on Predominant Local Pulse (PLP) method by Grosche & Müller (2011)
    // Testing showed +10-11% F1 improvement on full-mix and full-kit patterns
    float pulsePhaseWeight = 1.0f;  // 1.0 = Fourier phase (default), 0.0 = peak-finding only

    // === COMB FILTER PHASE TRACKER (Phase 4) ===
    // Independent phase tracking using comb filter resonance
    // Provides complementary phase estimate to autocorrelation and Fourier methods
    float combFilterWeight = 0.0f;  // 0.0 = disabled (default), 1.0 = full weight in fusion
    float combFeedback = 0.92f;     // Resonance strength (0.85-0.98)

    // === RHYTHM FUSION (Phase 5) ===
    // Combines phase estimates from multiple systems with confidence weighting
    // System 0: Autocorrelation (peak-finding or Fourier phase)
    // System 1: Comb filter resonator
    // Transient hints provide small nudges (max ±10%)
    bool fusionEnabled = true;              // Enable fusion (when multiple systems active)
    float transientHintWeight = 0.1f;       // Max influence of transient hints (0-0.2)

    // === ADVANCED ACCESS (for debugging/tuning only) ===

    AdaptiveMic& getMicForTuning() { return mic_; }
    const AdaptiveMic& getMic() const { return mic_; }

    EnsembleDetector& getEnsemble() { return ensemble_; }
    const EnsembleDetector& getEnsemble() const { return ensemble_; }

    // Get last ensemble output for debugging
    const EnsembleOutput& getLastEnsembleOutput() const { return ensemble_.getLastOutput(); }

    // Multi-hypothesis tracker access
    MultiHypothesisTracker& getMultiHypothesis() { return multiHypothesis_; }
    const MultiHypothesisTracker& getMultiHypothesis() const { return multiHypothesis_; }

    // Debug getters
    float getPeriodicityStrength() const { return periodicityStrength_; }
    float getBeatStability() const { return beatStability_; }
    float getTempoVelocity() const { return tempoVelocity_; }
    uint32_t getNextBeatMs() const { return nextBeatMs_; }
    float getLastTempoPriorWeight() const { return lastTempoPriorWeight_; }

    // Adaptive band weight debug getters
    const float* getAdaptiveBandWeights() const { return adaptiveBandWeights_; }
    const float* getBandPeriodicityStrength() const { return bandPeriodicityStrength_; }

    // Pulse train phase debug getters
    float getPulseTrainPhase() const { return pulseTrainPhase_; }
    float getPulseTrainConfidence() const { return pulseTrainConfidence_; }

    // Comb filter phase debug getters
    float getCombFilterPhase() const { return combFilter_.getPhase(); }
    float getCombFilterConfidence() const { return combFilter_.getConfidence(); }
    const CombFilterPhaseTracker& getCombFilter() const { return combFilter_; }

    // Fusion debug getters
    float getFusedPhase() const { return fusedPhase_; }
    float getFusedConfidence() const { return fusedConfidence_; }
    float getConsensusMetric() const { return consensusMetric_; }

private:
    // === HAL REFERENCES ===
    ISystemTime& time_;

    // === MICROPHONE ===
    AdaptiveMic mic_;

    // === PERCEPTUAL SCALING ===
    PerceptualScaling perceptual_;  // Public for ConfigStorage access

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
    static constexpr uint32_t BAND_AUTOCORR_PERIOD_MS = 500;  // Run every 500ms (faster response)

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

    // Multi-hypothesis tempo tracking
    MultiHypothesisTracker multiHypothesis_;

    // Comb filter phase tracker (Phase 4)
    CombFilterPhaseTracker combFilter_;

    // Current tempo estimate (for backward compatibility during transition)
    // TODO: These will be replaced by multiHypothesis_.getPrimary() values
    float bpm_ = 120.0f;
    float beatPeriodMs_ = 500.0f;
    float periodicityStrength_ = 0.0f;

    // Phase tracking (derived from autocorrelation, not from transients)
    float phase_ = 0.0f;                // Current beat phase (0-1)
    float targetPhase_ = 0.0f;          // Phase derived from autocorrelation

    // Pulse train phase estimation state
    float pulseTrainPhase_ = 0.0f;      // Phase from pulse train cross-correlation
    float pulseTrainConfidence_ = 0.0f; // Confidence of pulse train phase (correlation strength)

    // Rhythm fusion state (Phase 5)
    float fusedPhase_ = 0.0f;           // Final fused phase estimate
    float fusedConfidence_ = 0.0f;      // Overall confidence in fused estimate
    float consensusMetric_ = 0.0f;      // How much systems agree (0-1)
    float transientHint_ = 0.0f;        // Current transient-based phase hint

    // Transient-based phase correction (PLL)
    float transientPhaseError_ = 0.0f;  // Running average of phase error when transients detected
    uint32_t lastTransientCorrectionMs_ = 0; // Timestamp of last transient correction

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

    // Debug: last tempo prior weight applied
    float lastTempoPriorWeight_ = 1.0f;

    // Autocorrelation timing
    uint32_t lastAutocorrMs_ = 0;
    static constexpr uint32_t AUTOCORR_PERIOD_MS = 500;  // Run every 500ms

    // Silence detection
    uint32_t lastSignificantAudioMs_ = 0;

    // === SYNTHESIZED OUTPUT ===
    AudioControl control_;

    // === INTERNAL METHODS ===

    // Rhythm tracking
    void addOssSample(float onsetStrength, uint32_t timestampMs);
    void runAutocorrelation(uint32_t nowMs);
    void updatePhase(float dt, uint32_t nowMs);
    void updateTransientPhaseCorrection(float transientStrength, uint32_t nowMs);

    // Onset strength computation
    float computeSpectralFlux(const float* magnitudes, int numBins);
    float computeSpectralFluxBands(const float* magnitudes, int numBins,
                                    float& bassFlux, float& midFlux, float& highFlux);
    float computeMultiBandRms(const float* magnitudes, int numBins);

    // Adaptive band weighting
    void addBandOssSamples(float bassFlux, float midFlux, float highFlux);
    void updateBandPeriodicities(uint32_t nowMs);
    float computeBandAutocorrelation(int band);
    void computeCrossBandCorrelation();
    void computeBandPeakiness();
    void applyMaxFilter(float* magnitudes, int numBins);

    // Pulse train phase estimation (Phase 3)
    float computePulseTrainPhase(int beatPeriodSamples);
    float generateAndCorrelate(int phaseOffset, int beatPeriod);

    // Rhythm fusion (Phase 5)
    void fuseRhythmEstimates();

    // Tempo prior and stability
    float computeTempoPrior(float bpm) const;
    void updateBeatStability(uint32_t nowMs);
    void updateTempoVelocity(float newBpm, float dt);
    void predictNextBeat(uint32_t nowMs);

    // Output synthesis
    void synthesizeEnergy();
    void synthesizePulse();
    void synthesizePhase();
    void synthesizeRhythmStrength();

    // Utilities
    inline float clampf(float val, float minVal, float maxVal) const {
        return val < minVal ? minVal : (val > maxVal ? maxVal : val);
    }
};
