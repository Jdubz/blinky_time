#pragma once

#include "Generator.h"
#include "../config/TotemDefaults.h"

/**
 * Water - Flowing water simulation generator
 *
 * Generates realistic water patterns using flow simulation
 * that adapts to different LED layout arrangements:
 * - MATRIX_LAYOUT: Downward flow with waves for 2D matrices
 * - LINEAR_LAYOUT: Wave propagation along strings/linear arrangements
 * - RANDOM_LAYOUT: Ripple effects for scattered layouts
 *
 * Key features:
 * - Layout-aware flow algorithms
 * - Audio-reactive wave generation
 * - Blue color palette (cyan/blue/deep blue)
 * - Realistic water movement patterns
 */

struct WaterParams {
    uint8_t baseFlow           = 120;  // Base flow speed
    uint8_t waveHeightMin      = 30;   // Minimum wave height
    uint8_t waveHeightMax      = 180;  // Maximum wave height
    float   waveChance         = 0.25f; // Chance of new wave
    float   audioWaveBoost     = 0.4f; // Audio boost for waves
    uint8_t audioFlowBoostMax  = 80;   // Max flow boost from audio
    int8_t  flowAudioBias      = 15;   // Flow speed audio bias
    uint8_t maxWavePositions   = 12;   // For random layout tracking
};

class Water : public Generator {
public:
    Water();
    virtual ~Water() override;

    // Generator interface implementation
    virtual bool begin(const DeviceConfig& config) override;
    virtual void generate(PixelMatrix& matrix, const AudioControl& audio) override;
    virtual void reset() override;
    virtual const char* getName() const override { return "Water"; }
    virtual GeneratorType getType() const override { return GeneratorType::WATER; }

    // Water specific methods
    void update();
    void setAudioInput(float energy, float hit);

    // Parameter configuration
    void setParams(const WaterParams& params);
    void resetToDefaults();
    const WaterParams& getParams() const { return params_; }
    WaterParams& getParamsMutable() { return params_; }

    // Individual parameter setters
    void setBaseFlow(uint8_t flow);
    void setWaveParams(uint8_t heightMin, uint8_t heightMax, float chance);
    void setAudioParams(float waveBoost, uint8_t flowBoostMax, int8_t flowBias);

private:
    // Layout-specific flow algorithms
    void updateMatrixWater();     // Traditional 2D downward flow
    void updateLinearWater();     // 1D wave propagation
    void updateRandomWater();     // Ripple effects

    // Helper functions
    void generateWaves();
    void propagateFlow();
    void applyFlow();
    uint32_t depthToColor(uint8_t depth);
    // Note: coordsToIndex/indexToCoords are inherited from Generator base class

    // State variables
    uint8_t* depth_;      // Water depth instead of heat
    uint8_t* tempDepth_;  // Pre-allocated temp buffer for flow propagation

    // Configuration
    WaterParams params_;

    // Audio input
    float audioEnergy_;
    float audioHit_;
};
