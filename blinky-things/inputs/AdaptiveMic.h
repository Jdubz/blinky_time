#pragma once

#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"
#include "../hal/PlatformConstants.h"

// Utility functions for integer/float clamp to avoid pulling in math.h
static inline float maxValue(float a, float b) { return (a > b) ? a : b; }
static inline int constrainValue(int val, int lo, int hi) { return val < lo ? lo : (val > hi ? hi : val); }
static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

namespace MicConstants {
    // Exponential tracking limits (prevent division by zero)
    constexpr float MIN_TAU_RANGE = 0.005f;     // Minimum tau for range tracking (5ms)

    // Absolute noise gate default (raw ADC scale, 0-1): the measured mic
    // noise floor, ~-54 dBFS. Shared by AdaptiveMic and ConfigStorage defaults.
    constexpr float DEFAULT_NOISE_GATE = 0.002f;

    // Window/range normalization — divide-by-zero protection only.
    //
    // b192: removed the 0.01 floor that capped how far the normalizer could
    // scale at low-SNR sites. The old floor meant that when the actual
    // peak-valley span was smaller than 0.01 (mic raw peaks ~0.010 = -50 dBFS),
    // `mapped` was pinned low and `level` averaged ~0.03 even with audio
    // present. Now the only floor is a tiny epsilon to keep `peak == valley`
    // (true silence) from dividing by zero. The normalizer auto-scales to
    // whatever signal range exists — at noisy install sites this also
    // amplifies the mic's own thermal-noise floor into apparent activity,
    // which is the intentional trade for full responsiveness at any SNR.
    // Generator-side AudioControl semantics remain device-agnostic — all
    // per-site compensation is contained inside AdaptiveMic.
    constexpr float MIN_NORMALIZATION_RANGE = 1e-6f;

    // Valley tracking — no floor. The valley tracks the true envelope minimum,
    // wherever it is. Combined with the epsilon above this lets peak-valley
    // shrink arbitrarily on quiet input.
    constexpr float VALLEY_FLOOR = 0.0f;                      // No floor: valley free to track to 0
    constexpr float VALLEY_RELEASE_MULTIPLIER = 4.0f;         // How much slower valley rises vs peak
    constexpr float VALLEY_ATTACK_TAU = 0.2f;                 // Valley follows new minimums this fast (s)
    constexpr float INSTANT_ADAPT_THRESHOLD = 1.3f;           // Jump peak if signal exceeds by 30%...
    constexpr int   INSTANT_ADAPT_BLOCKS = 2;                 // ...for this many consecutive updates
    constexpr float SECTION_JUMP_RATIO = 2.0f;                // Peak jump this large = new section (drop)
    constexpr float SECTION_VALLEY_FRACTION = 0.5f;           // On a section jump, valley ≥ this × peak

    // Sustained-rise (riser / build-up) peak tracking (#176). A smooth rise
    // keeps the input above a first-order peak follower indefinitely
    // (input/peak settles at ~1 + rate*tau, below INSTANT_ADAPT_THRESHOLD),
    // so level = (x - valley)/(peak - valley) clamps at 1.0 for the whole
    // build: a 20 dB / 20 s noise riser read 1.0 for 58 % of it, 9 s in a
    // row. A faster attack alone can't help while x > peak; once the input
    // has stayed above the peak for RISER_HOLD_S, drive the peak toward
    // RISER_HEADROOM * x so level rides below 1 with room for transients.
    // Single hits never qualify (they don't stay above the peak that long).
    // Riser mode lasts while the rise continues (new input highs) and ends
    // RISER_STALL_S after the last new high; the raised peak then relaxes
    // through the normal release, so level eases back instead of jumping.
    // (Exiting as soon as the peak overtook the input re-pinned every ~1 s:
    // a brightness sawtooth through the build.)
#ifndef BLINKY_RISER_HOLD_S
#define BLINKY_RISER_HOLD_S 0.5f
#endif
#ifndef BLINKY_RISER_HEADROOM
#define BLINKY_RISER_HEADROOM 1.2f
#endif
#ifndef BLINKY_RISER_TAU
#define BLINKY_RISER_TAU 0.5f
#endif
#ifndef BLINKY_RISER_STALL_S
#define BLINKY_RISER_STALL_S 1.0f
#endif
#ifndef BLINKY_RISER_SMOOTH_TAU
#define BLINKY_RISER_SMOOTH_TAU 0.15f
#endif
#ifndef BLINKY_RISER_MAX_GROWTH
#define BLINKY_RISER_MAX_GROWTH 1.5f
#endif
    constexpr float RISER_HOLD_S = BLINKY_RISER_HOLD_S;       // continuous time above peak before riser mode (s)
    constexpr float RISER_HEADROOM = BLINKY_RISER_HEADROOM;   // riser-mode peak target = this × input
    constexpr float RISER_TAU = BLINKY_RISER_TAU;             // riser-mode peak attack time constant (s)
    constexpr float RISER_STALL_S = BLINKY_RISER_STALL_S;     // exit riser mode this long after the last new high (s)
    // The riser max follows a smoothed input so a single loud block (a hit
    // inside the build) can't become the target and suppress level after it.
    constexpr float RISER_SMOOTH_TAU = BLINKY_RISER_SMOOTH_TAU;  // smoothing for the riser max (s)
    // Real risers grow ~1-2 dB/s; a hit is a step. Cap how fast the riser
    // max may grow (factor per second, 1.5 = ~3.5 dB/s) so a hit inside the
    // build can only nudge the target.
    constexpr float RISER_MAX_GROWTH = BLINKY_RISER_MAX_GROWTH;  // max riser-max growth factor per second

