#pragma once

#include "AudioControl.h"
#include "CombFilterBank.h"
#include "../hal/PlatformDetect.h"

#if defined(BLINKY_PLATFORM_NRF52840) || defined(BLINKY_PLATFORM_ESP32S3)
#include "FrameBeatNN.h"
#else
struct FrameBeatNN {
    static constexpr int INPUT_MEL_BANDS = 26;
    bool begin()                                    { return false; }
    bool isReady() const                            { return false; }
    bool hasDownbeatOutput() const                  { return false; }
    float infer(const float*)                       { return 0.0f; }
    float getLastBeat() const                       { return 0.0f; }
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
 * AudioTracker - Simplified audio analysis with PLL phase tracking
 *
 * Replaces AudioController's 2162-line CBSS system with a ~400-line
 * ACF + Comb + PLL architecture. Targets 60fps (NN W16 ~7ms + DSP ~0.3ms).
 *
 * Architecture:
 *   PDM Microphone → AdaptiveMic → SharedSpectralAnalysis → FrameBeatNN (W16)
 *       → onset activation → OSS buffer → ACF (tempo) + Comb bank (validation)
 *       → PLL (smooth phase ramp) → AudioControl output
 *
 * Key design: PLL free-runs at estimated BPM, producing a perfectly smooth
 * phase ramp. Only corrected when strong onsets align with expected beat.
 * No CBSS, no Bayesian fusion, no predict/countdown, no onset snap.
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
    const FrameBeatNN& getFrameBeatNN() const { return frameBeatNN_; }
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
    uint32_t getLastBeatTimeMs() const { return 0; }
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
    float rayleighBpm = 140.0f;
    float combFeedback = 0.92f;
    float pllKp = 0.15f;
    float pllKi = 0.005f;
    float activationThreshold = 0.3f;
    float odfGateThreshold = 0.25f;
    float tempoSmoothing = 0.85f;
    uint16_t acfPeriodMs = 150;

    // Pulse modulation (keep from AudioController for generator compatibility)
    float pulseBoostOnBeat = 1.3f;
    float pulseSuppressOffBeat = 0.6f;
    float energyBoostOnBeat = 0.3f;
    float pulseNearBeatThreshold = 0.2f;
    float pulseFarFromBeatThreshold = 0.3f;

    // NN profiling
    bool nnProfile = false;

private:
    // === Audio input ===
    ISystemTime& time_;
    AdaptiveMic mic_;
    SharedSpectralAnalysis spectral_;
    FrameBeatNN frameBeatNN_;
    bool nnActive_ = false;

    // === OSS buffer (6 seconds @ ~66 Hz) ===
    static constexpr int OSS_BUFFER_SIZE = 360;
    static constexpr float OSS_FRAME_RATE = 66.0f;
    static constexpr float OSS_FRAMES_PER_MIN = OSS_FRAME_RATE * 60.0f;
    float ossBuffer_[OSS_BUFFER_SIZE] = {0};
    uint32_t ossTimestamps_[OSS_BUFFER_SIZE] = {0};
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

    // === Output ===
    AudioControl control_;

    // === Internal methods ===
    void addOssSample(float odf);
    void runAutocorrelation();
    void updatePulseDetection(float odf, float dt, uint32_t nowMs);
    void updatePll(float odf, uint32_t nowMs);
    void synthesizeOutputs(float dt, uint32_t nowMs);

    // Percival harmonic enhancement on ACF
    void percivalEnhance(float* acf, int minLag, int maxLag, int harmonicMaxLag, int acfSize);
};
