// Absolute-timing host test (#259, #260, #261).
//
// Animations and the rhythm phase must keep absolute time: a late frame
// lands where it belongs in time instead of stretching the animation. This
// checks the timing helpers and, with the simulator's controllable millis(),
// that a generator given one 2000 ms frame ends with the same time-driven
// state as one given two hundred 10 ms frames.
//
// Build (from the repo root; CI: the "timing" job in pr-validation.yml):
//   g++ -std=c++17 -Wall -Wextra -Werror -O1 -DARDUINO_ARCH_NRF52 -DBLINKY_PLATFORM_NRF52840 \
//     -I blinky-simulator/include -I blinky-things tests/timing/timing_test.cpp \
//     blinky-things/types/PixelMatrix.cpp blinky-things/types/BlinkyAssert.cpp \
//     blinky-things/physics/PhysicsContext.cpp blinky-things/particles/Particle.cpp \
//     blinky-things/math/SimplexNoise.cpp blinky-things/generators/Fire.cpp \
//     blinky-things/generators/Water.cpp blinky-things/generators/PlasmaGlobe.cpp \
//     blinky-things/generators/Audio.cpp -o build/timing_test

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "ArduinoCompat.h"

#include "types/FrameClock.h"
#include "audio/PhaseClock.h"
#include "audio/FakeAudio.h"

// The tests read generator state the classes keep private.
#define private public
#define protected public
#include "generators/Audio.h"
#include "generators/Fire.h"
#include "generators/PlasmaGlobe.h"
#include "generators/Water.h"
#undef private
#undef protected

static int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            failures++;                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
        }                                                    \
    } while (0)

static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

static void testFrameClock() {
    FrameClock c;
    CHECK(c.tick(1000) == 0.0f, "first tick must be 0 (no invented 1/60 s)");
    CHECK(near(c.tick(1010), 0.010f, 1e-6f), "10 ms tick");
    CHECK(near(c.tick(3010), 2.0f, 1e-6f), "a 2 s gap is never clamped");
    c.tick(0xFFFFFFF0u);
    CHECK(near(c.tick(0x10u), 0.032f, 1e-6f), "millis wrap");
    c.restart();
    CHECK(c.tick(5000) == 0.0f, "restart: next tick is 0 again");
}

static void testPhysicsSteps() {
    const float dts[] = {0.0f, 0.001f, 0.016f, 0.05f, 0.051f, 0.1f, 0.7f, 2.0f, 30.0f};
    for (float dt : dts) {
        FrameTiming::Steps s = FrameTiming::physicsSteps(dt);
        const float span = std::min(dt, FrameTiming::MAX_PHYSICS_SPAN_S);
        if (dt == 0.0f) {
            CHECK(s.count == 0, "no time, no steps");
            continue;
        }
        CHECK(near(s.count * s.dt, span, 1e-5f), "dt %.3f: steps cover %.3f, got %d x %.4f", dt, span,
              s.count, s.dt);
        CHECK(s.dt <= FrameTiming::MAX_PHYSICS_STEP_S + 1e-6f, "dt %.3f: step %.4f too long", dt, s.dt);
        CHECK(s.count <= 20, "dt %.3f: %d steps over budget", dt, s.count);
        CHECK(near(FrameTiming::physicsSpan(dt), span, 1e-6f), "physicsSpan(%.3f)", dt);
    }
}

static void testOutputPhase() {
    const float period = 33.0f;  // frames per beat (~113 BPM)
    float prev = -1.0f;
    for (uint32_t s = 0; s <= PhaseClock::MAX_EXTRAPOLATE_SAMPLES; s += 16) {
        const float p = PhaseClock::outputPhase(0.3f, s, period);
        const float analytic = 0.3f + (s / 256.0f) / period;
        CHECK(near(p, analytic, 1e-5f), "extrapolated phase at %u samples: %f vs %f", s, p, analytic);
        CHECK(p >= prev, "phase must not go backwards between frames");
        prev = p;
    }
    CHECK(near(PhaseClock::outputPhase(0.3f, 100000, period),
               PhaseClock::outputPhase(0.3f, PhaseClock::MAX_EXTRAPOLATE_SAMPLES, period), 1e-6f),
          "extrapolation is capped when the audio path stalls");
    CHECK(PhaseClock::outputPhase(0.99f, 1024, period) < 1.0f,
          "never crosses an accent the tracker hasn't counted");
    CHECK(PhaseClock::outputPhase(0.42f, 512, 0.0f) == 0.42f, "no period yet: phase as is");
}

