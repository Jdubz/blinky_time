// Host harness: run the firmware's AudioTracker on recorded audio with a
// simulated main loop, and log what the generators would see.
//
// Purpose: verify rhythm tracking (tempo, phase, beat counter, pulses) is
// independent of main-loop rate and doesn't drift — offline and
// deterministic, instead of eyeballing LEDs. See tests/tracker/README.md.
//
// Input:  raw little-endian int16 mono PCM at 16 kHz (the PDM rate).
// Output: CSV, one row per simulated loop iteration:
//   t_ms,phase,plp_pulse,pulse,onset,beat_count,bpm,period_f,rhythm,frames,level,energy
//
// Default build: no TFLite, and the onset NN is off (BLINKY_ONSET_NN_ENABLED=0,
// as on devices), so onset pulses come from spectral flux, exactly as the
// device runs. Build with BLINKY_HOST_NN (build_host_nn.sh) to run the real NN.

#include "audio/AudioTracker.h"
#include "types/BlinkyAssert.h"
#include "audio/HostCostHook.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr int SAMPLE_RATE = 16000;
constexpr int PDM_BLOCK = 256;  // samples per PDM DMA buffer (512 B), as on device

class HostTime : public ISystemTime {
public:
    uint64_t us = 0;
    uint32_t millis() const override { return static_cast<uint32_t>(us / 1000); }
    uint32_t micros() const override { return static_cast<uint32_t>(us); }
    void delay(uint32_t) override {}
    void delayMicroseconds(uint32_t) override {}
    void disableInterrupts() override {}
    void enableInterrupts() override {}
};

class HostPdm : public IPdmMic {
public:
    const std::vector<int16_t>* pcm = nullptr;
    size_t pos = 0;        // next sample to deliver
    size_t blockEnd = 0;   // end of the block the "ISR" is currently draining
    ReceiveCallback cb = nullptr;

    bool begin(int, long) override { return true; }
    void end() override {}
    void setGain(int) override {}
    void onReceive(ReceiveCallback c) override { cb = c; }
    int available() override { return static_cast<int>((blockEnd - pos) * 2); }
    int read(int16_t* buf, int maxBytes) override {
        size_t n = std::min<size_t>(blockEnd - pos, static_cast<size_t>(maxBytes / 2));
        std::memcpy(buf, pcm->data() + pos, n * 2);
        pos += n;
        return static_cast<int>(n * 2);
    }
    // Deliver one DMA block through the ISR callback, as hardware would.
    bool deliverBlock() {
        if (pos + PDM_BLOCK > pcm->size()) return false;
        blockEnd = pos + PDM_BLOCK;
        if (cb) cb();
        pos = blockEnd;  // ISR reads everything available
        return true;
    }
};

int usage() {
    std::fprintf(stderr,
        "Usage: tracker_harness <in.pcm16> <out.csv> [loop_fps=60] [jitter=0.25] [seed=1]\n"
        "  loop_fps  mean simulated main-loop rate (device: ~30-90 depending on load)\n"
        "  jitter    relative std-dev of each loop period (0 = perfectly periodic)\n"
        "  then optional overrides: [plpPhaseGain] [tempoPriorWidth] [syncopatedCandidateScale]\n"
        "  [pulseOnsetFloor] [fluxDelta];\n"
        "  \"-\" keeps the firmware\n"
        "  default for that slot\n"
        "  --nn-cost-us=N --frame-cost-us=N --render-cost-us=N  CPU cost model: the loop\n"
        "  period becomes render cost + audio work (loop_fps is ignored; jitter scales the\n"
        "  render cost)\n"
        "  --onset-pattern-bias=X  shared rhythm-pattern gate (0 = off)\n"
        "  --onset-max-frames=N --onset-min-gap=N  shared online peak picker (W1, W5)\n"
        "  --flux-avg-frames=N --flux-lambda=X  SuperFlux picker average (W3) and log lambda\n"
        "  --onset-detector=NAME  spectral-flux | onset-nn (NN builds)\n"
        "  --frame-features=PATH  per-frame feature dump\n");
    return 2;
}

