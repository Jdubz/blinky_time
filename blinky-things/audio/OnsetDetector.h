#pragma once

// Onset detectors behind one interface (#280).
//
// A detector turns each spectral frame into onset EVIDENCE: an activation,
// 0-1, on one scale whichever detector produced it. Everything after that is
// shared and detector-agnostic (AudioTracker::pickOnset): local-max peak
// picking, the gates (presence, cooldown, bass ratio, rhythm-pattern bias,
// crest, beat grid), the feature snapshot, and every output (onset strength,
// pulse envelope, onset density, rhythm timing). Swapping the detector
// changes only the evidence, never how it is used.
//
// The activation scale is the NN's: a probability-like value. On edm/ the
// NN's activation at its detections is p5 0.34, p50 0.65, p95 0.97, and flux
// is mapped onto that distribution (FluxOnsetDetector::activation()).

#include <math.h>
#include "SharedSpectralAnalysis.h"
#include "HostCostHook.h"

// Detector tunables, read each frame. They live on AudioTracker (the
// persisted settings surface: ConfigStorage, SerialConsole, tests/tracker)
// and are passed in, so a detector holds only its own running state.
struct OnsetDetectorParams {
    // Spectral flux
    float pulseThresholdMult;  // threshold = adaptive baseline x this
    float fluxOnsetFloor;      // threshold never below this (raw flux units)
    float baselineFastDrop;    // baseline EMA rate when flux is below it
    float baselineSlowRise;    // baseline EMA rate when flux is above it
    // NN
    float pulseOnsetFloor;     // activation threshold for peak picking
    bool nnProfile;            // FrameOnsetNN timing profile
};

struct OnsetEvidence {
    // False when this frame produced no new evidence (an expensive detector
    // skipped it to bound CPU per update). The picker then does not step:
    // re-feeding a held value would hide real peaks.
    bool fresh;
    float activation;  // 0-1, the onset API scale
};

class OnsetDetector {
public:
    virtual ~OnsetDetector() {}
    virtual const char* name() const = 0;
    // Called exactly once per spectral frame, in order. `mayRunExpensive`:
    // the CPU budget allows this frame's expensive work (the NN runs at most
    // once per AudioTracker::update()).
    virtual OnsetEvidence process(const SharedSpectralAnalysis& spectral,
                                  const OnsetDetectorParams& p, bool mayRunExpensive) = 0;
    // Peak-picking threshold in activation space: an activation peak must
    // exceed this (before the shared gates scale it) to be an onset.
    virtual float threshold(const OnsetDetectorParams& p) const = 0;
};

// --- Spectral flux ---------------------------------------------------------
// Band-weighted SuperFlux-style flux (SharedSpectralAnalysis) against an
// adaptive, floor-tracking baseline. Activation = how far the flux is above
// its adaptive threshold, mapped onto the NN's scale.
class FluxOnsetDetector : public OnsetDetector {
public:
    // Activation at the adaptive threshold: the picker's threshold, so a
    // flux peak is an onset exactly when it exceeds the adaptive threshold.
    static constexpr float ACTIVATION_AT_THRESHOLD = 0.30f;
    static constexpr float ACTIVATION_EXPONENT = 0.90f;

    // flux / adaptive threshold -> 0-1. At or above the threshold:
    // 1 - 0.7 * ratio^-0.9 (0.30 at the threshold, -> 1 far above it).
    // Below it, linear to 0, so activation is continuous. Fitted so flux
    // strengths at detections match the NN's distribution (edm/, host
    // harness, 13-14k detections each):
    //   NN strength       p5 0.34  p25 0.47  p50 0.65  p75 0.87  p95 0.97
    //   flux ratio        p5 1.10  p25 1.59  p50 2.63  p75 5.29  p95 18.4
    //   mapped flux       p5 0.36  p25 0.55  p50 0.71  p75 0.85  p95 0.95
    static float activation(float ratio) {
        if (ratio <= 1.0f) return ACTIVATION_AT_THRESHOLD * (ratio > 0.0f ? ratio : 0.0f);
        return 1.0f - (1.0f - ACTIVATION_AT_THRESHOLD) * powf(ratio, -ACTIVATION_EXPONENT);
    }

    const char* name() const override { return "spectral-flux"; }

    OnsetEvidence process(const SharedSpectralAnalysis& spectral,
                          const OnsetDetectorParams& p, bool) override {
        const float signal = spectral.getSpectralFlux();
        if (signal < baseline_) {
            baseline_ += (signal - baseline_) * p.baselineFastDrop;
        } else {
            baseline_ += (signal - baseline_) * p.baselineSlowRise;
        }
        float thr = baseline_ * p.pulseThresholdMult;
        if (thr < p.fluxOnsetFloor) thr = p.fluxOnsetFloor;
        // A zero threshold (floor 0, silent baseline) is passed by any
        // signal at all, so any signal is full evidence.
        const float a = (thr > 0.0f) ? activation(signal / thr) : (signal > 0.0f ? 1.0f : 0.0f);
        return OnsetEvidence{true, a};
    }

    float threshold(const OnsetDetectorParams&) const override { return ACTIVATION_AT_THRESHOLD; }

private:
    float baseline_ = 0.0f;  // floor-tracking baseline of the flux
};

// --- Onset NN ----------------------------------------------------------------
// FrameOnsetNN (TFLite Micro). Every frame is pushed into its input window
// (so the window stays evenly spaced in time); inference runs only when the
// CPU budget allows, and only those frames carry fresh evidence.
template <typename Model>
class NNOnsetDetector : public OnsetDetector {
public:
    const char* name() const override { return "onset-nn"; }

    bool begin() { return model_.begin() && model_.isReady(); }
    Model& model() { return model_; }
    const Model& model() const { return model_; }

    OnsetEvidence process(const SharedSpectralAnalysis& spectral,
                          const OnsetDetectorParams& p, bool mayRunExpensive) override {
        model_.setProfileEnabled(p.nnProfile);
        // Raw (pre-compressor) flatness + flux for the hybrid input — matches
        // the training pipeline's `append_hybrid_features` (pre-b136 passed
        // the compressed values: train/inference gap 4).
        model_.pushFrame(spectral.getRawMelBands(), spectral.getLinearMelBands(),
                         spectral.getRawFlatness(), spectral.getRawSpectralFlux());
        if (!mayRunExpensive) return OnsetEvidence{false, 0.0f};
        const float a = model_.invoke();
        BLINKY_HOST_COST(HostCost::NN_INFER);
        // No smoothing (#169): the picker reads the raw activation.
        return OnsetEvidence{true, a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a)};
    }

    float threshold(const OnsetDetectorParams& p) const override { return p.pulseOnsetFloor; }

private:
    Model model_;
};
