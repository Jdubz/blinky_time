#include "EnsembleDetector.h"

EnsembleDetector::EnsembleDetector() {
    // Initialize detector array (order matches DetectorType enum)
    detectors_[static_cast<int>(DetectorType::DRUMMER)] = &drummer_;
    detectors_[static_cast<int>(DetectorType::SPECTRAL_FLUX)] = &spectralFlux_;
    detectors_[static_cast<int>(DetectorType::HFC)] = &hfc_;
    detectors_[static_cast<int>(DetectorType::BASS_BAND)] = &bassBand_;
    detectors_[static_cast<int>(DetectorType::COMPLEX_DOMAIN)] = &complexDomain_;
    detectors_[static_cast<int>(DetectorType::NOVELTY)] = &novelty_;
    detectors_[static_cast<int>(DetectorType::BAND_FLUX)] = &bandFlux_;

    // Initialize last results
    for (int i = 0; i < NUM_DETECTORS; i++) {
        lastResults_[i] = DetectionResult::none();
    }
}

void EnsembleDetector::begin() {
    // Initialize spectral analysis
    spectral_.begin();
    bassSpectral_.begin();

    // Configure each detector with fusion defaults
    for (int i = 0; i < NUM_DETECTORS; i++) {
        DetectorType type = static_cast<DetectorType>(i);
        DetectorConfig config = fusion_.getConfig(type);
        detectors_[i]->configure(config);
    }
}

void EnsembleDetector::reset() {
    // Reset spectral analysis
    spectral_.reset();
    bassSpectral_.reset();

    // Reset all detectors
    for (int i = 0; i < NUM_DETECTORS; i++) {
        detectors_[i]->reset();
    }

    // Reset fusion (back to calibrated defaults)
    fusion_.resetToDefaults();

    // Reset last results
    for (int i = 0; i < NUM_DETECTORS; i++) {
        lastResults_[i] = DetectionResult::none();
    }
    lastOutput_ = EnsembleOutput();
}

bool EnsembleDetector::addSamples(const int16_t* samples, int count) {
    // Feed both spectral analyzers
    bassSpectral_.addSamples(samples, count);
    return spectral_.addSamples(samples, count);
}

EnsembleOutput EnsembleDetector::update(float level, float rawLevel,
                                         uint32_t timestampMs, float dt) {
    // Process FFT if samples are ready
    if (spectral_.hasSamples()) {
        spectral_.process();
    }

    // Process bass Goertzel if samples are ready
    if (bassSpectral_.hasSamples()) {
        bassSpectral_.process();
    }

    // Build frame structure for detectors
    AudioFrame frame = buildFrame(level, rawLevel, timestampMs);

    // Run only enabled detectors (disabled ones are skipped to save CPU)
    for (int i = 0; i < NUM_DETECTORS; i++) {
        if (fusion_.getConfig(static_cast<DetectorType>(i)).enabled) {
            lastResults_[i] = detectors_[i]->detect(frame, dt);
        } else {
            lastResults_[i] = DetectionResult::none();
        }
    }

    // Clear spectral frame ready flags (detectors have consumed data)
    spectral_.resetFrameReady();
    bassSpectral_.resetFrameReady();

    // Fuse results with unified ensemble cooldown and noise gate
    lastOutput_ = fusion_.fuse(lastResults_, timestampMs, level);

    return lastOutput_;
}

AudioFrame EnsembleDetector::buildFrame(float level, float rawLevel,
                                          uint32_t timestampMs) const {
    AudioFrame frame;

    // Time-domain data
    frame.level = level;
    frame.rawLevel = rawLevel;
    frame.timestampMs = timestampMs;

    // Spectral data (may be null if no FFT this frame)
    frame.spectralValid = spectral_.isFrameReady() || spectral_.hasPreviousFrame();
    frame.magnitudes = spectral_.getMagnitudes();
    frame.phases = spectral_.getPhases();
    frame.melBands = spectral_.getMelBands();
    frame.numBins = spectral_.getNumBins();
    frame.numMelBands = spectral_.getNumMelBands();

    // High-resolution bass data (Goertzel 512-sample window).
    // Uses most recent completed frame — may be from a previous update cycle
    // when new samples haven't accumulated to HOP_SIZE yet. This is expected
    // given the 50% overlap (256-sample hop vs 128-sample FFT frames).
    if (bassSpectral_.enabled && bassSpectral_.hasPreviousFrame()) {
        frame.bassMagnitudes = bassSpectral_.getMagnitudes();
        frame.numBassBins = bassSpectral_.getNumBins();
        frame.bassSpectralValid = true;
    }

    return frame;
}

IDetector* EnsembleDetector::getDetector(DetectorType type) {
    int idx = static_cast<int>(type);
    if (idx >= 0 && idx < NUM_DETECTORS) {
        return detectors_[idx];
    }
    return nullptr;
}

const IDetector* EnsembleDetector::getDetector(DetectorType type) const {
    int idx = static_cast<int>(type);
    if (idx >= 0 && idx < NUM_DETECTORS) {
        return detectors_[idx];
    }
    return nullptr;
}

void EnsembleDetector::setDetectorWeight(DetectorType type, float weight) {
    fusion_.setWeight(type, weight);

    // FIX: Also update detector's own config (matches setDetectorEnabled/setDetectorThreshold)
    int idx = static_cast<int>(type);
    if (idx >= 0 && idx < NUM_DETECTORS) {
        DetectorConfig config = detectors_[idx]->getConfig();
        config.weight = weight;
        detectors_[idx]->configure(config);
    }
}

void EnsembleDetector::setDetectorEnabled(DetectorType type, bool enabled) {
    fusion_.setEnabled(type, enabled);

    // Also update detector's own config
    int idx = static_cast<int>(type);
    if (idx >= 0 && idx < NUM_DETECTORS) {
        DetectorConfig config = detectors_[idx]->getConfig();
        config.enabled = enabled;
        detectors_[idx]->configure(config);
    }
}

void EnsembleDetector::setDetectorThreshold(DetectorType type, float threshold) {
    int idx = static_cast<int>(type);
    if (idx >= 0 && idx < NUM_DETECTORS) {
        // Update detector's own config
        DetectorConfig config = detectors_[idx]->getConfig();
        config.threshold = threshold;
        detectors_[idx]->configure(config);

        // Also update fusion config (for display consistency)
        DetectorConfig fusionConfig = fusion_.getConfig(type);
        fusionConfig.threshold = threshold;
        fusion_.configureDetector(type, fusionConfig);
    }
}