static void testPhaseAdvance() {
    const float period = 33.0f;
    PhaseClock::Advance a = PhaseClock::advance(0.5f, 1.0f, period);
    CHECK(a.accents == 0 && near(a.phase, 0.5f + 1.0f / period, 1e-6f), "one ordinary frame");
    a = PhaseClock::advance(0.99f, 1.0f, period);
    CHECK(a.accents == 1 && a.phase < 0.1f, "crossing one accent counts it");
    // A 2 s overrun inside one frame (125 frames at 62.5 Hz) crosses ~3.8 beats.
    a = PhaseClock::advance(0.2f, 125.0f, period);
    const float expect = 0.2f + 125.0f / period;
    CHECK(a.accents == static_cast<uint32_t>(expect), "long overrun: %u accents, expected %u", a.accents,
          static_cast<uint32_t>(expect));
    CHECK(near(a.phase, expect - floorf(expect), 1e-4f), "long overrun lands at the right phase");
    // The sanity cap: 60 s of samples is still bounded work and a phase in [0, 1).
    a = PhaseClock::advance(0.0f, PhaseClock::framesFromSamples(PhaseClock::MAX_FRAME_SPAN_SAMPLES), period);
    CHECK(a.phase >= 0.0f && a.phase < 1.0f, "phase stays in [0, 1) after the longest span");
    // Equivalent to the same time in single frames.
    float p = 0.2f;
    uint32_t beats = 0;
    for (int i = 0; i < 125; i++) {
        PhaseClock::Advance s1 = PhaseClock::advance(p, 1.0f, period);
        p = s1.phase;
        beats += s1.accents;
    }
    a = PhaseClock::advance(0.2f, 125.0f, period);
    CHECK(beats == a.accents && near(p, a.phase, 1e-3f), "one 125-frame step == 125 frame steps");
}

static void testFakeAudio() {
    FakeAudio one, many;
    one.enable();
    many.enable();
    // 2.25 s: more than a whole measure (4 beats at 120 BPM), ending
    // mid-beat so float accumulation in the small steps can't flip a beat.
    one.update(2.25f);
    for (int i = 0; i < 225; i++) many.update(0.01f);
    CHECK(one.getControl().beatCount == many.getControl().beatCount,
          "FakeAudio beats: one 2.25 s frame %u vs 225 x 10 ms %u", one.getControl().beatCount,
          many.getControl().beatCount);
    CHECK(near(one.getControl().phase, many.getControl().phase, 1e-3f), "FakeAudio phase after 2.25 s");
}

static DeviceConfig deviceConfig(bool linear) {
    DeviceConfig c;
    c.deviceName = linear ? "Hat" : "Tube";
    c.matrix.width = linear ? 89 : 4;
    c.matrix.height = linear ? 1 : 15;
    c.matrix.ledPin = 0;
    c.matrix.brightness = 255;
    c.matrix.ledType = 0;
    c.matrix.orientation = linear ? HORIZONTAL : VERTICAL;
    c.matrix.layoutType = linear ? LINEAR_LAYOUT : MATRIX_LAYOUT;
    return c;
}

static AudioControl steadyAudio() {
    AudioControl a;
    a.energy = 0.5f;
    a.level = 0.4f;
    a.pulse = 0.0f;
    a.onset = 0.0f;
    a.phase = 0.25f;
    a.rhythmStrength = 0.6f;
    a.plpPulse = 0.5f;
    a.onsetDensity = 3.0f;
    return a;
}

