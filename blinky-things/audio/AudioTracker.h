#pragma once

#include "AudioControl.h"
#include "../hal/PlatformDetect.h"

#if defined(BLINKY_PLATFORM_NRF52840) || defined(BLINKY_PLATFORM_ESP32S3)
#include "FrameOnsetNN.h"
#else
struct FrameOnsetNN {
    static constexpr int INPUT_MEL_BANDS = 26;
    bool begin()                                    { return false; }
    bool isReady() const                            { return false; }
    bool hasDownbeatOutput() const                  { return false; }
    float infer(const float*)                       { return 0.0f; }
    float getLastOnset() const                       { return 0.0f; }
    float getLastDownbeat() const                   { return 0.0f; }
    void setProfileEnabled(bool)                    {}
    void printDiagnostics() const                   {}
};
#endif

#include "SharedSpectralAnalysis.h"
#include "../inputs/AdaptiveMic.h"
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"

/**
 * AudioTracker - Audio analysis with decoupled tempo/onset architecture
 *
 * ACF + PLP architecture:
 *   BPM path:  spectral flux → OSS buffer → ACF → period estimate
 *   Onset path: FrameOnsetNN → pulse detection (visual sparks)
 *   Pulse path: PLP epoch-folds flux at detected period → repeating energy pattern
 *
 * Key design: BPM estimation uses spectral flux (NN-independent). NN onset
 * drives visual pulse only. PLP extracts the dominant repeating energy pattern
 * without needing onset-beat classification.
 */
class AudioTracker {
public:
    AudioTracker(IPdmMic& pdm, ISystemTime& time);
    ~AudioTracker();

    bool begin(uint32_t sampleRate = 16000);
    void end();

    const AudioControl& update(float dt);
    const AudioControl& getControl() const { return control_; }

    // === Accessors for SerialConsole / ConfigStorage ===
    AdaptiveMic& getMicForTuning() { return mic_; }
    const AdaptiveMic& getMic() const { return mic_; }
    SharedSpectralAnalysis& getSpectral() { return spectral_; }
    const SharedSpectralAnalysis& getSpectral() const { return spectral_; }
    const FrameOnsetNN& getFrameOnsetNN() const { return frameOnsetNN_; }
    float getCurrentBpm() const { return bpm_; }
    int getHwGain() const;

    // === Debug accessors ===
    float getPeriodicityStrength() const { return periodicityStrength_; }
    float getLastPulseStrength() const { return lastPulseStrength_; }
    uint16_t getBeatCount() const { return beatCount_; }
    float getPlpPhase() const { return plpPhase_; }
    float getPlpConfidence() const { return plpConfidence_; }
    float getPlpPulseValue() const { return plpPulseValue_; }
    int getPlpPatternLen() const { return plpPatternLen_; }
    float getPlpBestPmr() const { return plpBestPmr_; }
    int getPlpBestSource() const { return plpBestSource_; }
    float getOnsetDensity() const { return onsetDensity_; }
    float getBpmMin() const { return bpmMin; }
    float getBpmMax() const { return bpmMax; }

    // Alias for JSON audio stream ("onset" field)
    float getLastOnsetStrength() const { return lastPulseStrength_; }
    float getPatternConfidence() const { return patternConfidence_; }
    float getIoiConfidence() const { return ioiConfidence_; }
    float getIoiPeakBpm() const { return ioiPeakBpm_; }
    float getIoiPeakMs() const { return ioiPeakMs_; }
    float getBarEntropy() const { return barEntropy_; }
    uint16_t getPatternBarsAccumulated() const { return patternBarsAccumulated_; }
    const float* getBarBins() const { return barBins_; }
    static constexpr int getBarBinCount() { return BAR_BINS; }

    // Pattern memory reset (for test automation)
    void resetPatternMemory();

    // === Tunable parameters ===
    // Core tempo
    float bpmMin = 60.0f;
    float bpmMax = 200.0f;
    float tempoSmoothing = 0.85f;
    uint16_t acfPeriodMs = 150;

    // Rhythm activation
    float activationThreshold = 0.3f;

    // PLP (Predominant Local Pulse)
    float plpActivation = 0.3f;        // Min PLP confidence for pattern pulse (below: cosine fallback)
    float plpConfAlpha = 0.15f;        // Confidence EMA smoothing rate
    float plpNovGain = 1.5f;           // Pattern contrast/novelty scaling

