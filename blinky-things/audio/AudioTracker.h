#pragma once

#include "AudioControl.h"
#include "../hal/PlatformDetect.h"

// Onset NN build switch. OFF on devices: on the edm/ corpus with the hat's
// CPU cost model (tests/tracker), tuned spectral flux scores F1 0.542 at
// 178 fps; the v33 NN scores 0.480 and costs ~12 ms/inference, holding the
// loop at 48 fps. Visual smoothness comes first, so a model is enabled only
// when it beats flux under that same measurement (cheaper export: #174).
// The host NN harness (BLINKY_HOST_NN) keeps the NN path tested in CI.
#ifndef BLINKY_ONSET_NN_ENABLED
#  ifdef BLINKY_HOST_NN
#    define BLINKY_ONSET_NN_ENABLED 1
#  else
#    define BLINKY_ONSET_NN_ENABLED 0
#  endif
#endif

#if ((defined(BLINKY_PLATFORM_NRF52840) || defined(BLINKY_PLATFORM_ESP32S3)) && !defined(BLINKY_HOST_HARNESS))     || defined(BLINKY_HOST_NN)
// Device builds, and host harness builds linked against a host-compiled
// TFLite Micro (BLINKY_HOST_NN, see tests/tracker/README.md, #203).
#include "FrameOnsetNN.h"
#else
// Host builds without TFLite (default tests/tracker/ harness): the tracker
// runs its NN-unavailable path (spectral-flux pulses, mic-level NN buffer).
struct FrameOnsetNN {
    static constexpr int INPUT_MEL_BANDS = 26;
    bool begin()                                    { return false; }
    bool isReady() const                            { return false; }
    float infer(const float*, const float* = nullptr, float = 0.0f, float = 0.0f) { return 0.0f; }
    void pushFrame(const float*, const float* = nullptr, float = 0.0f, float = 0.0f) {}
    float invoke()                                  { return 0.0f; }
    float getLastOnset() const                       { return 0.0f; }
    void setProfileEnabled(bool)                    {}
    template <typename P> void printDiagnostics(P&) const {}
};
#endif

#include "SharedSpectralAnalysis.h"
#include "OnsetDetector.h"
#include "PhaseClock.h"
#include "../inputs/AdaptiveMic.h"
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"

// Pattern slot cache (v82): caches PLP patterns for instant section recall
static constexpr int SLOT_COUNT = 4;
static constexpr int SLOT_BINS = 16;

