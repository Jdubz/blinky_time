#pragma once

#include <stdint.h>

/**
 * DetectionResult - Output from a single onset detector
 *
 * Each detector produces a DetectionResult indicating:
 * - Whether it detected a transient (detected flag)
 * - How strong the transient appears (strength)
 * - How confident the detector is in its assessment (confidence)
 *
 * BandFlux Solo detector feeds into EnsembleFusion for cooldown and noise gate.
 */
struct DetectionResult {
    float strength;      // 0.0-1.0: How strong the transient appears
    float confidence;    // 0.0-1.0: How reliable this detection is
    bool detected;       // True if strength > detector's threshold

    // Default constructor - no detection
    DetectionResult()
        : strength(0.0f)
        , confidence(0.0f)
        , detected(false)
    {}

    // Explicit constructor
    DetectionResult(float s, float c, bool d)
        : strength(s)
        , confidence(c)
        , detected(d)
    {}

    // Create a "no detection" result
    static DetectionResult none() {
        return DetectionResult(0.0f, 0.0f, false);
    }

    // Create a detection with given strength and confidence
    static DetectionResult hit(float strength, float confidence) {
        return DetectionResult(strength, confidence, true);
    }
};

/**
 * DetectorConfig - Per-detector tuning parameters
 *
 * Each detector has its own configuration controlling:
 * - weight: Base contribution to ensemble (calibrated offline)
 * - threshold: Detection sensitivity (used by detector internally, e.g. BandFlux additive threshold)
 * - enabled: Runtime enable/disable without changing weights
 *
 * Note: EnsembleFusion::minConfidence is a separate post-detection confidence
 * gate applied after the detector fires. The detector's threshold controls when
 * it fires; minConfidence controls whether the fusion layer accepts the detection.
 */
struct DetectorConfig {
    float weight;        // 0.0-1.0: Base contribution weight (sum across all should = 1.0)
    float threshold;     // Detection threshold (meaning varies by detector)
    bool enabled;        // If false, detector is skipped entirely (no CPU usage)

    DetectorConfig()
        : weight(0.0f)
        , threshold(1.0f)
        , enabled(true)
    {}

    DetectorConfig(float w, float t, bool e = true)
        : weight(w)
        , threshold(t)
        , enabled(e)
    {}
};

/**
 * EnsembleOutput - Detection result from BandFlux solo detector + fusion
 */
struct EnsembleOutput {
    float transientStrength;   // 0.0-1.0: Detection strength (post-cooldown)
    float ensembleConfidence;  // 0.0-1.0: Detection confidence
    uint8_t detectorAgreement; // 0 or 1 (solo detector)
    uint8_t dominantDetector;  // Always 0 (BAND_FLUX)

    EnsembleOutput()
        : transientStrength(0.0f)
        , ensembleConfidence(0.0f)
        , detectorAgreement(0)
        , dominantDetector(0)
    {}

    bool hasDetection() const {
        return detectorAgreement > 0;
    }
};

/**
 * AudioFrame - Input data for detectors
 *
 * Contains the raw audio data and derived features that detectors use.
 * The SharedSpectralAnalysis class populates this structure.
 */
struct AudioFrame {
    // Time-domain data
    float level;              // Normalized audio level (0-1)
    float rawLevel;           // Raw ADC level before normalization
    uint32_t timestampMs;     // Frame timestamp

    // Spectral data (from SharedSpectralAnalysis)
    const float* magnitudes;          // FFT magnitude spectrum (128 bins) — compressed + whitened
    const float* preWhitenMagnitudes; // FFT magnitude spectrum (128 bins) — raw, no compression or whitening
    const float* phases;      // FFT phase spectrum (128 bins)
    const float* melBands;    // Mel-scaled bands (26 bands)
    int numBins;              // Number of FFT bins (128)
    int numMelBands;          // Number of mel bands (26)
    bool spectralValid;       // True if spectral data is valid this frame

    // High-resolution bass data (from BassSpectralAnalysis, Goertzel 512-sample window)
    const float* bassMagnitudes;  // 12 bins at 31.25 Hz/bin (nullptr if disabled)
    int numBassBins;              // 12 when available, 0 when disabled
    bool bassSpectralValid;       // True when high-res bass data is valid

    AudioFrame()
        : level(0.0f)
        , rawLevel(0.0f)
        , timestampMs(0)
        , magnitudes(nullptr)
        , preWhitenMagnitudes(nullptr)
        , phases(nullptr)
        , melBands(nullptr)
        , numBins(0)
        , numMelBands(0)
        , spectralValid(false)
        , bassMagnitudes(nullptr)
        , numBassBins(0)
        , bassSpectralValid(false)
    {}
};

/**
 * Detector type enumeration
 * BandFlux Solo — 6 disabled detectors removed Mar 2026 (v64).
 */
enum class DetectorType : uint8_t {
    BAND_FLUX = 0,      // Log-compressed band-weighted spectral flux (only active detector)
    COUNT = 1
};

inline const char* getDetectorName(DetectorType) {
    return "bandflux";
}

/**
 * Parse detector name to type. Only "bandflux"/"b"/"bf"/"f" are valid.
 */
inline bool parseDetectorType(const char* str, DetectorType& type) {
    if (!str) return false;
    char c = str[0];
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c == 'b' || c == 'f') {
        type = DetectorType::BAND_FLUX;
        return true;
    }
    return false;
}
