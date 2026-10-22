#include "AudioTracker.h"
#include "../types/BlinkyAssert.h"
#include "HostCostHook.h"
#include <math.h>
#include <string.h>

// Constants moved to AudioTracker public members (v74) for tuning via serial console.
// Previous hardcoded values: ODF_CONTRAST=2.0, PULSE_THRESHOLD_MULT=2.0,
// PULSE_MIN_LEVEL=0.03, PULSE_ONSET_FLOOR=0.1

static constexpr float TWO_PI_F = 6.28318530718f;

// Cold-start template bank: 8 common rhythmic patterns (16 bins = 16th-note resolution).
// Used ONLY for 1-bar seeding during cold start — not for runtime matching or caching.
// ~512 bytes flash. Each pattern normalized to sum ~1.0.
static constexpr int NUM_SEED_TEMPLATES = 8;
static constexpr int SEED_TEMPLATE_BINS = 16;
static_assert(SLOT_BINS == SEED_TEMPLATE_BINS, "SLOT_BINS must match SEED_TEMPLATE_BINS for cosine similarity");
static const float SEED_TEMPLATES[NUM_SEED_TEMPLATES][SEED_TEMPLATE_BINS] = {
    {0.25f,0.0f,0.0f,0.0f, 0.25f,0.0f,0.0f,0.0f, 0.25f,0.0f,0.0f,0.0f, 0.25f,0.0f,0.0f,0.0f},  // 4otf
    {0.20f,0.0f,0.0f,0.0f, 0.30f,0.0f,0.0f,0.0f, 0.20f,0.0f,0.0f,0.0f, 0.30f,0.0f,0.0f,0.0f},  // backbeat
    {0.35f,0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f,0.0f,  0.35f,0.0f,0.0f,0.0f, 0.15f,0.0f,0.0f,0.15f}, // halftime
    {0.20f,0.0f,0.0f,0.15f,0.0f,0.0f,0.20f,0.0f,  0.10f,0.0f,0.0f,0.0f, 0.15f,0.0f,0.20f,0.0f}, // breakbeat
    {0.15f,0.0f,0.10f,0.0f,0.15f,0.0f,0.10f,0.0f,  0.15f,0.0f,0.10f,0.0f,0.15f,0.0f,0.10f,0.0f}, // 8th note
    {0.25f,0.0f,0.10f,0.0f,0.0f,0.0f,0.0f,0.0f,   0.30f,0.0f,0.0f,0.10f,0.0f,0.0f,0.15f,0.10f}, // dnb
    {0.20f,0.0f,0.0f,0.15f,0.0f,0.0f,0.20f,0.0f,  0.0f,0.0f,0.0f,0.15f, 0.20f,0.0f,0.0f,0.10f}, // dembow
    {0.50f,0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f,0.0f,  0.10f,0.0f,0.0f,0.0f, 0.20f,0.0f,0.0f,0.20f},  // sparse
};

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ============================================================================
// Construction / Lifecycle
// ============================================================================

AudioTracker::AudioTracker(IPdmMic& pdm, ISystemTime& time)
    : time_(time)
    , mic_(pdm, time)
    , spectral_()
{
}

AudioTracker::~AudioTracker() {
    end();
}

bool AudioTracker::begin(uint32_t sampleRate) {
    if (!mic_.begin(sampleRate)) return false;
    spectral_.begin();
    haveFrameEnd_ = false;

    // Onset detector selection (OnsetDetector.h).
#if BLINKY_ONSET_NN_ENABLED
    const bool nnOk = nnDetector_.begin();
    // NN init failure is a build-time / model-export bug, not a runtime
    // condition. Assert loudly (serial + error counter) so the build that
    // falls back to spectral flux is never mistaken for an NN build.
    BLINKY_ASSERT(nnOk,
        "FrameOnsetNN failed to initialize — check getInitError(); onsets fall back to spectral flux");
    detector_ = nnOk ? static_cast<OnsetDetector*>(&nnDetector_) : &fluxDetector_;
#  ifdef ONSET_MODEL_USE_PCEN
    if (nnOk) nnDetector_.model().setPcenEnabled(true);
#  endif
#else
    // Disabled by design (see BLINKY_ONSET_NN_ENABLED in AudioTracker.h):
    // onsets come from spectral flux. Not a fallback; `show nn` says so.
    detector_ = &fluxDetector_;
#endif

    // Initialize all time-based state to current time to avoid
    // instant triggers on first update (nowMs >> 0 would fire immediately)
    uint32_t now = time_.millis();
    onsetDensityWindowStart_ = now;
    resetSlots();

    return true;
}

void AudioTracker::end() {
    mic_.end();
}

int AudioTracker::getHwGain() const {
    return mic_.getHwGain();
}

void AudioTracker::resetSlots() {
    for (int i = 0; i < SLOT_COUNT; i++) {
        memset(slots_[i].bins, 0, sizeof(slots_[i].bins));
        slots_[i].confidence = 0.0f;
        slots_[i].totalBars = 0;
        slots_[i].age = 0;
        slots_[i].valid = false;
        slots_[i].seeded = false;
    }
    memset(currentDigest_, 0, sizeof(currentDigest_));
    activeSlot_ = -1;
    lastSlotCheckBeat_ = 0;
}

uint32_t AudioTracker::discardSetupBacklog() {
    const uint32_t dropped = mic_.discardBacklog();
    haveFrameEnd_ = false;  // the first frame after this starts the sample clock afresh
    return dropped;
}

// ============================================================================
// Main Update Loop
// ============================================================================

const AudioControl& AudioTracker::update(float dt) {
    uint32_t nowMs = time_.millis();

    // 1. Mic input
    mic_.update(dt);

    // Per-update aggregates: update() may process 0..N spectral frames.
    lastPulseStrength_ = 0.0f;
    activationPeak_ = 0.0f;
    framesThisUpdate_ = 0;

    // 2. Drain the PDM ring, processing EVERY spectral frame exactly once.
    //
    // The spectral frame (62.5 Hz) is the only clock for rhythm analysis.
    // Each frame runs the full chain in processFrame(): NN inference, pulse
    // detection, OSS/bass/NN buffers, ACF cadence, PLP phase step. The main
    // loop's rate only decides how many frames are handled per call.
    //
    // History: pre-b208 only the LAST frame of a catch-up burst reached the
    // NN / OSS / pulse detector, and the PLP phase advanced once per loop
    // iteration. With the loop at ~43 fps a third of all frames were
    // invisible to tempo + onset detection, the NN's input window held
    // frames unevenly spaced in time (unlike training), and phase drifted
    // at loopFps/62.5 of the true tempo.
    //
    // Reads are sized with samplesNeeded() so the accumulator never
    // discards samples (windows stay contiguous).
    static int16_t sampleBuffer[SpectralConstants::FFT_SIZE];  // static: avoid stack churn
    // A full mic ring holds this many frames; draining that many per call
    // means a stall never leaves backlog that would later overrun.
    constexpr int MAX_DRAIN_ITERS = 8;
    // Drain only the samples already waiting when update() starts. Samples
    // that arrive while we process (one NN inference is ~12 ms, close to the
    // 16 ms frame period) wait for the next update(). Chasing them was the
    // b208-b210 livelock: every frame looked like the newest, every frame
    // ran the NN, and update() returned only after MAX_DRAIN_ITERS frames,
    // starving the renderer (hat_v1 b210: 12 fps, 62.4 inferences/s).
    uint32_t samplesBudget = mic_.getPendingSamples();
    for (int drainIter = 0; drainIter < MAX_DRAIN_ITERS && samplesBudget > 0; drainIter++) {
        uint32_t overrunsBefore = AdaptiveMic::getOverrunCount();
        uint32_t want = static_cast<uint32_t>(spectral_.samplesNeeded());
        if (want > samplesBudget) want = samplesBudget;
        int samplesRead = mic_.getSamplesForExternal(sampleBuffer, static_cast<int>(want));
        samplesBudget -= static_cast<uint32_t>(samplesRead);
        if (AdaptiveMic::getOverrunCount() != overrunsBefore) {
            spectral_.markDiscontinuity();  // samples were dropped inside this window
        }
        if (samplesRead == 0) break;  // ring empty
        spectral_.addSamples(sampleBuffer, samplesRead);
        if (!spectral_.hasSamples()) break;  // partial window — rest arrives next update()
        spectral_.process();
        BLINKY_HOST_COST(HostCost::FRAME);
        // Absolute mic-sample index at which this window ended. Shared
        // timeline with the record stream (RecordStream).
        const uint32_t frameEnd = mic_.getExternalReadIndex();
        // Real time this frame covers on the sample clock (#261): one frame
        // normally, more when an overrun dropped samples inside it. The
        // phase advances by this, so dropped audio doesn't put the beat
        // behind real time.
        float elapsedFrames = 1.0f;
        if (haveFrameEnd_) {
            const uint32_t span = frameEnd - lastFrameEndSampleIdx_;
            // A span beyond any real stall means the index jumped (mic
            // restart): count it as one frame, and say so.
            BLINKY_ASSERT_ONCE(span <= PhaseClock::MAX_FRAME_SPAN_SAMPLES,
                "AudioTracker: frame span beyond any real stall (mic sample index jumped)");
            if (span <= PhaseClock::MAX_FRAME_SPAN_SAMPLES) {
                elapsedFrames = PhaseClock::framesFromSamples(span);
            }
        }
        lastFrameEndSampleIdx_ = frameEnd;
        haveFrameEnd_ = true;
        // CPU guard: run the NN only on the last complete frame of this
        // update's budget. Every frame is still pushed into the NN window, so
        // its input stays evenly spaced; NN cost per update() is at most one
        // inference. The budget is fixed at entry, so arrivals during the
        // inference can't turn the next frame into "the newest" again.
        bool moreFramesPending =
            samplesBudget >= static_cast<uint32_t>(SpectralConstants::FFT_SIZE);
        processFrame(!moreFramesPending || drainIter == MAX_DRAIN_ITERS - 1, elapsedFrames);
        framesThisUpdate_++;
    }

    // 3. No silence detection. Every detector tried (relative level, a
    // fixed absolute gate, an adaptive noise floor) either reset tempo in
    // breakdowns / dense music or swallowed quiet music on this weak mic.
    // On edm/ at device-like levels the adaptive floor reset 487-2159 times
    // in music; never resetting gave 0 false resets and re-locked on a new
    // song as fast or faster (2.1-12.0 s vs 2.7-12.3 s). When music stops,
    // rhythm confidence falls as the analysis buffers fill with room noise.

    // Smoothed normalized level for PLP confidence gating (~1 s EMA). A
    // single instantaneous sample often lands in the sidechain dip between
    // kicks and reads ~0, repeatedly collapsing confidence.
    {
        float a = 1.0f - expf(-dt / 1.0f);
        levelEnv_ += a * (mic_.getLevel() - levelEnv_);
    }

    // 4. Pattern slot cache: check every bar (4 beats)
    if ((beatCount_ & 3) == 0 && beatCount_ != lastSlotCheckBeat_) {
        lastSlotCheckBeat_ = beatCount_;
        if (plpConfidence_ > 0.1f) {
            checkPatternSlots();
        }
    }

    // 5. Output synthesis
    synthesizeOutputs(dt, nowMs);

    return control_;
}

