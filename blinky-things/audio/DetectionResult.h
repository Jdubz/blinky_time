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
 * The ensemble fusion system combines results from all detectors
 * using weighted voting with agreement-based confidence scaling.
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
 * - threshold: Detection sensitivity for this detector
 * - enabled: Runtime enable/disable without changing weights
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
 * EnsembleOutput - Combined result from all detectors
 *
 * The fusion system produces this output by combining all detector results
 * using the A+B hybrid strategy:
 * - Fixed calibrated weights (Option A)
 * - Agreement-based confidence scaling (Option B)
 */
struct EnsembleOutput {
    float transientStrength;   // 0.0-1.0: Weighted combination of detector strengths
    float ensembleConfidence;  // 0.0-1.2: Agreement-scaled confidence
    uint8_t detectorAgreement; // Count of detectors that fired (0-6)
    uint8_t dominantDetector;  // Index of detector with highest contribution

    EnsembleOutput()
        : transientStrength(0.0f)
        , ensembleConfidence(0.0f)
        , detectorAgreement(0)
        , dominantDetector(0)
    {}

    // Check if any detector fired
    bool hasDetection() const {
        return detectorAgreement > 0;
    }

    // Check if multiple detectors agree (higher confidence)
    bool hasAgreement() const {
        return detectorAgreement >= 2;
    }

    // Check if strong consensus (3+ detectors)
    bool hasConsensus() const {
        return detectorAgreement >= 3;
    }

    // Debug accessors
    float getAgreementBoost() const {
        return ensembleConfidence;
    }

    // Defined after DetectorType enum (see below)
    const char* getDominantDetectorName() const;
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
    const float* magnitudes;  // FFT magnitude spectrum (128 bins)
    const float* phases;      // FFT phase spectrum (128 bins)
    const float* melBands;    // Mel-scaled bands (26 bands)
    int numBins;              // Number of FFT bins (128)
    int numMelBands;          // Number of mel bands (26)
    bool spectralValid;       // True if spectral data is valid this frame

    AudioFrame()
        : level(0.0f)
        , rawLevel(0.0f)
        , timestampMs(0)
        , magnitudes(nullptr)
        , phases(nullptr)
        , melBands(nullptr)
        , numBins(0)
        , numMelBands(0)
        , spectralValid(false)
    {}
};

/**
 * Detector type enumeration
 *
 * Used for identifying detectors in logs, configs, and serial commands.
 */
enum class DetectorType : uint8_t {
    DRUMMER = 0,        // Time-domain amplitude spikes
    SPECTRAL_FLUX = 1,  // SuperFlux on mel bands
    HFC = 2,            // High-frequency content (FFT-based)
    BASS_BAND = 3,      // Low-frequency flux (disabled: environmental noise)
    COMPLEX_DOMAIN = 4, // Phase deviation
    NOVELTY = 5,        // Cosine distance spectral novelty
    COUNT = 6           // Total number of detectors
};

/**
 * Get detector name string
 */
inline const char* getDetectorName(DetectorType type) {
    switch (type) {
        case DetectorType::DRUMMER:        return "drummer";
        case DetectorType::SPECTRAL_FLUX:  return "spectral";
        case DetectorType::HFC:            return "hfc";
        case DetectorType::BASS_BAND:      return "bass";
        case DetectorType::COMPLEX_DOMAIN: return "complex";
        case DetectorType::NOVELTY:        return "novelty";
        default:                           return "unknown";
    }
}

/**
 * Parse detector name to type
 * Returns true if valid, false otherwise
 */
inline bool parseDetectorType(const char* str, DetectorType& type) {
    if (!str) return false;

    char c = str[0];
    if (c >= 'A' && c <= 'Z') c += 32;  // tolower

    switch (c) {
        case 'd': type = DetectorType::DRUMMER; return true;
        case 's': type = DetectorType::SPECTRAL_FLUX; return true;
        case 'h': type = DetectorType::HFC; return true;
        case 'b': type = DetectorType::BASS_BAND; return true;
        case 'c': type = DetectorType::COMPLEX_DOMAIN; return true;
        case 'n': type = DetectorType::NOVELTY; return true;
        default: return false;
    }
}

/**
 * EnsembleOutput method implementations
 * (must be defined after DetectorType enum)
 */
inline const char* EnsembleOutput::getDominantDetectorName() const {
    return getDetectorName(static_cast<DetectorType>(dominantDetector));
}