// ---- CPU cost model (--nn-cost-us / --frame-cost-us / --render-cost-us) ----
// Off by default: the virtual clock then moves only by the simulated loop
// period, as before. With any cost set, the loop period is emergent: each
// iteration costs --render-cost-us, and every spectral frame / NN inference
// the tracker reports via BLINKY_HOST_COST advances the clock by its cost
// while the PDM "DMA" keeps delivering blocks. This reproduces on the host
// what the device does when audio work competes with rendering.
HostTime* g_time = nullptr;
HostPdm* g_pdm = nullptr;
uint64_t g_nextBlockUs = 0;
bool g_more = true;
double g_nnCostUs = 0, g_frameCostUs = 0, g_renderCostUs = 0;
uint64_t g_nnCount = 0;

void deliverDueBlocks() {
    while (g_more && g_nextBlockUs <= g_time->us) {
        if (!g_pdm->deliverBlock()) { g_more = false; break; }
        g_nextBlockUs += static_cast<uint64_t>(PDM_BLOCK * 1e6 / SAMPLE_RATE);
    }
}

void onWork(int work) {
    if (work == HostCost::NN_INFER) g_nnCount++;
    const double cost = (work == HostCost::NN_INFER) ? g_nnCostUs : g_frameCostUs;
    g_time->us += static_cast<uint64_t>(cost);
    deliverDueBlocks();  // the ISR keeps running while the CPU is busy
}

}  // namespace

namespace HostCost { void (*onWork)(int) = nullptr; }

// ---- Per-frame feature dump (--frame-features=PATH, drum verifier work, #280) ----
// One row per spectral frame, written from AudioTracker's frame hook: the
// frame's end time, the picker's output (pulse > 0 = onset at this frame:
// the online picker has no look-ahead), the detector activation, the
// SuperFlux ODF, global spectral features, and
// energy + half-wave flux in 11 log-spaced bands of the compressed magnitudes.
namespace FrameDump {
constexpr int NB = 11;
constexpr int EDGES[NB + 1] = {1, 3, 5, 7, 10, 14, 20, 28, 40, 56, 80, 128};  // FFT bins, 62.5 Hz each
FILE* out = nullptr;
const AudioTracker* tracker = nullptr;
float prev[SpectralConstants::NUM_BINS] = {0};

void hook(void*) {
    const SharedSpectralAnalysis& s = tracker->getSpectral();
    const float* m = s.getMagnitudes();
    std::fprintf(out, "%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.5f",
                 tracker->getLastFrameEndSampleIdx() / 16000.0, tracker->getLastFramePulse(),
                 tracker->getOnsetActivation(), s.getSuperFlux(), s.getSpectralFlux(), s.getBassFlux(),
                 s.getRawSpectralFlux(), s.getRawFlatness(), s.getRawCrest(),
                 s.getSpectralCentroid(), s.getRawRolloff(), s.getRawHFC());
    for (int b = 0; b < NB; b++) {
        float e = 0.0f, f = 0.0f;
        for (int i = EDGES[b]; i < EDGES[b + 1]; i++) {
            e += m[i];
            const float d = m[i] - prev[i];
            if (d > 0.0f) f += d;
        }
        const float n = static_cast<float>(EDGES[b + 1] - EDGES[b]);
        std::fprintf(out, ",%.5f,%.5f", e / n, f / n);
    }
    std::fputc('\n', out);
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) prev[i] = m[i];
}
}  // namespace FrameDump
static const char* g_frameFeatures = nullptr;

// Shared onset gate overrides (negative = keep the firmware default).
static double g_onsetPatternBias = -1.0;
static double g_onsetMaxFrames = -1.0;
static double g_onsetMinGap = -1.0;
static double g_fluxAvgFrames = -1.0;
static double g_fluxLambda = -1.0;
static const char* g_onsetDetector = nullptr;  // --onset-detector=NAME