    // Timing
    constexpr float MIN_DT_SECONDS = 0.0005f;                 // Minimum dt clamp (0.5ms)
    constexpr float MAX_DT_SECONDS = 0.1000f;                 // Maximum dt clamp (100ms)
    constexpr uint32_t MIC_DEAD_TIMEOUT_MS = 250;             // PDM alive check timeout
}

/**
 * AdaptiveMic — Microphone input with window/range normalization
 *
 * Hardware gain is fixed at the platform's level (Platform::Microphone::DEFAULT_GAIN;
 * nRF52840 = 32, i.e. GAINL register 32 = -4 dB since 0x28 = 0 dB).
 * No AGC — the window/range normalization handles all dynamic range adaptation.
 * This creates identical signal processing on both platforms and eliminates
 * competing adaptation systems that cause visible visual disruption.
 *
 * Window/Range Normalization:
 *   peak/valley tracking maps raw 0-32768 ADC range to normalized 0-1 output
 *   with asymmetric attack/release for natural dynamics.
 */
class AdaptiveMic {
public:
  // ---- Tunables (window/range normalization) ----
  float peakTau        = 2.0f;      // Peak adaptation speed (attack time, seconds)
  float releaseTau     = 5.0f;      // Peak release speed (release time, seconds)

  // Absolute noise gate (raw ADC scale, 0-1). When the instantaneous
  // raw mic level is below this, output level = 0 AND peak/valley
  // trackers are frozen. Prevents the b191-uncapped normalizer from
  // amplifying mic thermal noise into apparent signal after extended
  // silence (operator feedback: "always on after a few seconds of quiet").
  //
  // Tune via `set noisegate <N>` (persisted since SETTINGS_VERSION 96).
  // Default 0.002 matches measured mic
  // noise floor (~-54 dBFS). Raise to be more conservative on quiet;
  // lower to allow even fainter audio through.
  float noiseGate      = MicConstants::DEFAULT_NOISE_GATE;

  // ---- Public state ----
  float  level         = 0.0f;  // Final output level (0-1, normalized via adaptive peak/valley tracking)
  int    currentHardwareGain = Platform::Microphone::DEFAULT_GAIN;    // Fixed PDM hardware gain

  // Debug/health
  uint32_t lastIsrMs = 0;
  bool     pdmAlive  = false;
  inline bool isMicDead(uint32_t nowMs, uint32_t timeoutMs = MicConstants::MIC_DEAD_TIMEOUT_MS) const {
    return (int32_t)(nowMs - lastIsrMs) > (int32_t)timeoutMs;
  }

  // Public getters
  inline float getLevel() const { return level; }
  inline float getPeakLevel() const { return peakLevel; }
  inline float getValleyLevel() const { return valleyLevel; }
  inline float getRawLevel() const { return rawInstantLevel; }
  inline int getHwGain() const { return currentHardwareGain; }
  inline uint32_t getIsrCount() const { return s_isrCount; }
  inline bool isPdmAlive() const { return pdmAlive; }

  // FFT-ring overrun counters (monotonic from boot). The PDM ISR is
  // free-running DMA so audio never stops at the hardware level, but
  // if the main loop stalls longer than ~128 ms (ring fill time at
  // 16 kHz with FFT_RING_SIZE = 2048) `getSamplesForExternal` discards
  // old samples to catch up — that drop is invisible without these
  // counters. Surfaced via SerialConsole's `json info`.
  static uint32_t getOverrunCount() { return s_overrunCount; }
  static uint32_t getOverrunSamplesLost() { return s_overrunSamplesLost; }

