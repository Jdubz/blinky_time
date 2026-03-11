#pragma once

#include "DetectionResult.h"
#include "SharedSpectralAnalysis.h"
#include "BassSpectralAnalysis.h"
#include "EnsembleFusion.h"
#include "detectors/BandWeightedFluxDetector.h"

/**
 * EnsembleDetector - Main orchestrator for onset detection
 *
 * Runs BandWeightedFlux detector and passes result through EnsembleFusion.
 * Previously supported 7 concurrent detectors with weighted fusion;
 * 6 disabled detectors (Drummer, SpectralFlux, HFC, BassBand,
 * ComplexDomain, Novelty) were removed in Mar 2026 to save ~1.5 KB RAM.
 *
 * Architecture:
 * 1. Receive audio samples from AdaptiveMic
 * 2. Run SharedSpectralAnalysis once (FFT, magnitudes, phases, mel bands)
 * 3. Run BandWeightedFlux detector
 * 4. Pass result through EnsembleFusion (solo fast path)
 * 5. Return unified EnsembleOutput
 *
 * Memory: ~3.5KB (spectral analysis + 1 detector + fusion)
 * CPU: ~4% at 60Hz (FFT is shared, detector is lightweight)
 */
class EnsembleDetector {
public:
    EnsembleDetector();

    /**
     * Initialize the ensemble detector
     * Must be called before use
     */
    void begin();

    /**
     * Reset all detector state
     * Call when switching modes or after silence
     */
    void reset();

    /**
     * Feed audio samples to the spectral analyzer
     * Call this with raw PCM samples from the microphone
     * @param samples Pointer to int16_t sample buffer
     * @param count Number of samples
     * @return true if a new FFT frame is ready for processing
     */
    bool addSamples(const int16_t* samples, int count);

    /**
     * Update detector and fuse results
     * Call this once per frame (~60Hz)
     * @param level Normalized audio level (0-1) from AdaptiveMic
     * @param rawLevel Raw ADC level
     * @param timestampMs Current timestamp in milliseconds
     * @param dt Time since last frame in seconds
     * @return Combined ensemble output
     */
    EnsembleOutput update(float level, float rawLevel, uint32_t timestampMs, float dt);

    // --- Accessor for fusion engine ---
    EnsembleFusion& getFusion() { return fusion_; }
    const EnsembleFusion& getFusion() const { return fusion_; }

    // --- Accessor for spectral analysis ---
    SharedSpectralAnalysis& getSpectral() { return spectral_; }
    const SharedSpectralAnalysis& getSpectral() const { return spectral_; }

    // --- Accessor for bass spectral analysis ---
    BassSpectralAnalysis& getBassSpectral() { return bassSpectral_; }
    const BassSpectralAnalysis& getBassSpectral() const { return bassSpectral_; }

    // --- Accessor for BandFlux detector ---
    BandWeightedFluxDetector& getBandFlux() { return bandFlux_; }
    const BandWeightedFluxDetector& getBandFlux() const { return bandFlux_; }

    // --- Last result (for debugging/streaming) ---
    const DetectionResult& getLastBandFluxResult() const { return lastBandFluxResult_; }
    const EnsembleOutput& getLastOutput() const { return lastOutput_; }

    // --- Configuration (only BAND_FLUX type is supported) ---
    // Non-BAND_FLUX types were removed in v62. Setters for other types are
    // intentional no-ops — fusion config array retains all enum slots for
    // compatibility, but only BandFlux has a backing detector object.
    void setDetectorWeight(DetectorType type, float weight);
    void setDetectorEnabled(DetectorType type, bool enabled);
    void setDetectorThreshold(DetectorType type, float threshold);

    // --- Status ---
    bool isSpectralReady() const { return spectral_.isFrameReady(); }
    float getTotalEnergy() const { return spectral_.getTotalEnergy(); }
    float getSpectralCentroid() const { return spectral_.getSpectralCentroid(); }

private:
    // Shared spectral analysis (runs FFT once per frame)
    SharedSpectralAnalysis spectral_;

    // High-resolution bass analysis (Goertzel 512-sample, 12 bins)
    BassSpectralAnalysis bassSpectral_;

    // BandWeightedFlux detector (only active detector)
    BandWeightedFluxDetector bandFlux_;

    // Fusion engine
    EnsembleFusion fusion_;

    // Last frame results
    DetectionResult lastBandFluxResult_;
    EnsembleOutput lastOutput_;

    // Build AudioFrame from current state
    AudioFrame buildFrame(float level, float rawLevel, uint32_t timestampMs) const;
};
