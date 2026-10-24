#pragma once

// Onset detectors behind one interface (#280).
//
// A detector turns each spectral frame into onset EVIDENCE: the value of its
// onset detection function (ODF). Everything after that is shared and
// detector-agnostic (AudioTracker::pickOnset): the online peak picker of
// Boeck, Krebs & Schedl (ISMIR 2012), the rhythm-pattern bias, the feature
// snapshot, and every output (onset strength, pulse envelope, onset
// density, rhythm timing). Swapping the
// detector changes only the evidence and the detector's picker constants,
// never how they are used.
//
// Per detector: the ODF, the picker's delta, whether the threshold is
// relative to the ODF's recent mean (averageFrames), and how an ODF value
// maps onto the 0-1 activation scale. That scale is the NN's: a
// probability-like value; flux is mapped onto it (FluxOnsetDetector::
// activation()). Strength at detections on edm/ (host, 0 dB), p5/p50/p95:
// flux 0.38 / 0.69 / 0.88, NN 0.57 / 0.74 / 0.95.

#include <math.h>
#include "SharedSpectralAnalysis.h"
#include "HostCostHook.h"

// Detector tunables, read each frame. They live on AudioTracker (the
// persisted settings surface: ConfigStorage, SerialConsole, tests/tracker)
// and are passed in, so a detector holds only its own running state.
struct OnsetDetectorParams {
    // Spectral flux (SuperFlux)
    float fluxDelta;           // picker delta above the ODF's moving average
    uint8_t fluxAverageFrames; // moving-average window before the frame (W3)
    // NN
    float pulseOnsetFloor;     // activation threshold for peak picking
    bool nnProfile;            // FrameOnsetNN timing profile
};

struct OnsetEvidence {
    // False when this frame produced no new evidence (an expensive detector
    // skipped it to bound CPU per update). The picker then does not step:
    // re-feeding a held value would hide real peaks.
    bool fresh;
    float odf;  // the detector's onset detection function, >= 0
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
    // Picker threshold: the ODF must reach (mean of the last averageFrames
    // frames and this one) + delta, the delta scaled by the shared gates.
    // averageFrames 0 = no moving average (a fixed threshold, delta).
    virtual float delta(const OnsetDetectorParams& p) const = 0;
    virtual int averageFrames(const OnsetDetectorParams& p) const = 0;
    // The 0-1 activation of an ODF value, given the moving average and the
    // ungated delta: the onset strength and the pulse-envelope input.
    virtual float activation(float odf, float mean, float delta) const = 0;
};

// --- Spectral flux (SuperFlux) ----------------------------------------------
// SuperFlux (Boeck & Widmer, DAFx-13; SharedSpectralAnalysis::getSuperFlux)
// with the online picker's relative threshold: an onset reaches the mean of
// the last 100 ms (W3 = 6 frames at 62.5 fps) plus delta. edm/ host harness
// (#280; kick + snare labels, +-50 ms, 0 dB): drum F 0.648, on-drum 55%,
// kick recall 78%, snare recall 81%, 4.8 det/s, and 0 det/s on steady
// noise; the band-weighted flux with adaptive baseline, cooldown and 96 ms
// window: 0.580 / 48% / 79% / 59% / 5.2, and 3-6 det/s on noise.
class FluxOnsetDetector : public OnsetDetector {
public:
    // Chosen at the device's level (corpus at -50 dB re mastered, int16),
    // lambda 32768, voting bands <= 5 kHz; at -50 dB, drum F / kick R /
    // snare R / lone hats that trigger: delta 5 0.661 / 85% / 85% / 63% |
    // 6 0.661 / 82% / 81% / 54% | 7 0.657 / 79% / 76% / 45% | 8 0.652 /
    // 76% / 72% / 36% (all 78 bands at delta 10: 0.651 / 79% / 81% / 63%).
    // 7 keeps kick recall and cuts hats by a third: the task is kicks and
    // snares, and hats were what the owner saw firing.
    static constexpr float DELTA_DEFAULT = 7.0f;
    static constexpr uint8_t AVERAGE_FRAMES_DEFAULT = 6;  // 96 ms (Boeck 2012: 100 ms)

    // Activation at the threshold (margin = delta) and its growth above it:
    // 1 - 0.7 * r^-0.9, r = (odf - mean) / delta (0.30 at r = 1, -> 1 far
    // above). Below the threshold, linear to 0, so activation is continuous.
    static constexpr float ACTIVATION_AT_THRESHOLD = 0.30f;
    static constexpr float ACTIVATION_EXPONENT = 0.90f;
    static float activationOfRatio(float r) {
        if (r <= 1.0f) return ACTIVATION_AT_THRESHOLD * (r > 0.0f ? r : 0.0f);
        return 1.0f - (1.0f - ACTIVATION_AT_THRESHOLD) * powf(r, -ACTIVATION_EXPONENT);
    }

    const char* name() const override { return "spectral-flux"; }

    OnsetEvidence process(const SharedSpectralAnalysis& spectral,
                          const OnsetDetectorParams&, bool) override {
        return OnsetEvidence{true, spectral.getSuperFlux()};
    }

    float delta(const OnsetDetectorParams& p) const override { return p.fluxDelta; }
    int averageFrames(const OnsetDetectorParams& p) const override { return p.fluxAverageFrames; }
    float activation(float odf, float mean, float delta) const override {
        return activationOfRatio((odf - mean) / delta);
    }
};

// --- Onset NN ----------------------------------------------------------------
// FrameOnsetNN (TFLite Micro). Every frame is pushed into its input window
// (so the window stays evenly spaced in time); inference runs only when the
// CPU budget allows, and only those frames carry fresh evidence.
template <typename Model>
class NNOnsetDetector : public OnsetDetector {
public:
    NNOnsetDetector() : model_() {}
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

    // The activation is already a probability: a fixed threshold
    // (pulseOnsetFloor), no moving average.
    float delta(const OnsetDetectorParams& p) const override { return p.pulseOnsetFloor; }
    int averageFrames(const OnsetDetectorParams&) const override { return 0; }
    float activation(float odf, float, float) const override { return odf; }

private:
    Model model_;
};
