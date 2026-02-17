#pragma once

#include "Generator.h"

/**
 * AudioParams - Audio visualization tunable parameters
 */
struct AudioParams {
    // Transient visualization (green gradient from top)
    float transientRowFraction;   // Fraction of height for transient indicator (0-1)
    float transientDecayRate;     // How fast transient fades (0-1 per frame, higher = faster)
    uint8_t transientBrightness;  // Maximum brightness of transient indicator (0-255)

    // Energy level visualization (yellow row)
    uint8_t levelBrightness;      // Brightness of energy level row (0-255)
    float levelSmoothing;         // Smoothing factor for level changes (0-1, higher = smoother)

    // Phase visualization (blue row moving bottom to top)
    uint8_t phaseBrightness;      // Maximum brightness of phase row (0-255)
    float musicModeThreshold;     // Minimum rhythmStrength to show phase indicator (0-1)

    // Background
    uint8_t backgroundBrightness; // Dim background level (0-255)

    AudioParams() {
        // Transient: top 20% of display, visible green flash
        transientRowFraction = 0.2f;
        transientDecayRate = 0.15f;   // Decays over ~6-7 frames
        transientBrightness = 200;

        // Energy: bright yellow row
        levelBrightness = 220;
        levelSmoothing = 0.3f;  // Moderate smoothing

        // Phase: bright blue row when music mode active
        phaseBrightness = 200;
        musicModeThreshold = 0.3f;  // Show phase when rhythm confidence > 30%

        // Background: very dim
        backgroundBrightness = 8;
    }
};

/**
 * Audio - Diagnostic audio visualization generator
 *
 * Designed for cylindrical matrices (visible from all angles).
 * Visualizes key audio analysis data:
 *
 * 1. TRANSIENT (green, top rows): Gradient intensity based on pulse strength
 *    - Top ~20% of display
 *    - Bright flash on transient, fades over time
 *    - Full horizontal wrap (same on all columns)
 *
 * 2. ENERGY LEVEL (yellow, single row): Y position indicates audio energy
 *    - Position: bottom (0 energy) to top (max energy)
 *    - Full brightness horizontal row
 *
 * 3. PHASE (blue, single row): Beat phase position
 *    - Moves from bottom (phase=0, on-beat) to top (phase approaching 1)
 *    - Only visible when music mode active (rhythmStrength > threshold)
 *    - Brightness modulated by rhythm confidence
 *
 * Layout-aware: works on both matrix (2D) and linear (1D) layouts.
 * On linear layouts, uses LED position as proxy for Y coordinate.
 */
class Audio : public Generator {
public:
    Audio();
    virtual ~Audio() override = default;

    // Generator interface
    bool begin(const DeviceConfig& config) override;
    void generate(PixelMatrix& matrix, const AudioControl& audio) override;
    void reset() override;
    const char* getName() const override { return "Audio"; }
    GeneratorType getType() const override { return GeneratorType::AUDIO; }

    // Parameter access
    void setParams(const AudioParams& params) { params_ = params; }
    const AudioParams& getParams() const { return params_; }
    AudioParams& getParamsMutable() { return params_; }

private:
    AudioParams params_;

    // Smoothed state for visualization
    float smoothedEnergy_;        // Smoothed energy level (0-1)
    float transientIntensity_;    // Current transient intensity (0-1), decays over time

    // Helper methods
    void drawTransientRows(PixelMatrix& matrix, float pulse);
    void drawEnergyRow(PixelMatrix& matrix, float energy);
    void drawPhaseRow(PixelMatrix& matrix, float phase, float rhythmStrength);
    void drawBackground(PixelMatrix& matrix);

    // Utility: set a full horizontal row to a color
    void setRow(PixelMatrix& matrix, int y, uint8_t r, uint8_t g, uint8_t b);
};