// ============================================================================
// Per-frame processing (the frame clock)
// ============================================================================

void AudioTracker::processFrame(bool mayRunExpensive, float elapsedFrames) {
    frameClock_++;

    // --- Onset evidence for this frame (the detector in use) ---
    // Only onset detection reads it; the rhythm buffers below take bass flux,
    // bass energy and mic level whichever detector runs (#155).
    const OnsetDetectorParams params{pulseThresholdMult, fluxOnsetFloor, baselineFastDrop,
                                     baselineSlowRise, pulseOnsetFloor, nnProfile};
    const OnsetEvidence ev = detector_->process(spectral_, params, mayRunExpensive);
    if (ev.fresh) {
        onsetActivation_ = ev.activation;
        if (ev.activation > activationPeak_) activationPeak_ = ev.activation;
    }

    // --- Discrete onset: shared peak picking and gates ---
    float pulse = pickOnset(ev);
    lastFramePulse_ = pulse;
    if (pulse > lastPulseStrength_) lastPulseStrength_ = pulse;
    if (pulse > 0.0f) onsetCountInWindow_++;  // onset density counts detected onsets

    // --- Rhythm buffers ---
    // Bass-only flux for ACF period detection — isolates kick energy.
    // Hi-hats/snares in the high band create non-periodic ACF peaks that
    // overwhelm the kick pattern (especially in breakbeat/DnB).
    float bassFlux = spectral_.getBassFlux();
    float bassContrast;
    if (odfContrast == 2.0f) {
        bassContrast = bassFlux * bassFlux;
    } else if (odfContrast == 1.0f) {
        bassContrast = bassFlux;
    } else if (odfContrast == 0.5f) {
        bassContrast = sqrtf(bassFlux);
    } else {
        bassContrast = powf(bassFlux, odfContrast);
    }
    bassContrast = clampf(bassContrast, 0.0f, 1.0f);

    addOssSample(bassContrast);

    // Bass energy (PLP dual-source AND energy synthesis). Mel bands 0-5
    // cover the bass region (mel band indices, not FFT bins).
    const float* mel = spectral_.getMelBands();  // member array, never null
    cachedBassEnergy_ = 0.0f;
    for (int i = 0; i <= 5; i++) cachedBassEnergy_ += mel[i];
    cachedBassEnergy_ /= 6.0f;
    addBassSample(cachedBassEnergy_);

    // Tempo/phase source 2 is the mic level in BOTH modes, never the NN
    // activation. The NN fires on every percussive hit, off-beat hats
    // included, so as an ACF source it locked straight 4/4 to 2/3 tempo
    // and broke DnB (#155: corpus tempo 0.726 -> 0.621). The NN still drives
    // onset detection (pickOnset); tempo tracking stays on the
    // signals it was tuned on.
    nnOnsetBuffer_[nnWriteIdx_] = mic_.getLevel();
    nnWriteIdx_ = (nnWriteIdx_ + 1) % NN_BUFFER_SIZE;
    if (nnCount_ < NN_BUFFER_SIZE) nnCount_++;

    // --- Phase step for THIS frame (before analysis, so a correction made
    // below refers to the frame just added — the newest bin of the fold) ---
    advancePlpPhase(elapsedFrames);

    // --- ACF period detection + PLP pattern analysis, cadence in frames ---
    // Warmup: 100 frames (1.6 s); ACF_MAX_LAG (80) is the hard minimum.
    framesSinceAcf_++;
    int acfPeriodFrames = static_cast<int>(acfPeriodMs * OSS_FRAME_RATE / 1000.0f + 0.5f);
    if (acfPeriodFrames < 1) acfPeriodFrames = 1;
    if (framesSinceAcf_ >= acfPeriodFrames && ossCount_ >= 100) {
        framesSinceAcf_ = 0;
        uint32_t t0 = time_.millis();
        runAcf();
        uint32_t t1 = time_.millis();
        updatePlpAnalysis();
        uint32_t t2 = time_.millis();
        lastAcfDurationMs_ = t1 - t0;  // Profile: ACF only
        lastPlpMs_ = t2 - t1;          // Profile: PLP analysis only
    }

    if (frameHook_) frameHook_(frameHookCtx_);
}

// ============================================================================
// OSS Buffer
// ============================================================================

void AudioTracker::addOssSample(float ungatedFlux) {
    ossBuffer_[ossWriteIdx_] = ungatedFlux;
    ossWriteIdx_ = (ossWriteIdx_ + 1) % OSS_BUFFER_SIZE;
    if (ossCount_ < OSS_BUFFER_SIZE) ossCount_++;
}

// ============================================================================
// Multi-Source ACF — Period Detection + Epoch-Fold Pattern Scoring
// ============================================================================
// Replaces the Fourier Tempogram (Goertzel DFT, 75ms) with autocorrelation
// (~4ms). ACF scans beat-level lags (20-80) on 3 sources, then generates
// bar-level candidates via integer multiples (2×/3×/4×). Epoch-fold variance
// scoring selects the period with the best pattern contrast, biasing toward
// full-bar patterns over single-beat periods.

