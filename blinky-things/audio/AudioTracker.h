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
    float infer(const float*)                       { return 0.0f; }
    float getLastOnset() const                       { return 0.0f; }
    void setProfileEnabled(bool)                    {}
    void printDiagnostics(Print&) const              {}
};
#endif

#include "SharedSpectralAnalysis.h"
#include "../inputs/AdaptiveMic.h"
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"

// Pattern slot cache (v82): caches PLP patterns for instant section recall
static constexpr int SLOT_COUNT = 4;
static constexpr int SLOT_BINS = 16;

struct PatternSlot {
    float bins[SLOT_BINS];   // Resampled PLP pattern digest
    float confidence;         // PLP confidence when snapshot taken
    uint16_t totalBars;      // Bars observed with this pattern
    uint8_t age;             // LRU counter (0 = most recent)
    bool valid;              // Is this slot populated?
    bool seeded;             // Template-seeded (one-shot per slot)
};

/**
 * AudioTracker - Audio analysis with decoupled tempo/onset architecture
 *
 * ACF + PLP architecture:
 *   Period path:  spectral flux + bass energy → ACF → beat period → bar multipliers
 *   Onset path:   FrameOnsetNN → pulse detection (visual sparks)
 *   Pattern path:  PLP epoch-folds NN-gated flux at detected period → repeating pattern
 *
 * Key design: Period estimation uses multi-source ACF (bass + flux, NN-independent).
 * Beat-level ACF peaks are multiplied (2×/3×/4×) to find bar-level candidates.
 * Epoch-fold variance scoring selects the period with the best pattern contrast.
 * NN onset drives visual pulse and gates the epoch-fold source for pattern quality.
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
    int getPlpPeriod() const { return plpBestPeriod_; }  // ACF period in frames (~66Hz)
    int getHwGain() const;

    // === Debug accessors ===
    float getPeriodicityStrength() const { return periodicityStrength_; }
    float getLastPulseStrength() const { return lastPulseStrength_; }
    uint16_t getBeatCount() const { return beatCount_; }
    float getPlpPhase() const { return plpPhase_; }
    float getPlpConfidence() const { return plpConfidence_; }
    float getPlpAccentPhase() const { return plpAccentPhase_; }
    float getPlpPulseValue() const { return plpPulseValue_; }
    int getPlpPatternLen() const { return plpPatternLen_; }
    float getAcfPeakStrength() const { return acfPeakStrength_; }
    int getPlpBestSource() const { return plpBestSource_; }
    float getBeatStability() const { return beatStability_; }
    float getOnsetDensity() const { return onsetDensity_; }
    float getBpmMin() const { return bpmMin; }
    float getBpmMax() const { return bpmMax; }
    uint32_t getLastAcfMs() const { return lastAcfDurationMs_; }
    uint32_t getLastPlpMs() const { return lastPlpMs_; }

    // Alias for JSON audio stream ("onset" field)
    float getLastOnsetStrength() const { return lastPulseStrength_; }

    // Raw NN activation (before pulse detection threshold/cooldown)
    float getRawNNActivation() const { return rawNNActivation_; }
    uint32_t getRawNNPeakMs() const { return rawNNPeakMs_; }

    // Pattern slot cache (v82)
    int getActiveSlotId() const { return activeSlot_; }
    const struct PatternSlot& getSlot(int i) const { return slots_[i]; }
    static constexpr int getSlotCount() { return SLOT_COUNT; }
    void resetSlots();

    // === Tunable parameters ===
    // Core tempo
    float bpmMin = 15.0f;              // Captures full-bar patterns (4 beats at 60 BPM = 264 frames)
    float bpmMax = 200.0f;
    float tempoSmoothing = 0.85f;
    uint16_t acfPeriodMs = 100;

    // Rhythm activation
    float activationThreshold = 0.3f;

    // PLP (Predominant Local Pulse)
    float plpConfAlpha = 0.25f;        // Confidence EMA smoothing rate (was 0.15)
    float plpNovGain = 1.5f;           // Pattern contrast: power-law exponent (>1 sharpens peaks)
    float plpSignalFloor = 0.10f;      // Mic level for full confidence activation
    float plpVarianceSens = 10.0f;     // Epoch-fold variance suppression (higher = more aggressive)

    // NN profiling
    bool nnProfile = false;

    // Spectral flux contrast (power-law sharpening before ACF)
    float odfContrast = 1.25f;

    // Pulse detection thresholds
    float pulseThresholdMult = 2.0f;   // Baseline multiplier for pulse fire
    float pulseMinLevel = 0.03f;       // Minimum mic level to allow pulse
    float pulseOnsetFloor = 0.005f;    // ODF floor — tuned for spectral flux range (0.001-0.05)
    float pulseNNGate = 0.3f;          // NN activation gate — suppress pulse when NN < this

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

    // Pattern slot cache (v82): caches PLP patterns for instant section recall
    float slotSwitchThreshold = 0.70f;   // Cosine sim to recall cached slot
    float slotNewThreshold = 0.40f;      // Below this: allocate new slot
    float slotUpdateRate = 0.15f;        // EMA rate for active slot reinforcement
    float slotSaveMinConf = 0.25f;       // Min PLP confidence to save/update slots (was 0.50)
    float slotSeedBlend = 0.70f;         // Blend ratio when seeding from cached slot

private:
    // === Audio input ===
    ISystemTime& time_;
    AdaptiveMic mic_;
    SharedSpectralAnalysis spectral_;
    FrameOnsetNN frameOnsetNN_;
    bool nnActive_ = false;
    uint32_t lastSpectralFrameCount_ = 0;  // Track new frames via getFrameCount()

    // === OSS buffer (~12 seconds: 792 samples @ ~66 Hz) ===
    // Sized for 3 epochs at MAX_PATTERN_LEN (264 × 3 = 792)
    static constexpr int OSS_BUFFER_SIZE = 792;
    static constexpr float OSS_FRAME_RATE = 66.0f;
    static constexpr float OSS_FRAMES_PER_MIN = OSS_FRAME_RATE * 60.0f;
    float ossBuffer_[OSS_BUFFER_SIZE] = {0};       // Ungated spectral flux (NN-independent, for ACF period detection)
    float ossLinear_[OSS_BUFFER_SIZE] = {0};        // Linearized ungated flux (for period detection)
    float gatedFluxBuffer_[OSS_BUFFER_SIZE] = {0};  // NN-gated flux (for epoch-fold pattern extraction only)
    float gatedFluxLinear_[OSS_BUFFER_SIZE] = {0};   // Linearized NN-gated flux (for epoch-fold)
    int ossWriteIdx_ = 0;
    int ossCount_ = 0;

    // === Tempo state ===
    float bpm_ = 120.0f;
    float beatPeriodFrames_ = OSS_FRAME_RATE * 60.0f / 120.0f;  // ~33 frames
    float periodicityStrength_ = 0.0f;

    // === PLP state (Predominant Local Pulse) ===
    static constexpr int BASS_BUFFER_SIZE = 792;  // Same size as OSS buffer
    static constexpr int NN_BUFFER_SIZE = 792;    // NN onset activation history
    static constexpr int MAX_PATTERN_LEN = 264;   // 1 bar at 60 BPM (4 beats × 66 frames/beat)
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
    float patternPosition_ = 0.0f;              // 0→1 position within pattern cycle (advances each frame)
    float plpPhase_ = 0.0f;                     // 0→1 phase for generators (0 = accent, derived from position)
    float plpConfidence_ = 0.0f;                // Dual-source agreement confidence
    float plpPulseValue_ = 0.5f;               // Current pulse value (pattern at phase position)
    float cachedBassEnergy_ = 0.0f;            // Cached bass mel energy (shared by PLP + energy synthesis)
    float acfPeakStrength_ = 0.0f;                   // Normalized ACF peak of winning period (0-1)
    int plpBestPeriod_ = 33;                   // Winning period from ACF + epoch-fold scoring (frames)
    float plpAccentPhase_ = 0.0f;             // Epoch-fold accent position (0-1 within period)
    float plpPeakEma_ = 0.0f;                // EMA of PLP peak amplitudes (beat stability tracking)
    float beatStability_ = 0.0f;              // Current PLP peak / peak EMA (0=disrupted, 1=locked)
    uint8_t plpBestSource_ = 0;                // 0=flux, 1=bass, 2=nn (which source won)
    uint16_t beatCount_ = 0;                    // Beat counter (increments on phase wrap)

    // === Pulse detection ===
    float odfBaseline_ = 0.0f;        // Floor-tracking baseline
    float odfPeakHold_ = 0.0f;        // Peak-hold for energy synthesis
    float lastPulseStrength_ = 0.0f;
    uint32_t lastPulseMs_ = 0;

    float prevOdf_ = 0.0f;             // Previous frame ODF (for rising-edge detection)

    // === Raw NN activation tracking (before threshold/cooldown) ===
    float rawNNActivation_ = 0.0f;    // Current NN output (unfiltered)
    uint32_t rawNNPeakMs_ = 0;        // Timestamp of last NN activation peak

    // === Onset density ===
    float onsetDensity_ = 0.0f;
    int onsetCountInWindow_ = 0;
    uint32_t onsetDensityWindowStart_ = 0;

    // === Pulse envelope (spectral flux auto-normalized) ===
    float fluxPeak_ = 0.001f;        // Running peak of spectral flux (for normalization)
    float pulseEnvelope_ = 0.0f;     // Decaying envelope triggered by onset events
    float prevPulseOutput_ = 0.0f;   // For onset density counting

    // === ACF timing ===
    uint32_t lastAcfMs_ = 0;
    uint32_t lastAcfDurationMs_ = 0;  // Timing: last runAcf()+updatePlpAnalysis() duration
    uint32_t lastPlpMs_ = 0;       // Timing: last updatePlpAnalysis() duration

    // === Silence detection ===
    uint32_t lastSignificantAudioMs_ = 0;

    // === Pattern slot cache (v82) ===
    PatternSlot slots_[SLOT_COUNT] = {};
    float currentDigest_[SLOT_BINS] = {0};
    int activeSlot_ = -1;
    uint16_t lastSlotCheckBeat_ = 0;
    float prevBpm_ = 120.0f;          // For BPM shift detection

    // === Output ===
    AudioControl control_;

    // === Internal methods ===
    void addOssSample(float ungatedFlux, float gatedFlux);
    void addBassSample(float bassEnergy);
    void resetAnalysisState();
    void runAcf();
    void updatePulseDetection(float odf, float dt, uint32_t nowMs);
    void updatePlpAnalysis();       // Epoch-fold + bass ACF + cross-correlate (ACF cadence)
    void updatePlpPhase();          // Advance phase + read pattern value (every frame)
    void synthesizeOutputs(float dt, uint32_t nowMs);

    // Pattern slot cache methods
    void checkPatternSlots();
    int allocateSlot();
    static void resamplePattern(const float* src, int srcLen, float* dst, int dstLen);
    static float cosineSimilarity(const float* a, const float* b, int len);

};