    // NN profiling
    bool nnProfile = false;

    // Spectral flux contrast (power-law sharpening before ACF)
    float odfContrast = 1.25f;

    // Pulse detection thresholds
    float pulseThresholdMult = 2.0f;   // Baseline multiplier for pulse fire
    float pulseMinLevel = 0.03f;       // Minimum mic level to allow pulse
    float pulseOnsetFloor = 0.1f;      // ODF floor for pulse detection scaling

    // ODF baseline tracking rates
    float baselineFastDrop = 0.05f;    // Fast drop rate for floor tracking
    float baselineSlowRise = 0.005f;   // Slow rise rate for floor tracking

    // ODF peak-hold decay
    float odfPeakHoldDecay = 0.85f;    // Peak-hold release rate (~100ms at 62.5Hz)

    // Energy synthesis blend weights (should sum to ~1.0 for expected behavior;
    // not auto-normalized — setting all low under-drives energy, all high over-drives)
    float energyMicWeight = 0.30f;     // Broadband mic level weight
    float energyMelWeight = 0.30f;     // Bass mel energy weight
    float energyOdfWeight = 0.40f;     // ODF peak-hold transient weight

    // Pattern memory (v77): IOI histogram discovers tempo from onset intervals,
    // bar histogram reveals repeating pattern for prediction. See PATTERN_HISTOGRAM_DESIGN.md.
    float patternLearnRate = 0.15f;    // EMA alpha for histogram update
    float patternDecayRate = 0.9995f;  // Per-frame bar bin decay (half-life ~22s)
    float ioiDecayRate = 0.999f;       // Per-frame IOI bin decay (half-life ~11s)
    float patternGain = 0.3f;          // Prediction boost strength for onset confidence
    float anticipationGain = 0.1f;     // Energy pre-ramp before predicted onsets
    float patternLookahead = 0.05f;    // Phase lookahead (fraction of beat)
    float confidenceRise = 0.05f;      // Confidence EMA alpha when rising
    float confidenceDecay = 0.15f;     // Confidence EMA alpha when falling
    float histogramMinStrength = 0.5f; // Min onset strength for bar histogram (filters hi-hats)
    bool patternEnabled = true;        // Master enable for A/B testing

private:
    // === Audio input ===
    ISystemTime& time_;
    AdaptiveMic mic_;
    SharedSpectralAnalysis spectral_;
    FrameOnsetNN frameOnsetNN_;
    bool nnActive_ = false;
    uint32_t lastSpectralFrameCount_ = 0;  // Track new frames via getFrameCount()

    // === OSS buffer (~5.5 seconds: 360 samples @ ~66 Hz) ===
    static constexpr int OSS_BUFFER_SIZE = 360;
    static constexpr float OSS_FRAME_RATE = 66.0f;
    static constexpr float OSS_FRAMES_PER_MIN = OSS_FRAME_RATE * 60.0f;
    float ossBuffer_[OSS_BUFFER_SIZE] = {0};
    float ossLinear_[OSS_BUFFER_SIZE] = {0};  // Linearized OSS (shared by ACF + PLP)
    int ossWriteIdx_ = 0;
    int ossCount_ = 0;

    // === Tempo state ===
    float bpm_ = 120.0f;
    float beatPeriodFrames_ = OSS_FRAME_RATE * 60.0f / 120.0f;  // ~33 frames
    float periodicityStrength_ = 0.0f;

    // === PLP state (Predominant Local Pulse) ===
    static constexpr int BASS_BUFFER_SIZE = 360;  // Same size as OSS buffer
    static constexpr int NN_BUFFER_SIZE = 360;    // NN onset activation history
    static constexpr int MAX_PATTERN_LEN = 66;    // Max period in frames (66 Hz / 60 BPM)
    float bassBuffer_[BASS_BUFFER_SIZE] = {0};
    float bassLinear_[BASS_BUFFER_SIZE] = {0};    // Linearized bass (shared by ACF + PLP)
    int bassWriteIdx_ = 0;
    int bassCount_ = 0;
    float nnOnsetBuffer_[NN_BUFFER_SIZE] = {0};   // Raw NN activation per frame
    float nnLinear_[NN_BUFFER_SIZE] = {0};        // Linearized NN onset (shared by ACF + PLP)
    int nnWriteIdx_ = 0;
    int nnCount_ = 0;