  // Clipping telemetry. A sample at |x| >= CLIP_THRESHOLD is at int16 full
  // scale — the PDM decimator saturated and the waveform is flat-topped.
  // Monotonic from boot. Note an analog mic can overload BELOW digital full
  // scale; that only shows in raw audio (record mode), not in this counter.
  static constexpr uint16_t CLIP_THRESHOLD = 32767;
  static uint32_t getClipSampleCount() { return s_clipSamples; }
  static uint32_t getTotalSampleCount() { return s_totalSamples; }

  // Peak |sample| since the last takePeakAbs() call (raw int16 scale).
  // Single consumer: record-mode status packets.
  uint16_t takePeakAbs() { uint16_t p = peakAbsSinceTake_; peakAbsSinceTake_ = 0; return p; }

  // Absolute sample index (monotonic, wraps at 2^32) of the next sample the
  // external FFT consumer will read. After a spectral window completes this
  // is the index one past the window's last sample.
  uint32_t getExternalReadIndex() const { return s_extFftReadIdx; }

  // Samples waiting in the FFT ring for the external consumer (may exceed
  // the ring size after an overrun; getSamplesForExternal handles that).
  uint32_t getPendingSamples() const {
    time_.disableInterrupts();
    uint32_t w = s_fftWriteIdx;
    time_.enableInterrupts();
    return w - s_extFftReadIdx;
  }

  // Record-mode tap: called from the PDM ISR with every block of samples and
  // the absolute index of block[0]. Must be ISR-safe and O(n). nullptr = off.
  using RecordTap = void (*)(const int16_t* samples, int count, uint32_t firstIndex);
  // Atomically install `tap` and return the absolute index the tap's first
  // block will start at. `tapWriteIdx` (the consumer's producer-position
  // variable, which the tap advances) is initialized to that index inside
  // the same critical section, so no ISR block can land between the
  // snapshot, the init, and the install.
  static uint32_t armRecordTap(RecordTap tap, volatile uint32_t& tapWriteIdx);
  static void disarmRecordTap() { s_recordTap = nullptr; }

  // ---- Construction / lifecycle ----
  AdaptiveMic(IPdmMic& pdm, ISystemTime& time);

  bool begin(uint32_t sampleRate = Platform::Microphone::DEFAULT_SAMPLE_RATE,
             int gainInit = Platform::Microphone::DEFAULT_GAIN);
  void end();

  void update(float dt);

  /**
   * Get samples from ISR ring buffer for external FFT consumers
   * Used by AudioController to feed SharedSpectralAnalysis
   */
  int getSamplesForExternal(int16_t* buffer, int maxCount);

  // ISR hook
  static void onPDMdata();

private:
  IPdmMic& pdm_;
  ISystemTime& time_;

  // ISR accumulators
  static AdaptiveMic* s_instance;
  volatile static uint32_t s_isrCount;
  volatile static uint64_t s_sumAbs;
  volatile static uint32_t s_numSamples;
  volatile static uint16_t s_maxAbs;

  // 2048 samples = 128 ms headroom; was 512 (32 ms) until b154 overran 5-6×/sec (see #125).
  static constexpr int FFT_RING_SIZE = 2048;
  static_assert((FFT_RING_SIZE & (FFT_RING_SIZE - 1)) == 0,
                "FFT_RING_SIZE must be a power of two (used as ring mask in getSamplesForExternal)");
  volatile static int16_t s_fftRing[FFT_RING_SIZE];
  volatile static uint32_t s_fftWriteIdx;
  static uint32_t s_extFftReadIdx;

  // FFT-ring overrun counters. Public getters live in the public
  // section above (getOverrunCount / getOverrunSamplesLost) so the
  // class interface is grouped consistently. Counters themselves stay
  // private — only incremented from getSamplesForExternal and read
  // through the getters. A non-zero rate indicates overruns at the
  // rate of frames-with-corrupted-mel.
  static volatile uint32_t s_overrunCount;
  static volatile uint32_t s_overrunSamplesLost;

  static volatile uint32_t s_clipSamples;
  static volatile uint32_t s_totalSamples;
  static volatile RecordTap s_recordTap;
  uint16_t peakAbsSinceTake_ = 0;

  // Window/Range tracking
  float peakLevel = 0.0f;
  float valleyLevel = 0.0f;
  float rawInstantLevel = 0.0f;

  uint32_t _sampleRate = 16000;
  int instantAdaptRun_ = 0;   // consecutive updates above INSTANT_ADAPT_THRESHOLD × peak
  float aboveRunS_ = 0.0f;    // continuous time the input has stayed above peakLevel (#176)
  bool  riserMode_ = false;   // sustained-rise peak tracking active (#176)
  float riserMax_ = 0.0f;     // highest input seen in the current riser
  float riserStallS_ = 0.0f;  // time since riserMax_ last increased
  float riserSmooth_ = 0.0f;  // input smoothed with RISER_SMOOTH_TAU (riser max source)

private:
  void consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n);
};