void AudioTracker::runAcf() {
    // Linearize all source circular buffers into class members.
    int startIdx = (ossWriteIdx_ - ossCount_ + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
    for (int i = 0; i < ossCount_; i++) {
        int idx = (startIdx + i) % OSS_BUFFER_SIZE;
        ossLinear_[i] = ossBuffer_[idx];
    }
    int bStart = (bassWriteIdx_ - bassCount_ + BASS_BUFFER_SIZE) % BASS_BUFFER_SIZE;
    for (int i = 0; i < bassCount_; i++) bassLinear_[i] = bassBuffer_[(bStart + i) % BASS_BUFFER_SIZE];
    int nStart = (nnWriteIdx_ - nnCount_ + NN_BUFFER_SIZE) % NN_BUFFER_SIZE;
    for (int i = 0; i < nnCount_; i++) nnLinear_[i] = nnOnsetBuffer_[(nStart + i) % NN_BUFFER_SIZE];

    // Mean-subtract each source IN-PLACE (removes DC for clean ACF/epoch-fold).
    // NOTE: updatePlpAnalysis() reads these mean-subtracted buffers.
    float* sources[3] = { ossLinear_, bassLinear_, nnLinear_ };
    const int sourceCounts[3] = { ossCount_, bassCount_, nnCount_ };

    for (int src = 0; src < 3; src++) {
        int count = sourceCounts[src];
        if (count < 20) continue;
        float mean = 0.0f;
        for (int i = 0; i < count; i++) mean += sources[src][i];
        mean /= count;
        for (int i = 0; i < count; i++) sources[src][i] -= mean;
    }
    // --- Pass 1: Multi-source ACF at beat-level lags ---
    // Scan lags 20-80 (200-49 BPM beat rate) with step=2.
    // Bar-level candidates are generated from multiples, not scanned directly.
    // Cost: ~31 lags × 3 sources × ~700 overlap ≈ 65K MACs (~4ms on M4F).
    // Parabolic interpolation on peaks recovers sub-step resolution (odd lags).
    static constexpr int TOP_N = 5;
    static constexpr int ACF_MIN_LAG = 20;    // ~200 BPM beat rate
    static constexpr int ACF_MAX_LAG = 80;    // ~49 BPM beat rate
    static constexpr int ACF_STEP = 2;

    struct AcfPeak {
        float strength;   // Normalized ACF value at peak (0-1)
        int period;
        int source;
    };
    AcfPeak beatPeaks[TOP_N] = {};

    for (int src = 0; src < 3; src++) {
        int count = sourceCounts[src];
        if (count < ACF_MAX_LAG * 2) continue;
        const float* buf = sources[src];

        // Zero-lag energy for normalization (unbiased ACF)
        float zeroLag = 0.0f;
        for (int i = 0; i < count; i++) zeroLag += buf[i] * buf[i];
        if (zeroLag < 1e-10f) continue;

        // Scan ACF with local-maximum peak detection.
        // Extend one step past ACF_MAX_LAG so a peak AT the boundary is detected
        // (the 3-point detector looks one step back).
        int scanEnd = min(ACF_MAX_LAG + ACF_STEP, count / 2 - 1);
        float prevAcf = 0.0f, prevPrevAcf = 0.0f;
        for (int lag = ACF_MIN_LAG; lag <= scanEnd; lag += ACF_STEP) {
            float sum = 0.0f;
            int overlap = count - lag;
            for (int i = 0; i < overlap; i++) {
                sum += buf[i] * buf[i + lag];
            }
            float acfVal = sum / zeroLag;

            // Peak: previous sample was a local maximum above threshold
            // Only record if the peak lag is within the valid scan range
            if (prevAcf > prevPrevAcf && prevAcf > acfVal && prevAcf > 0.05f
                && (lag - ACF_STEP) >= ACF_MIN_LAG && (lag - ACF_STEP) <= ACF_MAX_LAG) {
                int peakLag = lag - ACF_STEP;
                float peakStrength = prevAcf;

                // Parabolic interpolation: refine peak position and strength
                // from 3 equidistant ACF samples (step=2 → recovers odd lags)
                float denom = prevPrevAcf - 2.0f * prevAcf + acfVal;
                if (fabsf(denom) > 1e-10f) {
                    float delta = 0.5f * (prevPrevAcf - acfVal) / denom;
                    if (delta > -1.0f && delta < 1.0f) {
                        int refined = peakLag + static_cast<int>(roundf(delta * ACF_STEP));
                        if (refined >= ACF_MIN_LAG && refined <= ACF_MAX_LAG) {
                            peakLag = refined;
                        }
                        peakStrength = prevAcf - 0.25f * (prevPrevAcf - acfVal) * delta;
                    }
                }

                // Insert into top-N with 10% minimum period separation
                bool tooClose = false;
                for (int k = 0; k < TOP_N; k++) {
                    if (beatPeaks[k].strength > 0.01f &&
                        abs(peakLag - beatPeaks[k].period) <= beatPeaks[k].period / 10) {
                        if (peakStrength > beatPeaks[k].strength) {
                            beatPeaks[k] = { peakStrength, peakLag, src };
                        }
                        tooClose = true;
                        break;
                    }
                }
                if (!tooClose) {
                    int minIdx = 0;
                    for (int k = 1; k < TOP_N; k++) {
                        if (beatPeaks[k].strength < beatPeaks[minIdx].strength) minIdx = k;
                    }
                    if (peakStrength > beatPeaks[minIdx].strength) {
                        beatPeaks[minIdx] = { peakStrength, peakLag, src };
                    }
                }
            }
            prevPrevAcf = prevAcf;
            prevAcf = acfVal;
        }
    }

    // --- Generate bar-level candidates from beat-peak multiples ---
    // Bar patterns are 2×/3×/4× the beat period. Epoch-fold variance scoring
    // selects bar-length periods over beat-length when the pattern is structured
    // (e.g., kick-snare-kick-snare has higher variance at 4× than at 1×).
    static constexpr int MULTIPLIERS[] = { 1, 2, 3, 4 };
    static constexpr int NUM_MULTIPLIERS = 4;
    // Syncopation-derived candidates (#166): per beat peak, 2 beat hypotheses
    // (L*2/3, L*2/5) x 2 bar multipliers (2x, 4x).
    static constexpr int SYNC_FRACS = 2;
    static constexpr int SYNC_MULTS = 2;
    static constexpr int MAX_CANDIDATES = TOP_N * (NUM_MULTIPLIERS + SYNC_FRACS * SYNC_MULTS);

    struct ScoredCandidate {
        float acfStrength;
        int period;
        int source;
        int multiplier;   // 1=beat, 2/3/4=bar-level
        bool derived;     // syncopation-derived (#166): score scaled by syncopatedCandidateScale
    };
    static_assert(MAX_CANDIDATES == TOP_N * (NUM_MULTIPLIERS + SYNC_FRACS * SYNC_MULTS),
                  "MAX_CANDIDATES invariant");
    ScoredCandidate candidates[MAX_CANDIDATES] = {};
    int numCandidates = 0;

    for (int k = 0; k < TOP_N; k++) {
        if (beatPeaks[k].strength < 0.01f) continue;
        for (int m = 0; m < NUM_MULTIPLIERS; m++) {
            int cPeriod = beatPeaks[k].period * MULTIPLIERS[m];
            if (cPeriod > MAX_PATTERN_LEN) continue;
            if (cPeriod >= ossCount_ / 2) continue;  // Need 2+ epochs

            // Skip near-duplicates
            bool dup = false;
            for (int c = 0; c < numCandidates; c++) {
                if (abs(cPeriod - candidates[c].period) <= cPeriod / 10) { dup = true; break; }
            }
            if (dup) continue;

            if (numCandidates < MAX_CANDIDATES) {
                candidates[numCandidates++] = { beatPeaks[k].strength, cPeriod, beatPeaks[k].source, MULTIPLIERS[m], false };
            }
        }
    }

    // --- Syncopation-derived bar candidates (#166) ---
    // Breakbeat / DnB / 2-step accent patterns (kick on 1 and the "and" of 3,
    // snare on 2 and 4) put the strongest beat-range ACF peaks at 1.5 and 2.5
    // beats, while the true beat lag barely shows up in any source. The bar
    // (4 beats) still correlates strongly. So from each beat peak L derive the
    // beat hypotheses b = 2L/3 (L was 1.5 beats) and 2L/5 (L was 2.5 beats), and
    // add bar-level candidates at 2b and 4b scored with the ACTUAL ACF at that
    // lag, not the parent peak's. Wrong hypotheses get a weak ACF and lose in
    // Pass 2 on their own; nothing is forced. Cost: <= TOP_N*4 single-lag ACFs.
    if (syncopatedCandidateScale > 0.0f) {
        static constexpr int FRAC_NUM[SYNC_FRACS] = { 2, 2 };
        static constexpr int FRAC_DEN[SYNC_FRACS] = { 3, 5 };
        static constexpr int SYNC_MULT[SYNC_MULTS] = { 2, 4 };
        for (int k = 0; k < TOP_N; k++) {
            if (beatPeaks[k].strength < 0.01f) continue;
            const int src = beatPeaks[k].source;
            const float* buf = sources[src];
            const int count = sourceCounts[src];
            float zero = 0.0f;
            for (int i = 0; i < count; i++) zero += buf[i] * buf[i];
            if (zero < 1e-10f) continue;
            const float zeroMean = zero / static_cast<float>(count);
            for (int f = 0; f < SYNC_FRACS; f++) {
                // b is rounded BEFORE the bar multiplier, so 4-beat candidates
                // sit on multiples of 4 frames and can miss the exact bar peak
                // by up to 2 frames (L=56, 2/5 -> 88, true 90). Rounding once
                // after both factors was measured on edm/: tempo 0.729 ->
                // 0.735 but phase lock (excess) 0.094 -> 0.091 (more tracks
                // lock to bar cycles, which phase-lock worse). Phase lock drives
                // the visuals, so the coarser rounding stays until bar-level
                // phase alignment itself improves.
                const int b = (beatPeaks[k].period * FRAC_NUM[f] + FRAC_DEN[f] / 2) / FRAC_DEN[f];
                if (b < ACF_MIN_LAG) continue;
                for (int m = 0; m < SYNC_MULTS; m++) {
                    const int cPeriod = b * SYNC_MULT[m];
                    if (cPeriod > MAX_PATTERN_LEN || cPeriod >= count / 2) continue;
                    // Dedupe within this source only: the same period from a
                    // weak source (e.g. a flat bass ACF) must not hide the
                    // strong-source version, which is the whole point here.
                    //
                    // Same-source ordinary candidates DO suppress a derived one
                    // at the same period, deliberately. Letting derived bar
                    // candidates bypass them was measured on edm/: tempo
                    // 0.729 -> 0.748 but phase lock (excess) 0.094 -> 0.087,
                    // because more tracks then lock to bar-length cycles.
                    // Phase lock is what the visuals animate on, so it wins.
                    bool dup = false;
                    for (int c = 0; c < numCandidates; c++) {
                        if (candidates[c].source == src &&
                            abs(cPeriod - candidates[c].period) <= cPeriod / 10) { dup = true; break; }
                    }
                    if (dup) continue;
                    // Unbiased normalized ACF at the bar lag.
                    const int overlap = count - cPeriod;
                    float sum = 0.0f;
                    for (int i = 0; i < overlap; i++) sum += buf[i] * buf[i + cPeriod];
                    const float acf = sum / static_cast<float>(overlap) / zeroMean;
                    if (acf < 0.05f) continue;
                    if (numCandidates < MAX_CANDIDATES) {
                        candidates[numCandidates++] = { clampf(acf, 0.0f, 1.0f), cPeriod, src, SYNC_MULT[m], true };
                    }
                }
            }
        }
    }

    // --- Pass 2: Score candidates by epoch-fold pattern quality ---
    // Reuses tempogram Pass 2 logic: the period that produces the highest-contrast
    // pattern is the one that creates the best visual pulse. Each candidate is
    // scored using its own source buffer for fair cross-source comparison.
    float bestScore = -1.0f;
    float bestStrength = 0.0f;
    int bestPeriod = static_cast<int>(OSS_FRAMES_PER_MIN / 120.0f);
    float bestAccentBin = 0.0f;
    int bestSource = 0;
    int bestMultiplier = 1;
    bool bestDerived = false;

    for (int c = 0; c < numCandidates; c++) {
        int cPeriod = candidates[c].period;
        int cSource = candidates[c].source;
        const float* cBuf = sources[cSource];
        int cCount = sourceCounts[cSource];

        if (cCount < cPeriod * 2) continue;

        // Recency-weighted epoch fold
        float* fold = scratchFold_;
        memset(fold, 0, cPeriod * sizeof(float));
        float totalWeight = 0.0f;
        int epochs = 0;
        for (int offset = cCount - cPeriod; offset >= 0; offset -= cPeriod) {
            float weight = expf(-plpDecayRate * epochs);  // ~3-epoch half-life
            for (int j = 0; j < cPeriod; j++) {
                fold[j] += cBuf[offset + j] * weight;
            }
            totalWeight += weight;
            epochs++;
        }
        if (epochs < 2) continue;
        for (int j = 0; j < cPeriod; j++) fold[j] /= totalWeight;

        // Pattern variance (contrast) — higher = more structured
        float mean = 0.0f;
        for (int j = 0; j < cPeriod; j++) mean += fold[j];
        mean /= cPeriod;
        float variance = 0.0f;
        for (int j = 0; j < cPeriod; j++) {
            float d = fold[j] - mean;
            variance += d * d;
        }
        variance /= cPeriod;

        // Combined score: ACF periodicity × pattern contrast.
        // Multiplier penalty: higher multipliers (2×/3×/4×) must produce
        // proportionally more variance to beat the base period. Without this,
        // 4× bar periods always win because they capture more structure.
        // Penalty: divide by sqrt(multiplier) — 2× needs 1.41× more contrast,
        // 4× needs 2× more contrast to win over the beat-level period.
        float multiplierPenalty = sqrtf(static_cast<float>(candidates[c].multiplier));
        float score = candidates[c].acfStrength * sqrtf(variance) / multiplierPenalty;
        if (candidates[c].derived) score *= syncopatedCandidateScale;

        // Tempo prior on the BEAT lag (Ellis 2007-style log-Gaussian around
        // 120 BPM). ACF peaks at 1.5x / 2x the true beat lag are often as
        // strong as the beat itself on syncopated material; multiplied up to
        // bar length they produced 6-8-beat "cycles". The prior breaks those
        // ties toward musically common tempi without forbidding any.
        if (tempoPriorWidth > 0.0f) {
            float beatLag = static_cast<float>(cPeriod) / static_cast<float>(candidates[c].multiplier);
            float oct = log2f(beatLag / (OSS_FRAMES_PER_MIN / 120.0f)) / tempoPriorWidth;
            score *= expf(-0.5f * oct * oct);
        }

        if (score > bestScore) {
            // Phase from epoch-fold peak position (where the main accent falls).
            // Parabolic interpolation on the 3 bins around the peak gives sub-frame
            // precision (~0.3% vs ~3% integer-bin at 120 BPM). Same technique used
            // in the ACF peak finder. Handles wrap-around for peaks at bin 0.
            int peakBin = 0;
            float peakVal = fold[0];
            for (int j = 1; j < cPeriod; j++) {
                if (fold[j] > peakVal) { peakVal = fold[j]; peakBin = j; }
            }
            // Parabolic interpolation for sub-frame phase
            float refinedBin = static_cast<float>(peakBin);
            if (cPeriod >= 3) {
                int prev = (peakBin - 1 + cPeriod) % cPeriod;  // wrap-around
                int next = (peakBin + 1) % cPeriod;
                float denom = fold[prev] - 2.0f * fold[peakBin] + fold[next];
                if (fabsf(denom) > 1e-6f) {
                    float delta = 0.5f * (fold[prev] - fold[next]) / denom;
                    delta = clampf(delta, -0.5f, 0.5f);
                    refinedBin += delta;
                    if (refinedBin < 0.0f) refinedBin += cPeriod;
                }
            }

            bestScore = score;
            bestStrength = candidates[c].acfStrength;
            bestPeriod = cPeriod;
            bestAccentBin = refinedBin;
            bestSource = cSource;
            bestMultiplier = candidates[c].multiplier;
            bestDerived = candidates[c].derived;
        }
    }

    acfPeakStrength_ = bestStrength;

    // Periodicity strength from ACF peak (already normalized 0-1)
    float newStrength = clampf(bestStrength, 0.0f, 1.0f);
    periodicityStrength_ = periodicityStrength_ * 0.5f + newStrength * 0.5f;

    // No candidate (silence / flat buffers): keep the last period and phase
    // rather than steering toward the default placeholder period.
    if (bestScore < 0.0f) return;

    // --- Fractional period ---
    // Candidate periods are integer frames (the fold needs integer bins), but
    // a real tempo almost never is: 124 BPM = 30.24 frames. Advancing phase
    // by 1/30 would gain ~1% per beat and rely on corrections to claw it
    // back. Refine the beat-level lag at FULL resolution (the scan used
    // step 2) with parabolic interpolation on the winning source, then
    // scale by the multiplier.
    //
    // Syncopation-derived winners (#166) are refined at their BAR lag instead
    // and not rescaled: they exist precisely because the beat-lag ACF peak is
    // weak or absent, so refining at bestPeriod / bestMultiplier would chase
    // an unrelated peak (or keep the rounded integer) and leave period error
    // in the phase advance.
    float periodF = static_cast<float>(bestPeriod);
    {
        const float* buf = sources[bestSource];
        const int count = sourceCounts[bestSource];
        const int refineMultiplier = bestDerived ? 1 : bestMultiplier;
        int lag = bestPeriod / refineMultiplier;
        // UNBIASED estimator (divide by overlap). The plain sum shrinks
        // linearly with lag, which tilts the peak toward shorter lags —
        // measured ~0.7% short (29.07 vs 29.30 frames at 128 BPM), a steady
        // phase drift the corrections had to keep fighting.
        auto acfAt = [&](int L) -> float {
            float sum = 0.0f;
            int overlap = count - L;
            for (int i = 0; i < overlap; i++) sum += buf[i] * buf[i + L];
            return (overlap > 0) ? sum / static_cast<float>(overlap) : 0.0f;
        };
        if (lag > 2 && lag + 1 < count / 2) {
            float a = acfAt(lag - 1), b = acfAt(lag), c = acfAt(lag + 1);
            // Hill-climb up to 2 steps if the step-2 scan landed beside the peak.
            for (int step = 0; step < 2; step++) {
                if (a > b && lag - 2 > 2) { c = b; b = a; lag--; a = acfAt(lag - 1); }
                else if (c > b && lag + 2 < count / 2) { a = b; b = c; lag++; c = acfAt(lag + 1); }
                else break;
            }
            float frac = 0.0f;
            float denom = a - 2.0f * b + c;
            if (b >= a && b >= c && denom < -1e-12f) {
                frac = clampf(0.5f * (a - c) / denom, -0.5f, 0.5f);
            }
            float lagF = static_cast<float>(lag) + frac;

            // Long-lag refinement. Onsets are quantized to whole frames, so
            // the ACF around one period is lopsided (e.g. 0 / .7 / .3 at lags
            // 28/29/30 for a true 29.3) and the parabola lands ~0.15 frame
            // short — a steady phase drift. The K-th repetition of the
            // period carries the same quantization error spread over K
            // periods: refine the peak near K·lagF and divide by K.
            int K = static_cast<int>((count / 2 - 2) / lagF);
            if (K > 12) K = 12;
            if (K >= 3) {
                int center = static_cast<int>(K * lagF + 0.5f);
                int halfWin = K / 2 + 1;  // ±0.5 frame/period uncertainty × K
                int bestL = center;
                float bestV = -1e30f;
                for (int L = center - halfWin; L <= center + halfWin; L++) {
                    if (L < 2 || L + 1 >= count) continue;
                    float v = acfAt(L);
                    if (v > bestV) { bestV = v; bestL = L; }
                }
                float la = acfAt(bestL - 1), lb = bestV, lc = acfAt(bestL + 1);
                float ld = la - 2.0f * lb + lc;
                float lfrac = 0.0f;
                if (lb >= la && lb >= lc && ld < -1e-12f) {
                    lfrac = clampf(0.5f * (la - lc) / ld, -0.5f, 0.5f);
                }
                float refined = (static_cast<float>(bestL) + lfrac) / static_cast<float>(K);
                // Accept only if it agrees with the short-lag estimate (a
                // different peak family at long lag would mean a tempo
                // change inside the buffer).
                if (fabsf(refined - lagF) < 0.75f) lagF = refined;
            }
            periodF = lagF * static_cast<float>(refineMultiplier);
            // The fold's integer period must stay the nearest integer.
            if (fabsf(periodF - static_cast<float>(bestPeriod)) > 1.5f) {
                periodF = static_cast<float>(bestPeriod);
            }
        }
    }

    // --- Accent position from the TRANSIENT source ---
    // The winning period may come from the bass-energy or NN buffer, but
    // those are smooth/level-like: with a long (bar-level) period there are
    // only ~3-4 epochs in the buffer, recency weighting favours the newest,
    // and any loudness trend puts the fold maximum at the newest frames —
    // i.e. the "accent" tracked "now" and phase hovered just below 1.0
    // (measured on techno-dub-groove: 30% of the time in 0.9-1.0). The OSS
    // buffer is half-wave-rectified bass flux (kick onsets, no trend), so
    // its fold peak is where the kick actually lands in the cycle.
    if (ossCount_ >= bestPeriod * 2) {
        float* fold = scratchFold_;
        memset(fold, 0, bestPeriod * sizeof(float));
        float totalWeight = 0.0f;
        int epochs = 0;
        for (int offset = ossCount_ - bestPeriod; offset >= 0; offset -= bestPeriod) {
            float weight = expf(-plpDecayRate * epochs);
            for (int j = 0; j < bestPeriod; j++) fold[j] += ossLinear_[offset + j] * weight;
            totalWeight += weight;
            epochs++;
        }
        if (epochs >= 2 && totalWeight > 0.0f) {
            int peakBin = 0;
            float peakVal = fold[0];
            float mean = 0.0f;
            for (int j = 0; j < bestPeriod; j++) {
                mean += fold[j];
                if (fold[j] > peakVal) { peakVal = fold[j]; peakBin = j; }
            }
            mean /= bestPeriod;
            // Only trust a fold with a real peak (flat fold = no kick structure).
            if (peakVal > mean + 1e-6f) {
                float refined = static_cast<float>(peakBin);
                int prev = (peakBin - 1 + bestPeriod) % bestPeriod;
                int next = (peakBin + 1) % bestPeriod;
                float denom = fold[prev] - 2.0f * fold[peakBin] + fold[next];
                if (fabsf(denom) > 1e-9f) {
                    refined += clampf(0.5f * (fold[prev] - fold[next]) / denom, -0.5f, 0.5f);
                    if (refined < 0.0f) refined += bestPeriod;
                }
                bestAccentBin = refined;
            }
        }
    }

    // A period jump > 10% means a different rhythm interpretation: snap phase.
    bool periodJump = !phaseLocked_ ||
                      abs(bestPeriod - plpBestPeriod_) > plpBestPeriod_ / 10;

    plpBestPeriod_ = bestPeriod;
    plpPeriodF_ = periodF;
    plpBestSource_ = static_cast<uint8_t>(bestSource);
    plpAccentBin_ = bestAccentBin;
    plpAccentPhase_ = bestAccentBin / static_cast<float>(bestPeriod);  // diagnostic

    // --- Phase from the fold ---
    // The fold is END-ALIGNED: bin P-1 holds the newest frame (the one just
    // processed). So the time since the accent is (P-1 - accentBin) frames,
    // which gives the phase of "now" directly. (Pre-b208 the code treated
    // the accent bin as a fixed offset from a free-running counter, but it
    // rotates by one bin per frame as the buffer advances — phase ran ~2x
    // tempo and jumped every analysis.)
    float framesSinceAccent = static_cast<float>(bestPeriod - 1) - bestAccentBin;
    if (framesSinceAccent < 0.0f) framesSinceAccent += static_cast<float>(bestPeriod);
    float targetPhase = framesSinceAccent / periodF;
    targetPhase -= floorf(targetPhase);
    correctPlpPhase(targetPhase, periodJump ? 1.0f : plpPhaseGain);
    phaseLocked_ = true;

    // BPM from the fractional period (informational + pulse cooldown)
    // Always smoothed toward the estimate. (Was: only updated when the
    // change exceeded 5%, so the reported BPM could sit up to 5% stale
    // indefinitely — e.g. 123 BPM displayed for a 128 BPM track.)
    float newBpm = OSS_FRAMES_PER_MIN / periodF;
    newBpm = clampf(newBpm, bpmMin, bpmMax);
    bpm_ = bpm_ * tempoSmoothing + newBpm * (1.0f - tempoSmoothing);

    // Invalidate pattern slots on significant BPM change (>15%)
    if (prevBpm_ > 0.0f && fabsf(bpm_ - prevBpm_) / prevBpm_ > 0.15f) {
        resetSlots();
    }
    prevBpm_ = bpm_;
}

// ============================================================================
// PLP (Predominant Local Pulse) — Dual-Source Pattern Extraction
// ============================================================================

void AudioTracker::addBassSample(float bassEnergy) {
    bassBuffer_[bassWriteIdx_] = bassEnergy;
    bassWriteIdx_ = (bassWriteIdx_ + 1) % BASS_BUFFER_SIZE;
    if (bassCount_ < BASS_BUFFER_SIZE) bassCount_++;
}

void AudioTracker::updatePlpAnalysis() {
    // --- 1. Use raw period from ACF (not BPM-smoothed) ---
    int patLen = plpBestPeriod_;
    if (patLen < 2) patLen = 2;
    if (patLen > MAX_PATTERN_LEN) patLen = MAX_PATTERN_LEN;
    // plpPatternLen_ is committed only once plpPattern_ holds a pattern of
    // that length (below) — an early return here used to leave the new
    // length paired with the old pattern's contents.
    if (ossCount_ < patLen * 2) return;

    // --- 2. Epoch fold for pattern extraction ---
    // Epoch-fold broadband spectral flux (bass-contrast weighted) at the ACF period.
    // Band selection was tested (mid/high flux) but regressed on 8-10/18 tracks;
    // broadband is the sole PLP source. Uses ungated flux — NN-independent (Grosche 2011).
    //
    // Improvements over basic mean fold:
    //   1. NN-confidence-weighted epochs (high NN activation = more trustworthy)
    //   2. Per-bin reliability from coefficient of variation (consistent events
    //      like kick-on-1 preserved, random events smoothed toward mean)
    //   3. Winsorized mean: drop most extreme epoch per bin before averaging
    //   4. Cross-correlation with NN fold for pattern validation
    {
        int count = ossCount_;
        int sIdx = (ossWriteIdx_ - count + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        for (int i = 0; i < count; i++) {
            epochFoldLinear_[i] = ossBuffer_[(sIdx + i) % OSS_BUFFER_SIZE];
        }

        // Count epochs for Winsorized mean
        int numEpochs = 0;
        for (int offset = count - patLen; offset >= 0; offset -= patLen) numEpochs++;
        if (numEpochs < 2) return;

        // Epoch fold with NN-confidence weighting + collect per-epoch values for Winsorizing
        float* perBinVariance = scratchVar_;
        memset(perBinVariance, 0, patLen * sizeof(float));
        const float decayFactor = expf(-plpDecayRate);

        // Stack buffer for Winsorized mean: store per-epoch values per bin
        // Process one bin at a time to limit stack usage (6 floats + 6 floats weights)
        static constexpr int MAX_EPOCHS = 12;
        float epochVals[MAX_EPOCHS];
        float epochWeights[MAX_EPOCHS];
        int actualEpochs = (numEpochs < MAX_EPOCHS) ? numEpochs : MAX_EPOCHS;

        // First pass: compute weights (recency × level "confidence"). Source 2
        // holds mic level since #155; mean-subtracted above, so this term is
        // ~0 and the weight is ~0.5 x recency in both modes.
        {
            float w = 1.0f;
            int ep = 0;
            for (int offset = count - patLen; offset >= 0 && ep < actualEpochs; offset -= patLen) {
                // Mean source-2 (mic level) value for this epoch
                float nnMean = 0.0f;
                if (nnCount_ >= count) {
                    for (int j = 0; j < patLen; j++) {
                        nnMean += nnLinear_[offset + j];
                    }
                    nnMean /= patLen;
                }
                epochWeights[ep] = w * (0.5f + clampf(nnMean, 0.0f, 0.5f));
                w *= decayFactor;
                ep++;
            }
        }

        // Second pass: Winsorized weighted fold per bin
        float* patternRaw = scratchRaw_;  // every bin < patLen written below before read
        float foldMean = 0.0f;

        for (int j = 0; j < patLen; j++) {
            // Collect this bin's value across all epochs
            int ep = 0;
            for (int offset = count - patLen; offset >= 0 && ep < actualEpochs; offset -= patLen) {
                epochVals[ep] = epochFoldLinear_[offset + j];
                ep++;
            }

            // Winsorize: find min/max indices and exclude them from weighted sum
            // (only when we have enough epochs to afford dropping 2)
            float wSum = 0.0f, wTotal = 0.0f;
            float wSumSq = 0.0f;
            if (actualEpochs >= 4) {
                // Note: when all values are equal, minIdx==maxIdx==0 and only
                // one epoch is skipped instead of two. Harmless: flat signal
                // means zero variance regardless of epoch count.
                int minIdx = 0, maxIdx = 0;
                for (int e = 1; e < actualEpochs; e++) {
                    if (epochVals[e] < epochVals[minIdx]) minIdx = e;
                    if (epochVals[e] > epochVals[maxIdx]) maxIdx = e;
                }
                for (int e = 0; e < actualEpochs; e++) {
                    if (e == minIdx || e == maxIdx) continue;
                    wSum += epochVals[e] * epochWeights[e];
                    wSumSq += epochVals[e] * epochVals[e] * epochWeights[e];
                    wTotal += epochWeights[e];
                }
            } else {
                for (int e = 0; e < actualEpochs; e++) {
                    wSum += epochVals[e] * epochWeights[e];
                    wSumSq += epochVals[e] * epochVals[e] * epochWeights[e];
                    wTotal += epochWeights[e];
                }
            }

            float mean = (wTotal > 0) ? wSum / wTotal : 0.0f;
            float meanSq = (wTotal > 0) ? wSumSq / wTotal : 0.0f;
            float variance = meanSq - mean * mean;
            if (variance < 0.0f) variance = 0.0f;

            patternRaw[j] = mean;
            perBinVariance[j] = variance;
            foldMean += mean;
        }
        foldMean /= patLen;

        // Per-bin reliability: coefficient of variation.
        // Low CV = consistent event (kick on 1) → keep raw value.
        // High CV = random event (ghost note) → blend toward fold mean.
        float minVal = 1e30f, maxVal = -1e30f;
        float reliabilitySum = 0.0f;
        float reliabilityWeightSum = 0.0f;
        for (int j = 0; j < patLen; j++) {
            float mean = patternRaw[j];
            float variance = perBinVariance[j];

            float cv = (mean > 0.01f) ? sqrtf(variance) / mean : 1.0f;
            float reliability = 1.0f / (1.0f + cv * cv);
            patternRaw[j] = mean * reliability + foldMean * (1.0f - reliability);

            reliabilitySum += reliability * mean;
            reliabilityWeightSum += mean;

            if (patternRaw[j] < minVal) minVal = patternRaw[j];
            if (patternRaw[j] > maxVal) maxVal = patternRaw[j];
        }
        plpMeanReliability_ = (reliabilityWeightSum > 0.01f)
            ? reliabilitySum / reliabilityWeightSum : 0.0f;

        // Normalize to [0, 1] using min-max
        float range = maxVal - minVal;
        if (range > 1e-10f) {
            for (int j = 0; j < patLen; j++) {
                float normalized = (patternRaw[j] - minVal) / range;
                plpPattern_[j] = (plpNovGain != 1.0f) ? powf(normalized, plpNovGain) : normalized;
            }
        } else {
            for (int j = 0; j < patLen; j++) plpPattern_[j] = 0.0f;
        }
        plpPatternLen_ = patLen;

        // Cross-correlation with the source-2 fold (mic level since #155, not
        // the NN) for pattern validation. High agreement = the flux pattern
        // matches the energy envelope. Low = noise-driven.
        if (nnCount_ >= patLen * 2) {
            float* nnFold = scratchNN_;
            memset(nnFold, 0, patLen * sizeof(float));
            float nnWeight = 0.0f;
            float w = 1.0f;
            for (int offset = nnCount_ - patLen; offset >= 0; offset -= patLen) {
                for (int j = 0; j < patLen; j++) {
                    nnFold[j] += nnLinear_[offset + j] * w;
                }
                nnWeight += w;
                w *= decayFactor;
                if (w < 0.01f) break;
            }
            if (nnWeight > 0) {
                for (int j = 0; j < patLen; j++) nnFold[j] /= nnWeight;
            }
            float agreement = cosineSimilarity(plpPattern_, nnFold, patLen);
            // Modulate confidence: low agreement reduces trust in the pattern
            plpNNAgreement_ = plpNNAgreement_ * 0.7f + clampf(agreement, 0.0f, 1.0f) * 0.3f;
        }

        // Rotate to ACCENT-ALIGNED storage: bin 0 = accent. The fold above is
        // end-aligned (bin P-1 = newest frame), so its rotation changes every
        // analysis as frames arrive. Reading it by phase, comparing it with
        // cached slots, or matching it to the seed templates (accent at bin
        // 0) all require a stable origin. runAcf() measured the accent on
        // the same end-aligned fold at the same period, so its bin applies.
        if (patLen == plpBestPeriod_) {
            int shift = static_cast<int>(plpAccentBin_ + 0.5f) % patLen;
            if (shift < 0) shift += patLen;
            if (shift != 0) {
                // In-place left rotation by `shift` (three reversals): no
                // temporary buffer on the 4 KB loop stack.
                auto reverse = [this](int lo, int hi) {  // [lo, hi)
                    for (--hi; lo < hi; ++lo, --hi) {
                        float t = plpPattern_[lo]; plpPattern_[lo] = plpPattern_[hi]; plpPattern_[hi] = t;
                    }
                };
                reverse(0, shift);
                reverse(shift, patLen);
                reverse(0, patLen);
            }
        }
    }

    // --- 3. PLP confidence from ACF strength + steep signal gate ---
    float acfConf = clampf(acfPeakStrength_, 0.0f, 1.0f);
    // Smoothed (~1 s) level, not one instantaneous sample — see update().
    float signalPresence = clampf((levelEnv_ - plpSignalFloor * 0.5f) / (plpSignalFloor * 0.5f), 0.0f, 1.0f);
    float targetConf = acfConf * signalPresence * (0.5f + 0.5f * plpNNAgreement_);
    plpConfidence_ += (targetConf - plpConfidence_) * plpConfAlpha;
}

void AudioTracker::countBeat() {
    if (skipNextWrap_) {
        skipNextWrap_ = false;  // this accent was counted before a backward correction
        return;
    }
    beatCount_++;
}

void AudioTracker::correctPlpPhase(float targetPhase, float gain) {
    // Shortest signed error in [-0.5, 0.5)
    float err = targetPhase - plpPhase_;
    err -= floorf(err + 0.5f);
    float p = plpPhase_ + gain * err;
    if (p >= 1.0f) {
        p -= 1.0f;
        countBeat();            // corrected forward across the accent
    } else if (p < 0.0f) {
        p += 1.0f;
        skipNextWrap_ = true;   // stepped back across an accent already counted
    }
    plpPhase_ = p;
}

void AudioTracker::advancePlpPhase(float frames) {
    // `frames` of phase at the fractional period: the real time this
    // spectral frame covers on the sample clock (1.0 normally; more after an
    // overrun dropped samples, #261). Called exactly once per spectral frame
    // (processFrame), never per loop iteration. Every accent crossed is
    // counted, so beatCount keeps absolute time too.
    float period = (plpPeriodF_ > 1.0f) ? plpPeriodF_ : 33.0f;
    const PhaseClock::Advance a = PhaseClock::advance(plpPhase_, frames, period);
    plpPhase_ = a.phase;
    if (a.accents > 0) {
        countBeat();  // the first accent crossed (honours skipNextWrap_)
        // Any more only after a long overrun: each was a real beat.
        beatCount_ = static_cast<uint16_t>(beatCount_ + (a.accents - 1));
    }

    plpPulseValue_ = patternValueAt(plpPhase_);

    // Beat stability: pattern amplitude at accent vs running EMA
    if (plpPhase_ < 0.05f || plpPhase_ > 0.95f) {
        float peakAmp = plpPulseValue_;
        static constexpr float STABILITY_ALPHA = 0.1f;
        plpPeakEma_ += STABILITY_ALPHA * (peakAmp - plpPeakEma_);
        beatStability_ = (plpPeakEma_ > 0.01f) ? clampf(peakAmp / plpPeakEma_, 0.0f, 1.5f) : 0.0f;
    }
}

float AudioTracker::patternValueAt(float phase) const {
    // plpPattern_ is accent-aligned (bin 0 = accent), so phase indexes it
    // directly.
    int patLen = plpPatternLen_;
    if (patLen <= 1) return 0.5f;
    float idx = phase * static_cast<float>(patLen);
    int i0 = static_cast<int>(idx) % patLen;
    float frac = idx - floorf(idx);
    int i1 = (i0 + 1) % patLen;
    return clampf(plpPattern_[i0] * (1.0f - frac) + plpPattern_[i1] * frac, 0.0f, 1.0f);
}

// ============================================================================
// Pulse Detection (Floor-Tracking Baseline)
// ============================================================================

float AudioTracker::pickOnset(const OnsetEvidence& ev) {
    // Runs exactly once per spectral frame (processFrame), for whichever
    // detector produced `ev` (OnsetDetector.h): one peak picker and one set
    // of gates, so swapping the detector changes only the evidence.
    // (Pre-b208 this ran once per loop iteration: duplicate samples failed
    // `prev > prevPrev` and real peaks were dropped above 62.5 fps.)

    // Spectral-flux peak hold: energy synthesis and the presence gate, both
    // detector-independent. Tracked on every frame.
    const float flux = spectral_.getSpectralFlux();
    if (flux > odfPeakHold_) {
        odfPeakHold_ = flux;
    } else {
        odfPeakHold_ *= odfPeakHoldDecay;
    }

    // Tempo-adaptive cooldown, counted in frames (the frame clock), not ms
    float bpmNorm = clampf((bpm_ - 60.0f) / 140.0f, 0.0f, 1.0f);
    float cooldownMs = 40.0f + 110.0f * (1.0f - bpmNorm);
    uint32_t cooldownFrames = static_cast<uint32_t>(cooldownMs * OSS_FRAME_RATE / 1000.0f);

    float signalPresence = max(odfPeakHold_, cachedBassEnergy_);
    float pulseStrength = 0.0f;
    bool cooldownOk = (frameClock_ - lastPulseFrame_) > cooldownFrames;

    if (!ev.fresh) {
        // No new evidence this frame (an expensive detector skipped it):
        // stepping the history with a held value would re-create the
        // duplicate-sample peak loss.
        return 0.0f;
    }

    // --- Shared gates, as threshold multipliers ---
    // Bass-ratio gate: kicks/snares carry strong bass flux; chord changes and
    // synth stabs are flat or mid-heavy. Full boost below a third.
    static constexpr float bassGateMinRatio = 1.0f / 3.0f;
    float bassFlux = spectral_.getBassFlux();
    float broadbandFlux = spectral_.getSpectralFlux();
    // In near-silence (broadbandFlux < 0.001), neutral ratio so the gate
    // doesn't suppress onsets at the start of music.
    float bassRatio = (broadbandFlux > 0.001f) ? bassFlux / broadbandFlux : 0.5f;
    float bassGate = 1.0f + onsetBassGate * clampf(1.0f - bassRatio / bassGateMinRatio, 0.0f, 1.0f);

    // Rhythm-pattern bias: while the PLP pattern is confident, raise the
    // threshold at positions where it predicts no hit. Soft (scaled by
    // confidence and pattern level), never a hard gate; onsetPatternBias 0
    // turns it off.
    static constexpr float plpBiasMinConfidence = 0.3f;  // only above 30% confidence
    float patternBias = 1.0f;
    if (plpConfidence_ > plpBiasMinConfidence) {
        float patternVal = clampf(plpPulseValue_, 0.0f, 1.0f);
        // At pattern minimum (0.0): bias = 1 + onsetPatternBias; at maximum: 1.0
        float suppressAmount = onsetPatternBias * plpConfidence_ * (1.0f - patternVal);
        patternBias = 1.0f + suppressAmount;
    }

    const float a = ev.activation;
    float effectiveFloor = detector_->threshold(OnsetDetectorParams{
        pulseThresholdMult, fluxOnsetFloor, baselineFastDrop, baselineSlowRise,
        pulseOnsetFloor, nnProfile}) * bassGate * patternBias;

    // Local maximum: prev frame rose above the frames before it (t-2 ...
    // t-1-onsetPeakWindow), is not exceeded by t, and is above the floor.
    // `>=` on the right fires a flat top (INT8 NN output often repeats a
    // value) on its FIRST frame; the strict `>` on the left stops the
    // plateau's second frame from firing again.
    int window = onsetPeakWindow;
    if (window < 1) window = 1;
    if (window > ONSET_PEAK_WINDOW_MAX) window = ONSET_PEAK_WINDOW_MAX;
    bool risesOverWindow = (prevSignal_ > prevPrevSignal_);
    for (int k = 1; k < window && risesOverWindow; k++) {
        risesOverWindow = (prevSignal_ > peakHistory_[k]);
    }
    bool isLocalMax = risesOverWindow &&
                      (prevSignal_ >= a) &&
                      (prevSignal_ > effectiveFloor);

    // Crest-factor gate (v95 / Phase 4 Path A). Drum hits have high peak/RMS;
    // tonal impulses lower. crestGateMin 0 = off (default).
    bool crestOk = (crestGateMin <= 0.0f) ||
                   (spectral_.getRawCrest() >= crestGateMin);

    // b142 — PLP pattern AND-gate (off by default, beatGridPatternMin 0):
    // reject candidates where the pattern predicts near-zero pulse. Guarded
    // by rhythm strength so it can't suppress during warmup or sparse content.
    bool beatGridOk = true;
    if (beatGridPatternMin > 0.0f &&
        periodicityStrength_ > beatGridMinRhythmStrength) {
        beatGridOk = (plpPulseValue_ >= beatGridPatternMin);
    }

    // Re-arm (onsetRearm 0 = always armed).
    const bool armedOk = (onsetRearm <= 0.0f) || onsetArmed_;

    if (signalPresence > pulseMinLevel && isLocalMax && cooldownOk && crestOk && beatGridOk &&
        armedOk) {
        pulseStrength = clampf(prevSignal_, 0.0f, 1.0f);
        lastPulseFrame_ = frameClock_ - 1;  // Peak was the previous frame
        onsetArmed_ = false;

        // T1.4/T1.5 — gate state + spectral features at every onset, for
        // offline TP/FP analysis. Bits are "which gate came close":
        //   bit 0 — bass gate raised the threshold
        //   bit 1 — pattern bias raised the threshold (off-pattern position)
        //   bit 2 — crest gate marginal (rawCrest within 1.5x of threshold)
        //   bit 3 — beat-grid gate marginal (pattern value within 0.1)
        // Features are read at the frame after the peak (detection time).
        lastPulseGateMask_ = 0;
        if (bassGate > 1.05f) lastPulseGateMask_ |= 0x01;
        if (patternBias > 1.05f) lastPulseGateMask_ |= 0x02;
        if (crestGateMin > 0.0f && spectral_.getRawCrest() < crestGateMin * 1.5f) {
            lastPulseGateMask_ |= 0x04;
        }
        if (beatGridPatternMin > 0.0f && plpPulseValue_ < beatGridPatternMin + 0.1f) {
            lastPulseGateMask_ |= 0x08;
        }
        lastPulseFeatures_.flatness = spectral_.getRawFlatness();
        lastPulseFeatures_.rawFlux = spectral_.getRawSpectralFlux();
        lastPulseFeatures_.centroid = spectral_.getSpectralCentroid();
        lastPulseFeatures_.crest = spectral_.getRawCrest();
        lastPulseFeatures_.rolloff = spectral_.getRawRolloff();
        lastPulseFeatures_.hfc = spectral_.getRawHFC();
        lastPulseFeatures_.bassRatio = bassRatio;
        lastPulseFeatures_.plpPulse = plpPulseValue_;
        lastPulseFeatures_.plpConfidence = plpConfidence_;
    }

    // Re-arm once the activation has fallen below onsetRearm x threshold.
    if (!onsetArmed_ && a < effectiveFloor * onsetRearm) onsetArmed_ = true;

    for (int k = ONSET_PEAK_WINDOW_MAX - 1; k > 0; k--) peakHistory_[k] = peakHistory_[k - 1];
    peakHistory_[0] = prevSignal_;  // becomes t-2 for the next frame
    prevPrevSignal_ = prevSignal_;
    prevSignal_ = a;

    return pulseStrength;
}

// ============================================================================
// Pattern Slot Cache (v82): PLP pattern caching for instant section recall
// ============================================================================

void AudioTracker::resamplePattern(const float* src, int srcLen, float* dst, int dstLen) {
    if (srcLen <= 0) { memset(dst, 0, dstLen * sizeof(float)); return; }
    for (int i = 0; i < dstLen; i++) {
        float srcPos = static_cast<float>(i) / dstLen * srcLen;
        int idx0 = static_cast<int>(srcPos) % srcLen;
        float frac = srcPos - floorf(srcPos);
        int idx1 = (idx0 + 1) % srcLen;
        dst[i] = src[idx0] * (1.0f - frac) + src[idx1] * frac;
    }
}

float AudioTracker::cosineSimilarity(const float* a, const float* b, int len) {
    float dot = 0.0f, normA = 0.0f, normB = 0.0f;
    for (int i = 0; i < len; i++) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }
    float denom = sqrtf(normA) * sqrtf(normB);
    return (denom > 1e-10f) ? dot / denom : 0.0f;
}

int AudioTracker::allocateSlot() {
    // Find first invalid slot
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!slots_[i].valid) return i;
    }
    // All valid — evict oldest (highest age)
    int oldest = 0;
    for (int i = 1; i < SLOT_COUNT; i++) {
        if (slots_[i].age > slots_[oldest].age) oldest = i;
    }
    // Clear the evicted slot
    memset(slots_[oldest].bins, 0, sizeof(slots_[oldest].bins));
    slots_[oldest].valid = false;
    slots_[oldest].totalBars = 0;
    slots_[oldest].confidence = 0.0f;
    slots_[oldest].seeded = false;
    return oldest;
}