    float plpPattern_[MAX_PATTERN_LEN] = {0};   // Epoch-folded pattern (one period)
    int plpPatternLen_ = 33;                     // Current pattern length (BPM-dependent)
    float plpPhase_ = 0.0f;                     // 0→1 phase within pattern cycle
    float plpConfidence_ = 0.0f;                // Dual-source agreement confidence
    float plpPulseValue_ = 0.5f;               // Current pattern value at phase position
    float plpBassPeriod_ = 33.0f;              // Bass ACF dominant period (frames)
    float cachedBassEnergy_ = 0.0f;            // Cached bass mel energy (shared by PLP + energy synthesis)
    float plpBestPmr_ = 0.0f;                 // PMR of winning epoch-fold (diagnostic)
    int plpBestPeriod_ = 33;                   // Winning period from grid search (frames, pre-smoothing)
    uint8_t plpBestSource_ = 0;                // 0=flux, 1=bass, 2=nn (which source won)
    uint16_t beatCount_ = 0;                    // Beat counter (increments on phase wrap)

    // === Pulse detection ===
    float odfBaseline_ = 0.0f;        // Floor-tracking baseline
    float odfPeakHold_ = 0.0f;        // Peak-hold for energy synthesis
    float lastPulseStrength_ = 0.0f;
    uint32_t lastPulseMs_ = 0;

    // === Onset density ===
    float onsetDensity_ = 0.0f;
    int onsetCountInWindow_ = 0;
    uint32_t onsetDensityWindowStart_ = 0;

    // === ACF timing ===
    uint32_t lastAcfMs_ = 0;

    // === Silence detection ===
    uint32_t lastSignificantAudioMs_ = 0;

    // === Pattern memory (v77) ===
    // Phase A: IOI histogram — discovers tempo from raw onset intervals
    static constexpr int ONSET_BUF_SIZE = 64;
    static constexpr int IOI_BINS = 128;          // 100-1600ms range (~12ms per bin)
    static constexpr float IOI_MIN_MS = 100.0f;
    static constexpr float IOI_MAX_MS = 1600.0f;
    // Beat-range IOI bins: 250ms (240 BPM) to 1000ms (60 BPM)
    static constexpr int IOI_BEAT_LOW = (int)((250.0f - IOI_MIN_MS) * IOI_BINS / (IOI_MAX_MS - IOI_MIN_MS));
    static constexpr int IOI_BEAT_HIGH = (int)((1000.0f - IOI_MIN_MS) * IOI_BINS / (IOI_MAX_MS - IOI_MIN_MS));
    uint32_t onsetTimes_[ONSET_BUF_SIZE] = {0};
    uint8_t onsetWriteIdx_ = 0;
    uint8_t onsetBufCount_ = 0;
    float ioiBins_[IOI_BINS] = {0};
    float ioiPeakMs_ = 500.0f;                   // Dominant IOI (ms)
    float ioiPeakBpm_ = 120.0f;                  // BPM from IOI peak
    float ioiPeakStrength_ = 0.0f;
    float ioiConfidence_ = 0.0f;                  // Agreement between IOI and ACF

    // Phase B: Bar-position histogram — 16 bins (16th-note resolution)
    static constexpr int BAR_BINS = 16;
    float barBins_[BAR_BINS] = {0};
    float barEntropy_ = 1.0f;
    float patternConfidence_ = 0.0f;
    uint16_t patternBarsAccumulated_ = 0;
    uint32_t lastBarBoundaryMs_ = 0;

    // === Output ===
    AudioControl control_;

    // === Internal methods ===
    void addOssSample(float odf);
    void addBassSample(float bassEnergy);
    void runAutocorrelation();
    void updatePulseDetection(float odf, float dt, uint32_t nowMs);
    void updatePlpAnalysis();       // Epoch-fold + bass ACF + cross-correlate (ACF cadence)
    void updatePlpPhase();          // Advance phase + read pattern value (every frame)
    void synthesizeOutputs(float dt, uint32_t nowMs);

    // Pattern memory methods
    void recordOnsetForPattern(float strength, uint32_t nowMs);
    void updateIoiAnalysis();
    void updateBarHistogram(float strength, uint32_t nowMs);
    void computePatternStats();
    float predictOnsetStrength(uint32_t nowMs);
    void decayPatternBins();

};
