#pragma once

#include "../../core/Generator.h"
#include "../../core/EffectMatrix.h"
#include "../../config/TotemDefaults.h"

/**
 * MatrixFireGenerator - Fire simulation for 2D matrix arrangements
 *
 * Generates realistic fire patterns using heat diffusion simulation
 * optimized for matrix-style LED arrangements where heat propagates upward.
 *
 * Key features:
 * - Heat propagation in Y-axis (upward)
 * - Audio-reactive spark generation
 * - Configurable cooling and spark parameters
 * - Realistic fire color palette
 */

struct MatrixFireParams {
    uint8_t baseCooling         = Defaults::BaseCooling;
    uint8_t sparkHeatMin        = Defaults::SparkHeatMin;
    uint8_t sparkHeatMax        = Defaults::SparkHeatMax;
    float   sparkChance         = Defaults::SparkChance;
    float   audioSparkBoost     = Defaults::AudioSparkBoost;
    uint8_t audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
    int8_t  coolingAudioBias    = Defaults::CoolingAudioBias;
    uint8_t bottomRowsForSparks = Defaults::BottomRowsForSparks;
    uint8_t transientHeatMax    = Defaults::TransientHeatMax;
};

class MatrixFireGenerator : public Generator {
public:
    MatrixFireGenerator(int width, int height);
    ~MatrixFireGenerator();

    // Generator interface
    void generate(EffectMatrix& matrix, float energy = 0.0f, float hit = 0.0f) override;
    void reset() override;

    // Configuration
    void setParams(const MatrixFireParams& newParams) { params = newParams; }
    MatrixFireParams& getParams() { return params; }
    const MatrixFireParams& getParams() const { return params; }

    // Heat access for debugging/visualization
    float getHeat(int x, int y) const;

private:
    int width, height;
    float* heat;  // Heat simulation grid
    unsigned long lastUpdateMs;
    MatrixFireParams params;

    // Heat simulation methods
    void coolCells();
    void propagateUp();
    void injectSparks(float energy);

    // Color conversion
    uint32_t heatToColorRGB(float heat) const;

    // Helper methods
    inline float& getHeatRef(int x, int y) { return heat[y * width + x]; }
    inline const float& getHeatValue(int x, int y) const { return heat[y * width + x]; }
};
