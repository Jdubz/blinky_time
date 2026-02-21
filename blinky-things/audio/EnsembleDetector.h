#pragma once

#include "DetectionResult.h"
#include "IDetector.h"
#include "SharedSpectralAnalysis.h"
#include "EnsembleFusion.h"
#include "detectors/DrummerDetector.h"
#include "detectors/SpectralFluxDetector.h"
#include "detectors/HFCDetector.h"
#include "detectors/BassBandDetector.h"
#include "detectors/ComplexDomainDetector.h"
#include "detectors/NoveltyDetector.h"
#include "detectors/BandWeightedFluxDetector.h"

/**
 * EnsembleDetector - Main orchestrator for ensemble onset detection
 *
 * Runs all detection algorithms simultaneously and combines their results
 * using the A+B hybrid fusion strategy:
 * - Fixed calibrated weights (Option A)
 * - Agreement-based confidence scaling (Option B)
 *
 * Architecture:
 * 1. Receive audio samples from AdaptiveMic
 * 2. Run SharedSpectralAnalysis once (FFT, magnitudes, phases, mel bands)
 * 3. Run enabled detectors (disabled ones are skipped to save CPU)
 * 4. Fuse results using EnsembleFusion
 * 5. Return unified EnsembleOutput
 *
 * This replaces the mutually-exclusive mode switching in AdaptiveMic.
 * All techniques contribute simultaneously with weighted confidence scores.
 *
 * Memory: ~5KB (spectral analysis + 6 detectors + fusion)
 * CPU: ~4% at 60Hz (FFT is shared, detectors are lightweight)
 */
class EnsembleDetector {
public:
    // Number of detectors
    static constexpr int NUM_DETECTORS = static_cast<int>(DetectorType::COUNT);

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
     * Update all detectors and fuse results
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

    // --- Accessor for individual detectors ---
    IDetector* getDetector(DetectorType type);
    const IDetector* getDetector(DetectorType type) const;

    // --- Convenience accessors ---
    DrummerDetector& getDrummer() { return drummer_; }
    SpectralFluxDetector& getSpectralFlux() { return spectralFlux_; }
    HFCDetector& getHFC() { return hfc_; }
    BassBandDetector& getBassBand() { return bassBand_; }
    ComplexDomainDetector& getComplexDomain() { return complexDomain_; }
    NoveltyDetector& getNovelty() { return novelty_; }
    BandWeightedFluxDetector& getBandFlux() { return bandFlux_; }

    // --- Last results (for debugging/streaming) ---
    const DetectionResult* getLastResults() const { return lastResults_; }
    const EnsembleOutput& getLastOutput() const { return lastOutput_; }

    // --- Configuration ---
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

    // Individual detectors
    DrummerDetector drummer_;
    SpectralFluxDetector spectralFlux_;
    HFCDetector hfc_;
    BassBandDetector bassBand_;
    ComplexDomainDetector complexDomain_;
    NoveltyDetector novelty_;
    BandWeightedFluxDetector bandFlux_;

    // Detector array for iteration
    IDetector* detectors_[NUM_DETECTORS];

    // Fusion engine
    EnsembleFusion fusion_;

    // Last frame results
    DetectionResult lastResults_[NUM_DETECTORS];
    EnsembleOutput lastOutput_;

    // Build AudioFrame from current state
    AudioFrame buildFrame(float level, float rawLevel, uint32_t timestampMs) const;
};
