#pragma once

/**
 * RecordStream.h - Phone record mode: raw mic PCM + live analysis over BLE.
 *
 * See docs/PHONE_RECORD_MODE_PLAN.md. A phone (Android Chrome, Web
 * Bluetooth) subscribes to the Record data characteristic, sends
 * `record start` over NUS, and receives a binary packet stream:
 *
 *   - AUDIO:  raw int16 PCM straight from the PDM ISR (authoritative stream)
 *   - FRAME:  per-spectral-frame analysis (NN activation, pulse, level, ...)
 *   - STATUS: 1 Hz counters (drops, clipping, loop fps, RSSI)
 *   - END:    final counters after `record stop`
 *
 * Data-integrity rules (the reason this exists — dropped or laggy frames
 * poison the data):
 *   1. Audio has priority. Secondary packets are shed first, and only while
 *      the audio backlog is under half the ring.
 *   2. Alignment is by absolute mic-sample index (AdaptiveMic's ring
 *      counter), never by arrival time. AUDIO carries the index of its first
 *      sample; FRAME carries the index at which its FFT window ended.
 *   3. Nothing is lost silently. Per-type sequence numbers + drop counters.
 *
 * The packet structs below are the CANONICAL wire format. Mirrors:
 *   blinky-console/src/services/record/recordProtocol.ts
 *   ml-training/scripts/convert_blrec.py
 * Bump RecordProto::VERSION on any layout change.
 *
 * Threading:
 *   - PDM ISR        → onIsrSamples()  (audio ring producer)
 *   - loop task      → onLoop()        (frame/status queue producer)
 *   - sender task    → drains both via BLE notify (sole consumer)
 *   - BLE/loop task  → start()/stop() via console commands
 *
 * nRF52840 only.
 */

#include <Arduino.h>
#include <bluefruit.h>
#include "../audio/SharedSpectralAnalysis.h"

class AudioTracker;
class AdaptiveMic;
class BleNus;

namespace RecordProto {
    constexpr uint8_t VERSION = 1;

    enum PacketType : uint8_t {
        PKT_AUDIO  = 1,
        PKT_FRAME  = 2,
        PKT_STATUS = 3,
        PKT_END    = 4,
    };

    // FRAME flags
    constexpr uint8_t FRAME_NN_READY     = 0x01;  // NN model loaded (nna meaningful)
    constexpr uint8_t FRAME_PULSE        = 0x02;  // onset pulse fired this frame
    constexpr uint8_t FRAME_FFT_OVERRUN  = 0x04;  // mic FFT ring overran since previous frame
    constexpr uint8_t FRAME_HAS_MEL      = 0x08;  // mel[] appended

    constexpr int NUM_MEL = SpectralConstants::NUM_MEL_BANDS;

    struct __attribute__((packed)) Header {
        uint8_t  type;
        uint8_t  flags;
        uint16_t seq;          // per-type, wraps
    };

    struct __attribute__((packed)) AudioPayload {
        uint32_t sampleIndex;  // absolute index of samples[0]
        // int16_t samples[N] follows; N = (len - sizeof(Header) - 4) / 2
    };

    struct __attribute__((packed)) FramePayload {
        uint32_t sampleIndex;  // absolute index one past the window's last sample
        uint32_t ms;           // device millis() when the frame was reported
        uint16_t nna;          // raw NN activation, 0..1 → 0..65535
        uint16_t pulse;        // pulse strength this frame (0 unless fired), 0..1 → 0..65535
        uint16_t level;        // AdaptiveMic normalized level, 0..1 → 0..65535
        uint16_t rawLevel;     // mean |x|/32768 over the loop window, 0..1 → 0..65535
        uint16_t bpm10;        // BPM × 10
        uint8_t  phase;        // PLP phase 0..1 → 0..255
        uint8_t  rstr;         // rhythm strength 0..1 → 0..255
        uint8_t  skipped;      // spectral frames not reported since last FRAME (0 since b208: every frame is reported)
        uint8_t  reserved;
        // uint8_t mel[NUM_MEL] follows iff FRAME_HAS_MEL (raw mel 0..1 → 0..255)
    };

    struct __attribute__((packed)) StatusPayload {
        uint32_t ms;
        uint32_t audioSamplesSent;
        uint32_t audioSamplesDropped;   // record-ring overrun (link too slow)
        uint32_t framesQueued;
        uint32_t framesDropped;         // secondary queue full
        uint32_t clipSamples;           // since record start
        uint32_t fftOverruns;           // since record start (device analysis corrupted)
        uint32_t fftOverrunSamples;     // since record start
        uint16_t peakAbs;               // max |sample| since previous STATUS
        uint16_t fpsX10;                // loop fps (last closed 5 s window) × 10
        uint16_t maxFrameMs;            // loop max frame time, last window
        uint16_t ringHighWater;         // max audio backlog (samples) since previous STATUS
        int8_t   rssi;
        uint8_t  hwGain;
        uint16_t reserved;
    };