int main(int rawArgc, char** rawArgv) {
    // Split --name=value options from the positional arguments.
    std::vector<char*> pos;
    for (int i = 0; i < rawArgc; i++) {
        const char* a = rawArgv[i];
        if (i > 0 && std::strncmp(a, "--", 2) == 0) {
            const char* eq = std::strchr(a, '=');
            if (!eq) return usage();
            if (std::strncmp(a, "--onset-detector=", 17) == 0) {
                g_onsetDetector = eq + 1;
                continue;
            }
            if (std::strncmp(a, "--frame-features=", 17) == 0) {
                g_frameFeatures = eq + 1;
                continue;
            }
            const double v = std::atof(eq + 1);
            if (std::strncmp(a, "--nn-cost-us=", 13) == 0) g_nnCostUs = v;
            else if (std::strncmp(a, "--frame-cost-us=", 16) == 0) g_frameCostUs = v;
            else if (std::strncmp(a, "--render-cost-us=", 17) == 0) g_renderCostUs = v;
            else if (std::strncmp(a, "--onset-pattern-bias=", 21) == 0) g_onsetPatternBias = v;
            else if (std::strncmp(a, "--onset-max-frames=", 19) == 0) g_onsetMaxFrames = v;
            else if (std::strncmp(a, "--onset-min-gap=", 16) == 0) g_onsetMinGap = v;
            else if (std::strncmp(a, "--flux-avg-frames=", 18) == 0) g_fluxAvgFrames = v;
            else if (std::strncmp(a, "--flux-lambda=", 14) == 0) g_fluxLambda = v;
            else return usage();
        } else {
            pos.push_back(rawArgv[i]);
        }
    }
    const int argc = static_cast<int>(pos.size());
    char** argv = pos.data();
    const bool costModel = g_nnCostUs > 0 || g_frameCostUs > 0 || g_renderCostUs > 0;
    if (argc < 3) return usage();
    double loopFps = argc > 3 ? std::atof(argv[3]) : 60.0;
    double jitter = argc > 4 ? std::atof(argv[4]) : 0.25;
    unsigned seed = argc > 5 ? static_cast<unsigned>(std::atoi(argv[5])) : 1u;
    if (loopFps <= 0) return usage();

    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::vector<int16_t> pcm;
    int16_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 2, 4096, f)) > 0) pcm.insert(pcm.end(), buf, buf + n);
    std::fclose(f);
    if (pcm.size() < static_cast<size_t>(SAMPLE_RATE)) {
        std::fprintf(stderr, "input too short (%zu samples)\n", pcm.size());
        return 1;
    }

    FILE* out = std::fopen(argv[2], "w");
    if (!out) { std::perror(argv[2]); return 1; }
    std::fprintf(out, "t_ms,phase,plp_pulse,pulse,onset,beat_count,bpm,period_f,rhythm,frames,level,energy\n");

    HostTime time;
    HostPdm pdm;
    pdm.pcm = &pcm;
    AudioTracker tracker(pdm, time);
    tracker.begin(SAMPLE_RATE);
    // Any firmware assert after begin() is a real failure: the firmware's
    // fallbacks keep outputs finite, so without this check a tripped
    // BLINKY_ASSERT would pass silently.
    const uint16_t assertsAfterBegin = BlinkyAssert::failCount;
    // Optional tuning overrides; "-" keeps the firmware default for that slot.
    auto given = [&](int i) { return argc > i && std::strcmp(argv[i], "-") != 0; };
    if (given(6)) tracker.plpPhaseGain = static_cast<float>(std::atof(argv[6]));
    if (given(7)) tracker.tempoPriorWidth = static_cast<float>(std::atof(argv[7]));
    if (given(8)) tracker.syncopatedCandidateScale = static_cast<float>(std::atof(argv[8]));
    if (given(9)) tracker.pulseOnsetFloor = static_cast<float>(std::atof(argv[9]));
    if (given(10)) tracker.fluxDelta = static_cast<float>(std::atof(argv[10]));
    if (argc > 11) {
        std::fprintf(stderr, "slots 11-12 (pulseMinLevel, pulseThresholdMult) were removed with the online picker (#280)\n");
        return 2;
    }
    if (g_onsetPatternBias >= 0) tracker.onsetPatternBias = static_cast<float>(g_onsetPatternBias);
    if (g_onsetMaxFrames >= 0) tracker.onsetMaxFrames = static_cast<uint8_t>(g_onsetMaxFrames);
    if (g_onsetMinGap >= 0) tracker.onsetMinGapFrames = static_cast<uint8_t>(g_onsetMinGap);
    if (g_fluxAvgFrames >= 0) tracker.fluxAverageFrames = static_cast<uint8_t>(g_fluxAvgFrames);
    if (g_fluxLambda >= 0) tracker.getSpectral().superFluxLambda = static_cast<float>(g_fluxLambda);
    if (g_onsetDetector && !tracker.selectOnsetDetector(g_onsetDetector)) {
        std::fprintf(stderr, "unknown or unavailable onset detector: %s\n", g_onsetDetector);
        return 2;
    }
    if (g_frameFeatures) {
        FrameDump::out = std::fopen(g_frameFeatures, "w");
        if (!FrameDump::out) {
            std::fprintf(stderr, "cannot open %s\n", g_frameFeatures);
            return 2;
        }
        std::fprintf(FrameDump::out, "t,pulse,act,superflux,flux,bassflux,rawflux,flat,crest,cent,roll,hfc");
        for (int b = 0; b < FrameDump::NB; b++) std::fprintf(FrameDump::out, ",e%d,f%d", b, b);
        std::fputc('\n', FrameDump::out);
        FrameDump::tracker = &tracker;
        tracker.setFrameHook(FrameDump::hook, nullptr);
    }

    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    const double meanPeriodUs = 1e6 / loopFps;
    uint64_t lastUs = 0;
    g_time = &time;
    g_pdm = &pdm;
    g_nextBlockUs = static_cast<uint64_t>(PDM_BLOCK * 1e6 / SAMPLE_RATE);
    if (costModel) HostCost::onWork = onWork;
    uint32_t prevFrames = 0;
    uint64_t loops = 0;

    while (g_more) {
        double p = costModel ? g_renderCostUs * (1.0 + jitter * nd(rng))
                             : meanPeriodUs * (1.0 + jitter * nd(rng));
        if (p < 1000.0) p = 1000.0;  // vTaskDelay(1) floor
        time.us += static_cast<uint64_t>(p);
        // PDM DMA completes on its own clock regardless of the loop.
        deliverDueBlocks();
        if (!g_more) break;
        loops++;
        float dt = static_cast<float>((time.us - lastUs) * 1e-6);
        lastUs = time.us;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f;   // same clamp as the firmware loop
        const AudioControl& c = tracker.update(dt);
        uint32_t frames = tracker.getSpectral().getFrameCount();
        std::fprintf(out, "%.3f,%.5f,%.4f,%.4f,%.4f,%u,%.2f,%.3f,%.4f,%u,%.4f,%.4f\n",
                     time.us / 1000.0, c.phase, c.plpPulse, c.pulse,
                     c.onset, tracker.getBeatCount(),  // delivered (gated) onset, as generators see it
                     tracker.getCurrentBpm(), tracker.getPlpPeriodF(), c.rhythmStrength,
                     frames - prevFrames, tracker.getMicForTuning().getLevel(), c.energy);
        prevFrames = frames;
    }
    std::fclose(out);
    if (FrameDump::out) std::fclose(FrameDump::out);
    if (costModel) {
        const double secs = time.us / 1e6;
        std::fprintf(stderr, "[COST] loops=%llu sim_s=%.1f fps=%.1f nn_per_s=%.1f nn_cpu=%.0f%%\n",
                     static_cast<unsigned long long>(loops), secs, loops / secs,
                     g_nnCount / secs, 100.0 * g_nnCount * g_nnCostUs / time.us);
    }
