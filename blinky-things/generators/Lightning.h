#pragma once

#include "Generator.h"
#include "../config/TotemDefaults.h"

/**
 * Lightning - Electric lightning simulation generator
 *
 * Generates realistic lightning patterns using branching algorithms
 * that adapts to different LED layout arrangements:
 * - MATRIX_LAYOUT: Branching lightning bolts across 2D matrices
 * - LINEAR_LAYOUT: Lightning bolts along strings/linear arrangements
 * - RANDOM_LAYOUT: Electric arcs between scattered points
 *
 * Key features:
 * - Layout-aware lightning algorithms
 * - Audio-reactive bolt generation
 * - Yellow/white color palette (yellow/white/electric blue)
 * - Realistic branching and fading patterns
 */

struct LightningParams {
    uint8_t baseFade           = 160;  // Base fade speed
    uint8_t boltIntensityMin   = 100;  // Minimum bolt intensity
    uint8_t boltIntensityMax   = 255;  // Maximum bolt intensity
    float   boltChance         = 0.15f; // Chance of new bolt
    float   audioBoltBoost     = 0.5f; // Audio boost for bolts
    uint8_t audioIntensityBoostMax = 100; // Max intensity boost from audio
    int8_t  fadeAudioBias      = -30;  // Fade speed audio bias (negative = slower fade on audio)
    uint8_t maxBoltPositions   = 8;    // For random layout tracking
    uint8_t branchChance       = 30;   // Percentage chance of branching
};

class Lightning : public Generator {
public:
    Lightning();
    virtual ~Lightning() override;

    // Generator interface implementation
    virtual bool begin(const DeviceConfig& config) override;
    virtual void generate(PixelMatrix& matrix, const AudioControl& audio) override;
    virtual void reset() override;
    virtual const char* getName() const override { return "Lightning"; }
    virtual GeneratorType getType() const override { return GeneratorType::LIGHTNING; }

    // Lightning specific methods
    void update();
    void setAudioInput(float energy, float hit);

    // Parameter configuration
    void setParams(const LightningParams& params);
    void resetToDefaults();
    const LightningParams& getParams() const { return params_; }
    LightningParams& getParamsMutable() { return params_; }

    // Individual parameter setters
    void setBaseFade(uint8_t fade);
    void setBoltParams(uint8_t intensityMin, uint8_t intensityMax, float chance);
    void setAudioParams(float boltBoost, uint8_t intensityBoostMax, int8_t fadeBias);

private:
    // Layout-specific lightning algorithms
    void updateMatrixLightning();     // 2D branching bolts
    void updateLinearLightning();     // 1D lightning along string
    void updateRandomLightning();     // Electric arcs between points

    // Helper functions
    void generateBolts();
    void propagateBolts();
    void applyFade();
    uint32_t intensityToColor(uint8_t intensity);
    void createBranch(int startIndex, int direction, uint8_t intensity);
    // Note: coordsToIndex/indexToCoords are inherited from Generator base class

    // State variables
    uint8_t* intensity_;      // Lightning intensity instead of heat
    uint8_t* tempIntensity_;  // Pre-allocated temp buffer for bolt propagation

    // Configuration
    LightningParams params_;

    // Audio input
    float audioEnergy_;
    float audioHit_;

    // Layout-specific state
    uint8_t* boltPositions_;   // For random layout bolt tracking
};