// T1.5 — spectral feature snapshot captured at each onset (either detector).
// Emitted in the transient JSON so offline analysis can cluster TP vs FP
// firings by spectral signature without needing persist_raw signal_frames
// replay.
struct PulseFeatureSnapshot {
    float flatness;
    float rawFlux;
    float centroid;
    float crest;
    float rolloff;
    float hfc;
    float bassRatio;
    float plpPulse;
    float plpConfidence;
};


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
    const FrameOnsetNN& getFrameOnsetNN() const { return nnDetector_.model(); }
    // The onset detector in use (OnsetDetector.h): "spectral-flux" or "onset-nn".
    const char* getOnsetDetectorName() const { return detector_->name(); }
    // Switch detector at runtime by name ("spectral-flux", or "onset-nn" when
    // built in and ready). False for an unknown or unavailable one.
    bool selectOnsetDetector(const char* name);
    float getCurrentBpm() const { return bpm_; }
    int getPlpPeriod() const { return plpBestPeriod_; }  // ACF period in spectral frames (62.5 Hz), integer
    float getPlpPeriodF() const { return plpPeriodF_; }  // Fractional period (frames) — drives phase advance
    int getHwGain() const;

    // === Debug accessors ===
    float getPeriodicityStrength() const { return periodicityStrength_; }
    float getLastPulseStrength() const { return lastPulseStrength_; }
    // T1.4/T1.5 — per-firing diagnostics, populated only when a pulse fires
    // (pulseStrength > 0). Values persist until next firing.
    uint8_t getLastPulseGateMask() const { return lastPulseGateMask_; }
    const PulseFeatureSnapshot& getLastPulseFeatures() const { return lastPulseFeatures_; }
    uint16_t getBeatCount() const { return beatCount_; }
    float getPlpPhase() const { return plpPhase_; }
    float getPlpConfidence() const { return plpConfidence_; }
    float getPlpAccentPhase() const { return plpAccentPhase_; }
    float getPlpPulseValue() const { return plpPulseValue_; }
    int getPlpPatternLen() const { return plpPatternLen_; }
    float getAcfPeakStrength() const { return acfPeakStrength_; }
    int getPlpBestSource() const { return plpBestSource_; }
    float getPlpNNAgreement() const { return plpNNAgreement_; }
    float getPlpReliability() const { return plpMeanReliability_; }
    float getBeatStability() const { return beatStability_; }
    float getOnsetDensity() const { return onsetDensity_; }
    float getBpmMin() const { return bpmMin; }
    float getBpmMax() const { return bpmMax; }
    uint32_t getLastAcfMs() const { return lastAcfDurationMs_; }
    uint32_t getLastPlpMs() const { return lastPlpMs_; }

    // Alias for JSON audio stream ("onset" field)
    float getLastOnsetStrength() const { return lastPulseStrength_; }

    // Onset activation (0-1, either detector) of the last frame with fresh
    // evidence, before peak picking and gates.
    float getOnsetActivation() const { return onsetActivation_; }

    // Absolute mic-sample index (AdaptiveMic ring counter) at which the most
    // recent spectral frame's window ended. Same timeline as record-mode audio.
    uint32_t getLastFrameEndSampleIdx() const { return lastFrameEndSampleIdx_; }
    // Call once at the end of setup(): drops the audio captured while setup
    // ran, uncounted (AdaptiveMic::discardBacklog, #275). Returns samples dropped.
    uint32_t discardSetupBacklog();

    // Pulse strength fired by the frame most recently processed (0 = none).
    // getLastOnsetStrength() is the max over all frames of the last update().
    float getLastFramePulse() const { return lastFramePulse_; }

    // Per-frame observer: called at the end of every processed spectral
    // frame (loop task, inside update()). Record mode uses it to log every
    // frame — not just the last one of each update().
    using FrameHook = void (*)(void* ctx);
    void setFrameHook(FrameHook hook, void* ctx) { frameHook_ = hook; frameHookCtx_ = ctx; }

    // Pattern slot cache (v82)
    int getActiveSlotId() const { return activeSlot_; }
    const struct PatternSlot& getSlot(int i) const { return slots_[i]; }
    static constexpr int getSlotCount() { return SLOT_COUNT; }
    void resetSlots();

    // === Tunable parameters ===
    // Diagnostic: scale all downstream AudioControl values (energy, pulse,
    // plpPulse, rhythmStrength) by this multiplier before generators see
    // them. Default 1.0 = no change. Set to a large value (e.g. 100) to
    // saturate the audio chain and visually distinguish "wired correctly
    // but subtle" from "not wired at all" — every non-zero input clamps
    // to 1.0, so generators should show fully-saturated response on any
    // audio activity. Set to 0 to verify ambient-only baseline.
    float audioAmp = 1.0f;

    // Core tempo
    float bpmMin = 15.0f;              // Captures full-bar patterns (4 beats at 60 BPM = 264 frames)
    float bpmMax = 200.0f;
    float tempoSmoothing = 0.85f;
    float tempoPriorWidth = 0.8f;      // Beat-lag prior width in octaves around 120 BPM (0 = off).
    float syncopatedCandidateScale = 1.0f;  // Bar candidates derived from 1.5/2.5-beat peaks; score scale (0 = off) (#166)
                                       // tests/tracker sweep: 0.8 lifts valid-tempo time 0.60 -> 0.67
                                       // and phase alignment 0.079 -> 0.103 vs off.
    uint16_t acfPeriodMs = 100;

    // Rhythm activation
    float activationThreshold = 0.3f;

    // PLP (Predominant Local Pulse)
    float plpConfAlpha = 0.25f;        // Confidence EMA smoothing rate (was 0.15)
    float plpNovGain = 1.0f;           // Pattern contrast: power-law exponent (>1 sharpens peaks, 1.0=linear)
    float plpSignalFloor = 0.10f;      // Mic level for full confidence activation
    float plpVarianceSens = 0.0f;      // Epoch-fold variance suppression (0=disabled, sweep showed best autoCorr)
    float plpDecayRate = 0.2f;         // Epoch-fold recency decay (sweep: 0.2 best plp@T+autoCorr)
    float plpPhaseGain = 0.3f;         // Fraction of measured phase error removed per analysis (~every 100 ms)

    // NN profiling
    bool nnProfile = false;

    // Spectral flux contrast (power-law sharpening before ACF)
    float odfContrast = 1.25f;

    // Pulse detection
    // Spectral-flux picker delta (FluxOnsetDetector, SuperFlux ODF units).
    float fluxDelta = FluxOnsetDetector::DELTA_DEFAULT;
    // NN activation threshold (the NN's picker delta). With the online
    // picker (no 96 ms window or cooldown) the old 0.20 fired 10/s; edm/
    // host sweep, drum F at 0 dB: 0.4 0.587 | 0.5 0.632 | 0.55 0.641 |
    // 0.6 0.638 | 0.7 0.607 (-20 dB: 0.5 0.656 | 0.55 0.669 | 0.6 0.671).
    static constexpr float PULSE_ONSET_FLOOR_DEFAULT = 0.55f;
    float pulseOnsetFloor = PULSE_ONSET_FLOOR_DEFAULT;
    // Rhythm outputs need recent onsets (see update()): full for
    // RHYTHM_HOLD_S after the last onset, then fade to 0 over RHYTHM_FADE_S.
    static constexpr float RHYTHM_HOLD_S = 2.0f;
    static constexpr float RHYTHM_FADE_S = 2.0f;

    // === Shared onset gate (AudioTracker::pickOnset) ===
    // Applied to every detector's candidates alike (OnsetDetector.h). The
    // bass-ratio gate and the presence gate (pulseMinLevel) were deleted
    // with the online picker (#280): on edm/ the bass gate cost both
    // detectors recall for no drum F (flux 0.649 -> 0.648 off, -20 dB
    // 0.640 -> 0.647; NN 0.641 -> 0.644), and presence never blocked an
    // onset, in music or in noise (the old flux fired 3-6/s on steady
    // noise at -60..-30 dBFS with it on; SuperFlux fires 0).
    //
    // Rhythm-pattern bias: while the PLP pattern is confident, a candidate
    // at a position the pattern predicts no hit needs up to this much more
    // activation (scaled by confidence and by how low the pattern is there;
    // 0 = off). Beat-informed, so keep it easy to switch off for testing;
    // it should become a strength multiplier rather than a gate once onset
    // detection is accurate enough (separate work). Was NN-only.
    static constexpr float ONSET_PATTERN_BIAS_DEFAULT = 0.3f;
    float onsetPatternBias = ONSET_PATTERN_BIAS_DEFAULT;

    // === Shared peak picker (AudioTracker::pickOnset) ===
    // Online peak picking, Boeck, Krebs & Schedl, "Evaluating the online
    // capabilities of onset detection methods" (ISMIR 2012): frame t is an
    // onset if its ODF is the maximum of the last W1 frames and itself, it
    // reaches (mean of the last W3 frames and itself) + delta, and the last
    // onset was more than W5 frames ago. Their online values are 30 / 100 /
    // 30 ms; at 62.5 fps W1 = 2, W3 = 6, W5 = 2. No look-ahead: the onset is
    // reported at the frame that peaks. W3 and delta are per detector
    // (OnsetDetector::averageFrames / delta). This replaces the 96 ms
    // local-max window, the tempo-adaptive 40-150 ms cooldown, the adaptive
    // flux baseline and the crest / beat-grid / re-arm gates: on edm/ the
    // cooldown blocked 61-71% of the kicks the old picker missed (#280).
    static constexpr int ONSET_HISTORY_MAX = 16;  // frames of ODF history kept
    static constexpr uint8_t ONSET_MAX_FRAMES_DEFAULT = 2;   // W1
    static constexpr uint8_t ONSET_MIN_GAP_FRAMES_DEFAULT = 2;  // W5
    uint8_t onsetMaxFrames = ONSET_MAX_FRAMES_DEFAULT;
    uint8_t onsetMinGapFrames = ONSET_MIN_GAP_FRAMES_DEFAULT;
    uint8_t fluxAverageFrames = FluxOnsetDetector::AVERAGE_FRAMES_DEFAULT;  // W3, flux

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
    // Onset detectors (OnsetDetector.h). detector_ is the one in use: the NN
    // when built in and ready, otherwise spectral flux (devices: flux).
    FluxOnsetDetector fluxDetector_;
    NNOnsetDetector<FrameOnsetNN> nnDetector_;
    OnsetDetector* detector_ = &fluxDetector_;
    uint32_t lastSpectralFrameCount_ = 0;  // Track new frames via getFrameCount()

    // === OSS buffer (~12 seconds: 792 samples @ ~66 Hz) ===
    // Sized for 3 epochs at MAX_PATTERN_LEN (264 × 3 = 792)
    static constexpr int OSS_BUFFER_SIZE = 792;
    // THE clock for all rhythm analysis: one OSS sample, one NN inference,
    // one pulse-detector step and one phase step per spectral frame.
    // 16 kHz / 256-sample non-overlapping frames = exactly 62.5 Hz.
    // (Was 66.0 and per-loop-iteration — see docs/AUDIO_ARCHITECTURE.md
    // "Frame clock": phase drifted at loopFps/62.5 and skipped frames were
    // invisible to tempo/NN.)
    static constexpr float OSS_FRAME_RATE =
        SpectralConstants::SAMPLE_RATE / static_cast<float>(SpectralConstants::FFT_SIZE);
    static_assert(OSS_FRAME_RATE > 62.49f && OSS_FRAME_RATE < 62.51f, "frame clock must be 62.5 Hz");
    static constexpr float OSS_FRAMES_PER_MIN = OSS_FRAME_RATE * 60.0f;
    float ossBuffer_[OSS_BUFFER_SIZE] = {0};       // Ungated spectral flux (NN-independent, for ACF period detection)
    float ossLinear_[OSS_BUFFER_SIZE] = {0};        // Linearized ungated flux (for period detection)
    // midFluxBuffer_ / highFluxBuffer_ removed — band selection tested in v82,
    // regressed on 8-10/18 tracks. Broadband (ossBuffer_) is the sole PLP source.
    // Saves 6336 bytes RAM (2 × 792 floats).
    int ossWriteIdx_ = 0;
    int ossCount_ = 0;

    // === Tempo state ===
    float bpm_ = 120.0f;
    float periodicityStrength_ = 0.0f;

    // === PLP state (Predominant Local Pulse) ===
    static constexpr int BASS_BUFFER_SIZE = 792;  // Same size as OSS buffer
    static constexpr int NN_BUFFER_SIZE = 792;    // NN onset activation history
    static constexpr int MAX_PATTERN_LEN = 264;   // 1 bar at 60 BPM (4 beats × 66 frames/beat)
    float bassBuffer_[BASS_BUFFER_SIZE] = {0};
    float bassLinear_[BASS_BUFFER_SIZE] = {0};    // Linearized bass (shared by ACF + PLP)
    int bassWriteIdx_ = 0;
    int bassCount_ = 0;
    // Rhythm source 2. Holds MIC LEVEL per frame in both modes (#155), not the
    // NN activation, despite the nn* names: the NN drives onsets only.
    float nnOnsetBuffer_[NN_BUFFER_SIZE] = {0};   // Mic level per frame (source 2)
    float nnLinear_[NN_BUFFER_SIZE] = {0};        // Linearized source 2 (shared by ACF + PLP)
    int nnWriteIdx_ = 0;
    int nnCount_ = 0;

    float plpPattern_[MAX_PATTERN_LEN] = {0};   // Epoch-folded pattern (one period), ACCENT-ALIGNED: bin 0 = accent
    int plpPatternLen_ = 33;                     // Current pattern length (BPM-dependent)
    float plpPhase_ = 0.0f;                     // 0→1 phase since the accent. Advanced 1/plpPeriodF_ per frame,
                                                // pulled toward the measured accent position at each analysis.
    float plpPeriodF_ = 33.0f;                  // Fractional period (frames) refined from full-resolution ACF
    float plpAccentBin_ = 0.0f;                 // Accent position in the end-aligned fold (bin P-1 = newest frame)
    bool phaseLocked_ = false;                  // First analysis snaps phase; later ones correct gradually
    bool skipNextWrap_ = false;                 // Phase stepped back across 0 — that beat was already counted
    float plpConfidence_ = 0.0f;                // Pattern confidence (ACF × signal × NN agreement)
    float plpNNAgreement_ = 0.5f;               // Cross-correlation between flux fold and source-2 (mic level) fold (0-1)
                                                // Starts at 0.5 (neutral) so PLP confidence isn't penalized during warmup
    float plpMeanReliability_ = 0.0f;           // Mean per-bin reliability (low CV = consistent events)
    float plpPulseValue_ = 0.5f;               // Current pulse value (pattern at phase position)
    float cachedBassEnergy_ = 0.0f;            // Cached bass mel energy (shared by PLP + energy synthesis)
    float acfPeakStrength_ = 0.0f;                   // Normalized ACF peak of winning period (0-1)
    int plpBestPeriod_ = 33;                   // Winning period from ACF + epoch-fold scoring (frames)
    float plpAccentPhase_ = 0.0f;             // Epoch-fold accent position (0-1 within period)
    float plpPeakEma_ = 0.0f;                // EMA of PLP peak amplitudes (beat stability tracking)
    float beatStability_ = 0.0f;              // Current PLP peak / peak EMA (0=disrupted, 1=locked)
    uint8_t plpBestSource_ = 0;                // 0=flux, 1=bass, 2=nn (which source won)
    uint16_t beatCount_ = 0;                    // Beat counter (increments on phase wrap)

    // === Onset picking (shared by every detector, see OnsetDetector.h) ===
    // Outputs never know which detector ran: control_.onset is the strength
    // (0-1 activation at the peak), control_.pulse the envelope of the
    // activation, onset density the count of picked onsets.
    float odfPeakHold_ = 0.0f;        // Peak-hold of spectral flux: energy synthesis + presence gate
    float lastPulseStrength_ = 0.0f;  // Max pulse fired by any frame processed in the last update()
    float lastFramePulse_ = 0.0f;     // Pulse fired by the most recent frame
    FrameHook frameHook_ = nullptr;
    void* frameHookCtx_ = nullptr;
    uint32_t lastPulseFrame_ = 0;     // frameClock_ of the last onset (minimum gap W5, rhythm presence)
    // T1.4/T1.5 — per-onset diagnostics captured by pickOnset at every onset
    // (either detector). Consumed by the TRANSIENT JSON emitter in
    // blinky-things.ino for offline FP/TP class clustering.
    uint8_t lastPulseGateMask_ = 0;
    PulseFeatureSnapshot lastPulseFeatures_ = {0};

    // ODF of the last fresh frames, newest first ([0] = t-1). The picker's
    // max and moving-average windows read it.
    float odfHistory_[ONSET_HISTORY_MAX] = {0};
    float onsetActivation_ = 0.0f;     // Activation of the last fresh frame (getOnsetActivation)
    float activationPeak_ = 0.0f;      // Max activation over frames processed in the last update()

    // Epoch-fold linearized buffer (raw, not mean-subtracted — for pattern extraction)
    float epochFoldLinear_[OSS_BUFFER_SIZE] = {0};
    uint32_t lastFrameEndSampleIdx_ = 0;  // See getLastFrameEndSampleIdx()
    bool haveFrameEnd_ = false;           // lastFrameEndSampleIdx_ is a real frame end (PhaseClock)

    // === Onset density ===
    float onsetDensity_ = 0.0f;
    int onsetCountInWindow_ = 0;
    uint32_t onsetDensityWindowStart_ = 0;

    // === Pulse envelope (driven by the onset activation) ===
    float pulseEnvelope_ = 0.0f;     // Decaying envelope triggered by onset events

    // === Frame clock ===
    uint32_t frameClock_ = 0;         // Spectral frames processed since boot
    int framesThisUpdate_ = 0;        // Frames processed by the last update() (0 = no new audio)
    int framesSinceAcf_ = 0;          // ACF/PLP analysis cadence is counted in frames

    // === Smoothed levels (gating must not sample one instant) ===
    float levelEnv_ = 0.0f;           // ~1 s EMA of mic normalized level (PLP confidence gate)

    // === ACF timing ===
    uint32_t lastAcfDurationMs_ = 0;  // Timing: last runAcf()+updatePlpAnalysis() duration
    uint32_t lastPlpMs_ = 0;       // Timing: last updatePlpAnalysis() duration


    // === Pattern slot cache (v82) ===
    PatternSlot slots_[SLOT_COUNT] = {};
    float currentDigest_[SLOT_BINS] = {0};
    int activeSlot_ = -1;
    uint16_t lastSlotCheckBeat_ = 0;
    float prevBpm_ = 120.0f;          // For BPM shift detection

    // === Scratch buffers (loop-task only) ===
    // The Arduino loop task has a 4 KB stack (core LOOP_STACK_SZ = 256*4
    // words) with FreeRTOS overflow checking on. runAcf/updatePlpAnalysis/
    // checkPatternSlots used to put up to four 1 KB arrays on it at once —
    // a stack overflow there is a crash loop on a sealed device. Members
    // cost static RAM (plenty free) instead of stack. Never used re-entrantly:
    // all callers run sequentially inside update() on the loop task.
    float scratchFold_[MAX_PATTERN_LEN];  // runAcf folds, checkPatternSlots resample
    float scratchRaw_[MAX_PATTERN_LEN];   // updatePlpAnalysis patternRaw
    float scratchVar_[MAX_PATTERN_LEN];   // updatePlpAnalysis perBinVariance
    float scratchNN_[MAX_PATTERN_LEN];    // updatePlpAnalysis NN fold

    // === Output ===
    AudioControl control_;

    // === Internal methods ===
    // Everything that must run exactly once per spectral frame. elapsedFrames:
    // real time the frame covers on the sample clock (1, more after an overrun).
    // mayRunExpensive: this frame may run the detector's expensive work.
    void processFrame(bool mayRunExpensive, float elapsedFrames);
    void addOssSample(float ungatedFlux);
    void addBassSample(float bassEnergy);
    void runAcf();
    float pickOnset(const OnsetEvidence& ev, const OnsetDetectorParams& params);  // One frame; returns onset strength (0 = none)
    OnsetDetectorParams detectorParams() const;
    void updatePlpAnalysis();       // Epoch-fold + bass ACF + cross-correlate (ACF cadence)
    void advancePlpPhase(float frames);  // Advance phase by `frames` + read pattern value
    float patternValueAt(float phase) const;  // accent-aligned PLP pattern, interpolated
    void correctPlpPhase(float targetPhase, float gain);
    void countBeat();
    void synthesizeOutputs(float dt, uint32_t nowMs);

    // Pattern slot cache methods
    void checkPatternSlots();
    int allocateSlot();
    static void resamplePattern(const float* src, int srcLen, float* dst, int dstLen);
    static float cosineSimilarity(const float* a, const float* b, int len);

};