void AudioTracker::checkPatternSlots() {
    if (plpPatternLen_ < 2) return;

    // 1. Resample current PLP pattern to 16-bin digest
    resamplePattern(plpPattern_, plpPatternLen_, currentDigest_, SLOT_BINS);

    // 2. Compare against all valid slots
    int bestSlot = -1;
    float bestSim = 0.0f;
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!slots_[i].valid) continue;
        float sim = cosineSimilarity(currentDigest_, slots_[i].bins, SLOT_BINS);
        if (sim > bestSim) {
            bestSim = sim;
            bestSlot = i;
        }
    }

    // 3. Decision logic
    if (bestSlot >= 0 && bestSlot != activeSlot_ && bestSim > slotSwitchThreshold) {
        // INSTANT RECALL: previously-seen section returned
        activeSlot_ = bestSlot;
        // Refresh LRU ages
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (slots_[i].valid && i != activeSlot_) slots_[i].age++;
        }
        slots_[activeSlot_].age = 0;
        // Blend cached pattern into PLP (slotSeedBlend controls mix ratio)
        float* tempPattern = scratchFold_;  // member scratch: keeps 1 KB off the loop stack
        resamplePattern(slots_[activeSlot_].bins, SLOT_BINS, tempPattern, plpPatternLen_);
        for (int j = 0; j < plpPatternLen_; j++) {
            plpPattern_[j] = tempPattern[j] * slotSeedBlend + plpPattern_[j] * (1.0f - slotSeedBlend);
        }
        // Confidence boost
        plpConfidence_ = fmaxf(plpConfidence_, slots_[activeSlot_].confidence * 0.8f);

    } else if (bestSlot == activeSlot_ && bestSim > slotNewThreshold && plpConfidence_ > slotSaveMinConf) {
        // REINFORCE active slot with current PLP pattern
        for (int i = 0; i < SLOT_BINS; i++) {
            slots_[activeSlot_].bins[i] =
                slots_[activeSlot_].bins[i] * (1.0f - slotUpdateRate) +
                currentDigest_[i] * slotUpdateRate;
        }
        slots_[activeSlot_].totalBars++;
        slots_[activeSlot_].confidence = plpConfidence_;
        slots_[activeSlot_].age = 0;  // Keep active slot fresh for LRU

    } else if (bestSim < slotNewThreshold && plpConfidence_ > slotSaveMinConf) {
        // NEW SECTION: allocate a new slot
        int newSlot = allocateSlot();
        memcpy(slots_[newSlot].bins, currentDigest_, sizeof(currentDigest_));
        slots_[newSlot].confidence = plpConfidence_;
        slots_[newSlot].totalBars = 1;
        slots_[newSlot].valid = true;
        slots_[newSlot].age = 0;
        slots_[newSlot].seeded = false;
        // Age all other slots
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (i != newSlot && slots_[i].valid) slots_[i].age++;
        }
        activeSlot_ = newSlot;

    } else if (activeSlot_ < 0 && plpConfidence_ > slotSaveMinConf) {
        // FIRST SLOT: no active slot yet, create one
        activeSlot_ = 0;
        memcpy(slots_[0].bins, currentDigest_, sizeof(currentDigest_));
        slots_[0].confidence = plpConfidence_;
        slots_[0].totalBars = 1;
        slots_[0].valid = true;
        slots_[0].age = 0;
        slots_[0].seeded = false;
    }

    // 4. Template seeding for new/young slots (one-shot, reuses SEED_TEMPLATES)
    if (activeSlot_ >= 0 && slots_[activeSlot_].valid && !slots_[activeSlot_].seeded &&
        slots_[activeSlot_].totalBars >= 1 && slots_[activeSlot_].totalBars <= 3) {
        float bestTemplateSim = 0.0f;
        int bestTemplate = -1;
        for (int t = 0; t < NUM_SEED_TEMPLATES; t++) {
            float sim = cosineSimilarity(slots_[activeSlot_].bins, SEED_TEMPLATES[t], SLOT_BINS);
            if (sim > bestTemplateSim) {
                bestTemplateSim = sim;
                bestTemplate = t;
            }
        }
        if (bestTemplate >= 0 && bestTemplateSim > 0.50f) {
            // Blend template into slot (use slotSeedBlend for consistency with recall path)
            float seedBlend = slotSeedBlend;
            for (int i = 0; i < SLOT_BINS; i++) {
                slots_[activeSlot_].bins[i] =
                    (1.0f - seedBlend) * slots_[activeSlot_].bins[i] + seedBlend * SEED_TEMPLATES[bestTemplate][i];
            }
            // Also seed PLP pattern
            float* tempPattern = scratchFold_;  // member scratch: keeps 1 KB off the loop stack
            resamplePattern(slots_[activeSlot_].bins, SLOT_BINS, tempPattern, plpPatternLen_);
            for (int j = 0; j < plpPatternLen_; j++) {
                plpPattern_[j] = tempPattern[j] * seedBlend + plpPattern_[j] * (1.0f - seedBlend);
            }
        }
        slots_[activeSlot_].seeded = true;  // One-shot: don't re-seed this slot
    }
}

