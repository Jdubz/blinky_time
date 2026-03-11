#include "EnsembleDetector.h"

EnsembleDetector::EnsembleDetector()
    : lastBandFluxResult_(DetectionResult::none())
{
}

void EnsembleDetector::begin() {
    // Initialize spectral analysis
    spectral_.begin();
    bassSpectral_.begin();

    // Configure BandFlux detector with fusion defaults
    DetectorConfig config = fusion_.getConfig();
    bandFlux_.configure(config);
}

void EnsembleDetector::reset() {
    // Reset spectral analysis
    spectral_.reset();
    bassSpectral_.reset();

    // Reset detector
    bandFlux_.reset();

    // Reset fusion (back to calibrated defaults)
    fusion_.resetToDefaults();

    // Reset last results
    lastBandFluxResult_ = DetectionResult::none();
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

    // Build frame structure for detector
    AudioFrame frame = buildFrame(level, rawLevel, timestampMs);

    // Run BandFlux detector (only enabled detector)
    if (fusion_.getConfig().enabled) {
        lastBandFluxResult_ = bandFlux_.detect(frame, dt);
    } else {
        lastBandFluxResult_ = DetectionResult::none();
    }

    // Clear spectral frame ready flags (detector has consumed data)
    spectral_.resetFrameReady();
    bassSpectral_.resetFrameReady();

    // Solo fusion: pass BandFlux result directly (no 7-element array needed)
    lastOutput_ = fusion_.fuseSolo(lastBandFluxResult_,
                                   static_cast<int>(DetectorType::BAND_FLUX),
                                   timestampMs, level);

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
    frame.preWhitenMagnitudes = spectral_.getPreWhitenMagnitudes();
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

void EnsembleDetector::setDetectorWeight(DetectorType, float weight) {
    fusion_.setWeight(weight);
    DetectorConfig config = bandFlux_.getConfig();
    config.weight = weight;
    bandFlux_.configure(config);
}

void EnsembleDetector::setDetectorEnabled(DetectorType, bool enabled) {
    fusion_.setEnabled(enabled);
    DetectorConfig config = bandFlux_.getConfig();
    config.enabled = enabled;
    bandFlux_.configure(config);
}

void EnsembleDetector::setDetectorThreshold(DetectorType, float threshold) {
    // Update detector's own config
    DetectorConfig config = bandFlux_.getConfig();
    config.threshold = threshold;
    bandFlux_.configure(config);

    // Also update fusion config (for display consistency)
    DetectorConfig fusionConfig = fusion_.getConfig();
    fusionConfig.threshold = threshold;
    fusion_.configureDetector(fusionConfig);
}