// Run `gen` from t=1000 ms: one frame to start its clock, then frames of
// `stepMs` until `totalMs` have passed.
template <typename G>
static void runFrames(G& gen, PixelMatrix& m, uint32_t stepMs, uint32_t totalMs) {
    const AudioControl a = steadyAudio();
    SimulatorTime::setSimulatedTime(1000);
    gen.generate(m, a);
    for (uint32_t t = stepMs; t <= totalMs; t += stepMs) {
        SimulatorTime::setSimulatedTime(1000 + t);
        m.clear();
        gen.generate(m, a);
    }
}

template <typename G, typename Probe>
static void checkStallEquivalence(const char* name, bool linear, Probe probe) {
    const DeviceConfig cfg = deviceConfig(linear);
    G oneFrame, manyFrames;
    CHECK(oneFrame.begin(cfg) && manyFrames.begin(cfg), "%s: begin", name);
    PixelMatrix m1(cfg.matrix.width, cfg.matrix.height), m2(cfg.matrix.width, cfg.matrix.height);
    runFrames(oneFrame, m1, 2000, 2000);   // one 2000 ms frame
    runFrames(manyFrames, m2, 10, 2000);   // two hundred 10 ms frames
    probe(oneFrame, manyFrames);
}

static void testGenerators() {
    // Time-driven state must match: a 2 s stall lands where 2 s of frames would.
    checkStallEquivalence<PlasmaGlobe>("PlasmaGlobe", false, [](PlasmaGlobe& a, PlasmaGlobe& b) {
        CHECK(near(a.noiseTime_, b.noiseTime_, 1e-4f * std::max(1.0f, b.noiseTime_)),
              "PlasmaGlobe noise time: one 2 s frame %f vs 200 x 10 ms %f", a.noiseTime_, b.noiseTime_);
        CHECK(a.noiseTime_ > 0.0f, "PlasmaGlobe: a 2 s frame advanced its clock");
        for (int i = 0; i < a.numOrbs_; i++) {
            CHECK(std::isfinite(a.orbX_[i]) && std::isfinite(a.orbY_[i]), "orb %d finite", i);
            CHECK(a.orbX_[i] >= 0.0f && a.orbX_[i] <= a.width_ - 1 && a.orbY_[i] >= 0.0f &&
                      a.orbY_[i] <= a.height_ - 1,
                  "orb %d in bounds after a 2 s frame", i);
        }
    });
    for (bool linear : {false, true}) {
        checkStallEquivalence<Fire>("Fire", linear, [linear](Fire& a, Fire& b) {
            CHECK(near(a.noiseTime_, b.noiseTime_, 1e-4f * std::max(1.0f, b.noiseTime_)),
                  "Fire (%s) noise time: %f vs %f", linear ? "linear" : "matrix", a.noiseTime_, b.noiseTime_);
            CHECK(near(a.paletteBias_, b.paletteBias_, 1e-3f), "Fire palette bias: %f vs %f", a.paletteBias_,
                  b.paletteBias_);
        });
        checkStallEquivalence<Water>("Water", linear, [linear](Water& a, Water& b) {
            CHECK(near(a.noiseTime_, b.noiseTime_, 1e-3f * std::max(1.0f, b.noiseTime_)),
                  "Water (%s) noise time: %f vs %f", linear ? "linear" : "matrix", a.noiseTime_, b.noiseTime_);
        });
    }
    checkStallEquivalence<Audio>("Audio", false, [](Audio& a, Audio& b) {
        CHECK(near(a.smoothedEnergy_, b.smoothedEnergy_, 1e-3f), "Audio smoothed energy: %f vs %f",
              a.smoothedEnergy_, b.smoothedEnergy_);
    });
}

int main() {
    testFrameClock();
    testPhysicsSteps();
    testOutputPhase();
    testPhaseAdvance();
    testFakeAudio();
    testGenerators();
    if (failures) {
        std::printf("%d timing check(s) failed\n", failures);
        return 1;
    }
    std::printf("timing: all checks passed\n");
    return 0;
}