// ============================================================================
// Output Synthesis
// ============================================================================

void AudioTracker::synthesizeOutputs(float dt, uint32_t nowMs) {
    // ========================================================================
    // Clean 3-signal audio API for generators:
    //
    //   energy    (0-1)  Normalized mic amplitude. Auto-ranged to fill 0-1
    //                    regardless of absolute volume. THE primary signal.
    //
    //   pulse     (0-1)  Onset envelope. Spikes to ~1.0 on transients (kicks,
    //                    snares), decays exponentially. Triggers visual events.
    //
    //   plpPulse  (0-1)  Breathing curve. Repeating pattern shape at detected
    //                    tempo. Cosine fallback when no rhythm detected.
    //
    // Generators consume ONLY these signals. They don't know about mic levels,
    // FFT, BPM, or detection internals.
    // ========================================================================

    // --- Energy: weighted blend of mic, bass mel, and ODF peak-hold ---
    // Three complementary signals: broadband mic level (overall loudness),
    // bass mel energy (low-frequency punch), ODF peak-hold (transient accent).
    float micEnergy = mic_.getLevel();
    float bassEnergy = clampf(cachedBassEnergy_, 0.0f, 1.0f);
    float odfEnergy = clampf(odfPeakHold_, 0.0f, 1.0f);
    float weightedEnergy = micEnergy * energyMicWeight
                         + bassEnergy * energyMelWeight
                         + odfEnergy * energyOdfWeight;

    // No noise-gate propagation: the absolute gate was removed (AdaptiveMic),
    // so energy, pulse, plpPulse, onset and rhythm are never zeroed by a
    // fixed level threshold. On this mic that threshold sat at quiet-music
    // level and silenced real music.

    // Smolder: hash-based deterministic noise (0.12-0.20) for baseline activity.
    uint32_t h = nowMs;
    h ^= h >> 16; h *= 0x45d9f3bU; h ^= h >> 16;
    float smolder = 0.12f + 0.08f * (h & 0xFF) * (1.0f / 255.0f);
    control_.energy = clampf(max(weightedEnergy, smolder), 0.0f, 1.0f);
    control_.level = clampf(mic_.getLevel(), 0.0f, 1.0f);

    // --- Continuous pulse envelope (visual generators) ---
    // Driven by the onset activation, the same signal as the discrete onsets,
    // whichever detector produced it (onset API, AudioTracker.h). Peak over
    // the frames processed this call: with several frames per update, the
    // latest frame alone can miss the onset frame. No new frame -> no new
    // evidence -> no re-trigger (re-feeding a stale value every loop pass
    // used to hold the envelope up between frames). Flux used to drive this
    // from flux / a 10 s running peak, a second detector: after one loud hit
    // it muted ordinary hits for seconds (#280).
    const float pulseSignal = (framesThisUpdate_ > 0) ? clampf(activationPeak_, 0.0f, 1.0f) : 0.0f;

    if (pulseSignal > 0.4f) {
        float trigger = (pulseSignal - 0.4f) / 0.6f;
        if (trigger > pulseEnvelope_) {
            pulseEnvelope_ = trigger;
        }
    }
    pulseEnvelope_ *= expf(-dt / 0.24f);
    if (pulseEnvelope_ < 0.01f) pulseEnvelope_ = 0.0f;

    control_.pulse = clampf(pulseEnvelope_, 0.0f, 1.0f);

    // --- Phase + PLP Pulse: breathing curve ---
    // Phase advances once per spectral frame at the fractional period and is
    // pulled toward the measured accent at each analysis (advancePlpPhase /
    // correctPlpPhase). plpPulseValue_ is the accent-aligned pattern at it.
    // Rhythm needs hits. In room noise the ACF still finds "periodicity"
    // (the slowly varying inputs self-correlate at beat lags: rhythm read
    // 0.45-0.77 after music stopped), and there is no level threshold to
    // tell noise from music on this mic. Onsets are the evidence: the flux
    // detector fires ~6.5/s in music and not at all on room noise. After
    // RHYTHM_HOLD_S without an onset the rhythm outputs fade over
    // RHYTHM_FADE_S; the next onset restores them at once (no added delay).
    // Tempo/phase state is kept, so a breakdown can't wipe it (#172).
    const float sinceOnsetS = static_cast<float>(frameClock_ - lastPulseFrame_) / OSS_FRAME_RATE;
    const float rhythmPresence =
        clampf(1.0f - (sinceOnsetS - RHYTHM_HOLD_S) / RHYTHM_FADE_S, 0.0f, 1.0f);

    // Phase at the moment of drawing (#260): plpPhase_ is as of the last
    // processed frame, up to a frame plus an undelivered PDM block ago.
    // Extrapolated along the sample clock, held below an accent the tracker
    // hasn't counted yet (PhaseClock::outputPhase).
    float phaseNow = plpPhase_;
    if (haveFrameEnd_) {
        const uint32_t ahead = mic_.samplesHeardSince(lastFrameEndSampleIdx_, time_.millis());
        phaseNow = PhaseClock::outputPhase(plpPhase_, ahead, plpPeriodF_);
    }
    control_.phase = phaseNow;
    control_.beatCount = beatCount_;
    control_.plpPulse = patternValueAt(phaseNow) * rhythmPresence;
    control_.onset = lastPulseStrength_;

    // --- Rhythm Strength ---
    // Driven by ACF periodicity strength, present only while hits are.
    control_.rhythmStrength = periodicityStrength_ * rhythmPresence;

    // --- Onset Density ---
    // Detected onsets per second (counted in processFrame). It used to count
    // rising edges of the pulse envelope through 0.1, which dense hits keep
    // high: it read ~1/s on dnb with 6 onsets/s (#280).
    if (nowMs - onsetDensityWindowStart_ >= 1000) {
        float newDensity = static_cast<float>(onsetCountInWindow_);
        onsetDensity_ = onsetDensity_ * 0.7f + newDensity * 0.3f;
        onsetCountInWindow_ = 0;
        onsetDensityWindowStart_ = nowMs;
    }
    control_.onsetDensity = onsetDensity_;

    // Diagnostic amplification: scale and clamp all downstream signals.
    // When audioAmp=1 this is a no-op. When audioAmp=100 any tiny non-zero
    // input clamps to 1.0, so a correctly-wired chain shows fully saturated
    // visual response on any audio activity — distinguishes "wired but
    // subtle" from "not wired" without ambiguity.
    if (audioAmp != 1.0f) {
        control_.energy        = clampf(control_.energy        * audioAmp, 0.0f, 1.0f);
        control_.level         = clampf(control_.level         * audioAmp, 0.0f, 1.0f);
        control_.pulse         = clampf(control_.pulse         * audioAmp, 0.0f, 1.0f);
        control_.onset         = clampf(control_.onset         * audioAmp, 0.0f, 1.0f);
        control_.plpPulse      = clampf(control_.plpPulse      * audioAmp, 0.0f, 1.0f);
        control_.rhythmStrength= clampf(control_.rhythmStrength* audioAmp, 0.0f, 1.0f);
    }
}