    constexpr size_t MAX_SECONDARY_LEN = sizeof(Header) + sizeof(FramePayload) + NUM_MEL;
    static_assert(sizeof(Header) + sizeof(StatusPayload) <= MAX_SECONDARY_LEN,
                  "secondary queue slot must fit a STATUS packet");
    // Minimum notification payload we accept (MTU - 3). Every secondary packet
    // must fit in ONE notification — BLECharacteristic::notify() would split a
    // longer one across notifications and break packet framing.
    constexpr uint16_t MIN_PAYLOAD = 128;
    static_assert(MAX_SECONDARY_LEN <= MIN_PAYLOAD, "secondary packet exceeds MIN_PAYLOAD");
}

class RecordStream {
public:
    /// Register the GATT service. Call after bleNus.begin() (Bluefruit up).
    void begin(AudioTracker* audio, AdaptiveMic* mic);

    /// Start recording. Writes a one-line JSON reply (success header or
    /// {"error":...}) to `out`. Returns true on success.
    bool start(bool withMel, Print& out);

    /// Request stop: the tap is cut immediately, the sender flushes what's
    /// buffered, then sends END. Returns false if not recording.
    bool stop(Print& out);

    /// One-line JSON status to `out`.
    void printStatus(Print& out) const;

    /// Loop-task hook: emits a STATUS packet every second. Cheap no-op when
    /// not recording. FRAME packets come from onFrame() (AudioTracker hook).
    void onLoop();

    bool isActive() const { return active_; }

    /// Service UUID (for advertising / docs).
    static const uint8_t SERVICE_UUID[16];
    static const uint8_t DATA_UUID[16];

private:
    static void onIsrSamples(const int16_t* samples, int count, uint32_t firstIndex);
    static void onFrameHook(void* ctx);
    void onFrame();             // one FRAME packet per spectral frame (loop task)
    static void senderTaskEntry(void* arg);
    void senderLoop();
    bool sendPacket(const uint8_t* data, uint16_t len);
    void finishLocked(const char* reason);
    bool pushSecondary(const uint8_t* data, uint16_t len, uint32_t session);
    void buildStatus(uint8_t type, uint16_t seq, uint8_t* buf, uint16_t& len);

    static RecordStream* instance_;

    BLEService service_{BLEUuid(SERVICE_UUID)};
    BLECharacteristic dataChr_{BLEUuid(DATA_UUID)};
    TaskHandle_t task_ = nullptr;   // created on first start()
    bool serviceReady_ = false;     // GATT service + characteristic registered

    AudioTracker* audio_ = nullptr;
    AdaptiveMic* mic_ = nullptr;

    // --- Audio ring (ISR producer, sender consumer) ---
    // Slot = absolute sample index & mask, so overwrite-oldest needs no
    // coordination and gaps are located exactly by index arithmetic.
    // 16384 samples = 32 KB = 1.02 s of link slack at 16 kHz.
    static constexpr uint32_t RING_SAMPLES = 16384;
    static_assert((RING_SAMPLES & (RING_SAMPLES - 1)) == 0, "RING_SAMPLES must be a power of two");
    // Allocated on the first `record start`, never freed. Not static
    // storage: 32 KB of .bss would shrink the heap at boot on every device,
    // whether or not record mode is ever used (and a boot-time allocation
    // failure elsewhere is a halt). Lazy allocation failing just refuses
    // the record session.
    static int16_t* ring_;
    volatile uint32_t writeIdx_ = 0;   // absolute index one past newest sample (ISR)
    uint32_t readIdx_ = 0;             // absolute index of next sample to send (sender)

    // --- Secondary queue (loop producer, sender consumer) ---
    static constexpr int QUEUE_SLOTS = 64;
    // `session` tags each slot with the generation it was produced in, so a
    // push that straddles a stop→start (loop task mid-onLoop while another
    // task starts a new session) is discarded instead of leaking into the
    // new session's stream.
    struct Slot { uint16_t len; uint32_t session; uint8_t data[RecordProto::MAX_SECONDARY_LEN]; };
    Slot queue_[QUEUE_SLOTS];
    volatile uint16_t qHead_ = 0;      // next write (loop)
    volatile uint16_t qTail_ = 0;      // next read (sender)

    // --- Session state ---
    volatile bool active_ = false;          // session running (sender owns the transition to false)
    volatile bool stopRequested_ = false;
    volatile uint32_t session_ = 0;         // generation, bumped by start()
    bool withMel_ = false;
    uint16_t connHandle_ = BLE_CONN_HANDLE_INVALID;
    uint16_t samplesPerPacket_ = 0;
    uint32_t startIdx_ = 0;
    uint32_t startMs_ = 0;

    // Sequence numbers (sender owns audio/end; loop owns frame/status).
    uint16_t seqAudio_ = 0, seqFrame_ = 0, seqStatus_ = 0, seqEnd_ = 0;

    // Loop-side bookkeeping
    uint32_t lastFrameCount_ = 0;
    uint32_t lastStatusMs_ = 0;
    uint32_t lastOverrunCount_ = 0;
    uint32_t clipBase_ = 0, overrunBase_ = 0, overrunSamplesBase_ = 0;

    // Counters (monotonic per session)
    volatile uint32_t audioSamplesSent_ = 0;
    volatile uint32_t audioSamplesDropped_ = 0;
    volatile uint32_t framesQueued_ = 0;
    volatile uint32_t framesDropped_ = 0;
    volatile uint32_t ringHighWater_ = 0;   // reset per STATUS
    volatile uint32_t sendRetries_ = 0;
    const char* lastEndReason_ = "none";
};
