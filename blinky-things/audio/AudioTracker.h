#pragma once

#include "AudioControl.h"
#include "CombFilterBank.h"
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
 * Replaces AudioController's 2162-line CBSS system with a ~400-line
 * ACF + Comb + PLL architecture with decoupled signal paths:
 *
 *   BPM path:  SharedSpectralAnalysis → spectral flux → OSS buffer
 *                → ACF (tempo) + Comb bank (validation) → BPM estimate
 *
 *   Onset path: SharedSpectralAnalysis → FrameOnsetNN (Conv1D W16, ~7ms)
 *                → onset activation → pulse detection (visual sparks)
 *                → PLL phase refinement (onset-gated correction)
 *
 *   Phase path: PLL free-running sawtooth at BPM → smooth phase ramp
 *
 * Key design: BPM estimation uses spectral flux (NN-independent), not NN
 * onset activation. The NN detects acoustic onsets (kicks/snares) which
 * drive visual pulse and refine PLL phase, but cannot distinguish on-beat
 * from off-beat onsets — so it must not drive tempo estimation.
 *
 * ~10 tunable parameters (vs ~56 in AudioController).
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
    float getCombBankBPM() const { return combFilterBank_.getPeakBPM(); }
    float getCombBankConfidence() const { return combFilterBank_.getPeakConfidence(); }
    float getPllPhase() const { return pllPhase_; }
    float getPllIntegral() const { return pllIntegral_; }
    float getOnsetDensity() const { return onsetDensity_; }
    float getBpmMin() const { return bpmMin; }
    float getBpmMax() const { return bpmMax; }

    // Compatibility stubs for SerialConsole commands that reference
    // AudioController-specific metrics. Return sensible defaults.
    float getBeatStability() const { return periodicityStrength_; }
    float getTempoVelocity() const { return 0.0f; }
    uint32_t getNextBeatMs() const { return 0; }
    uint32_t getLastOnsetTimeMs() const { return 0; }
    float getCbssConfidence() const { return periodicityStrength_; }
    float getCurrentCBSS() const { return 0.0f; }
    float getLastOnsetStrength() const { return lastPulseStrength_; }
    float getTimeToNextBeat() const { return 0.0f; }
    bool wasLastBeatPredicted() const { return false; }
    int getBayesBestBin() const { return 0; }
    float getBayesBestConf() const { return 0.0f; }
    float getBayesCombObs() const { return 0.0f; }
    float getBeatPeriodSamples() const { return beatPeriodFrames_; }

    // === Tunable parameters (~10 total) ===
    float bpmMin = 60.0f;
    float bpmMax = 200.0f;
    float rayleighBpm = 130.0f;
    float combFeedback = 0.855f;
    float pllKp = 0.15f;
    float pllKi = 0.005f;
    float activationThreshold = 0.3f;
    float odfGateThreshold = 0.20f;
    float tempoSmoothing = 0.85f;
    uint16_t acfPeriodMs = 150;

    // Phase-aware onset confidence modulation (v75)
    // Replaces binary boost/suppress with subdivision-aware cosine proximity curve.
    // Phase alignment matters; octave errors don't (half/double time still looks musical).
    float pulseBoostOnBeat = 1.3f;         // Max boost for confident on-grid onsets
    float energyBoostOnBeat = 0.3f;        // Energy boost near beat subdivisions
    float confFloor = 0.4f;                // Min confidence for maximally off-grid onsets (0=suppress, 1=passthrough)
    float confActivation = 0.3f;           // rhythmStrength below this: no modulation (all onsets passthrough)
    float confFullModulation = 0.7f;       // rhythmStrength above this: full phase-based modulation
    float subdivTolerance = 0.10f;         // Phase distance for "near subdivision" (at 120 BPM, 0.10 = 50ms)

    // NN profiling
    bool nnProfile = false;

    // === Newly exposed tuning constants (v74) ===
    // Spectral flux contrast (power-law sharpening before ACF/comb)
    float odfContrast = 1.25f;

    // Pulse detection thresholds
    float pulseThresholdMult = 2.0f;   // Baseline multiplier for pulse fire
    float pulseMinLevel = 0.03f;       // Minimum mic level to allow pulse

    // PLL tuning constants
    float pllOnsetFloor = 0.1f;        // ODF values below this get zero PLL correction
    float pllNearBeatWindow = 0.25f;   // Phase distance (0-0.5) for onset-gated correction
    float pllIntegralDecay = 0.95f;    // Leaky integrator decay rate
    float pllSilenceDecay = 0.99f;     // Integral decay during silence

    // Percival ACF harmonic enhancement weights
    float percivalWeight2 = 0.5f;      // 2nd harmonic fold weight
    float percivalWeight4 = 0.25f;     // 4th harmonic fold weight

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
    float energyBoostWindow = 0.25f;   // Phase distance for beat-proximity boost

    // Pattern memory (v77): IOI histogram discovers tempo from onset intervals,
    // bar histogram reveals repeating pattern for prediction. See PATTERN_HISTOGRAM_DESIGN.md.
    float patternLearnRate = 0.15f;    // EMA alpha for histogram update
    float patternDecayRate = 0.9995f;  // Per-frame global decay (half-life ~22s)
    float patternGain = 0.3f;          // Prediction boost strength for onset confidence
    float anticipationGain = 0.1f;     // Energy pre-ramp before predicted onsets
    float patternLookahead = 0.05f;    // Phase lookahead (fraction of beat)
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
    int ossWriteIdx_ = 0;
    int ossCount_ = 0;

    // === Comb filter bank ===
    CombFilterBank combFilterBank_;

    // === Tempo state ===
    float bpm_ = 120.0f;
    float beatPeriodFrames_ = OSS_FRAME_RATE * 60.0f / 120.0f;  // ~33 frames
    float periodicityStrength_ = 0.0f;

    // === PLL state ===
    float pllPhase_ = 0.0f;           // 0→1 sawtooth
    float pllIntegral_ = 0.0f;        // Leaky integrator for frequency correction
    uint16_t beatCount_ = 0;

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
    void runAutocorrelation();
    void updatePulseDetection(float odf, float dt, uint32_t nowMs);
    void updatePll(float odf, uint32_t nowMs);
    void synthesizeOutputs(float dt, uint32_t nowMs);

    // Pattern memory methods
    void recordOnsetForPattern(float strength, uint32_t nowMs);
    void updateIoiAnalysis();
    void updateBarHistogram(float strength, uint32_t nowMs);
    void computePatternStats();
    float predictOnsetStrength(uint32_t nowMs);
    void decayPatternBins();

    // Percival harmonic enhancement on ACF
    void percivalEnhance(float* acf, int minLag, int maxLag, int harmonicMaxLag, int acfSize);
};