#ifdef BLINKY_HOST_NN
    // With the real NN linked, a model that initialized must actually have
    // inferred, and every Invoke() must have succeeded: FrameOnsetNN logs a
    // failed Invoke once and returns 0, which would otherwise pass silently.
    // This build exists to run the real model, so a model refused at begin()
    // (e.g. width mismatch, initError 5) is a hard failure too: otherwise the
    // run silently measures the fallback ODF.
    {
        const FrameOnsetNN& nn = tracker.getFrameOnsetNN();
        if (!nn.isReady()) {
            std::fprintf(stderr, "[HARNESS] onset NN refused at begin(): the embedded model did not "
                                 "initialize (see the [ASSERT] line on stdout)\n");
            return 5;
        }
        if (nn.isReady() && (nn.getInvokeErrors() != 0 || nn.getInferCount() == 0)) {
            std::fprintf(stderr, "[HARNESS] onset NN ready but invokeErrors=%u inferCount=%u\n",
                         static_cast<unsigned>(nn.getInvokeErrors()),
                         static_cast<unsigned>(nn.getInferCount()));
            return 4;
        }
    }
#endif
    const unsigned fired = static_cast<unsigned>(BlinkyAssert::failCount - assertsAfterBegin);
    if (fired) {
        std::fprintf(stderr, "[HARNESS] %u firmware assert(s) fired during the run (see [ASSERT] lines on stdout)\n", fired);
        return 3;
    }
    return 0;
}
