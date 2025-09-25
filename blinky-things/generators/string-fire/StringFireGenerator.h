#pragma once

#include "../../core/Generator.h"
#include "../../core/EffectMatrix.h"
#include "../../config/TotemDefaults.h"

/**
 * StringFireGenerator - Fire simulation for linear LED arrangements
 * 
 * Generates fire patterns optimized for string/linear LED arrangements
 * where heat propagates laterally instead of upward. Used for:
 * - Hat installations (circular strings)
 * - LED strips (linear arrangements)
 * - Single-row installations
 * 
 * Key differences from MatrixFireGenerator:
 * - Heat dissipates sideways (laterally) instead of upward
 * - Multiple sparks use maximum heat value, not additive combination
 * - Optimized for linear arrangements where "up" doesn't make sense
 * - Sparks can originate from multiple positions along the string
 */

struct StringFireParams {
    uint8_t baseCooling         = Defaults::BaseCooling;
    uint8_t sparkHeatMin        = Defaults::SparkHeatMin;
    uint8_t sparkHeatMax        = Defaults::SparkHeatMax;
    float   sparkChance         = Defaults::SparkChance;
    float   audioSparkBoost     = Defaults::AudioSparkBoost;
    uint8_t audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
    int8_t  coolingAudioBias    = Defaults::CoolingAudioBias;
    uint8_t sparkSpreadRange    = 3;  // How many pixels sparks can spread
    uint8_t transientHeatMax    = Defaults::TransientHeatMax;
};

class StringFireGenerator : public Generator {
public:
    StringFireGenerator(int length);
    ~StringFireGenerator();

    // Generator interface  
    void generate(EffectMatrix& matrix, float energy = 0.0f, float hit = 0.0f) override;
    void reset() override;
    
    // Configuration
    void setParams(const StringFireParams& newParams) { params = newParams; }
    StringFireParams& getParams() { return params; }
    const StringFireParams& getParams() const { return params; }
    
    // Heat access for debugging/visualization
    float getHeat(int index) const;

private:
    int length;
    float* heat;  // Heat simulation array
    unsigned long lastUpdateMs;
    StringFireParams params;
    
    // Heat simulation methods
    void coolCells();
    void propagateLateral();
    void injectSparks(float energy);
    
    // Color conversion
    uint32_t heatToColorRGB(float heat) const;
};