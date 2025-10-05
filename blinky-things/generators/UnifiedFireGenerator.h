#pragma once

#include "../core/Generator.h"
#include "../core/EffectMatrix.h"
#include "../config/TotemDefaults.h"
#include "../devices/DeviceConfig.h"

/**
 * UnifiedFireGenerator - Universal fire simulation for all layout types
 *
 * Generates realistic fire patterns using heat diffusion simulation
 * that adapts to different LED layout arrangements:
 * - MATRIX_LAYOUT: Traditional upward heat propagation for 2D matrices
 * - LINEAR_LAYOUT: Lateral heat propagation for strings/linear arrangements
 * - RANDOM_LAYOUT: Omnidirectional heat propagation for scattered layouts
 *
 * Key features:
 * - Layout-aware heat propagation algorithms
 * - Audio-reactive spark generation
 * - Configurable cooling and spark parameters
 * - Realistic fire color palette
 * - Automatic algorithm selection based on layout type
 */

struct UnifiedFireParams {
    uint8_t baseCooling         = Defaults::BaseCooling;
    uint8_t sparkHeatMin        = Defaults::SparkHeatMin;
    uint8_t sparkHeatMax        = Defaults::SparkHeatMax;
    float   sparkChance         = Defaults::SparkChance;
    float   audioSparkBoost     = Defaults::AudioSparkBoost;
    uint8_t audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
    int8_t  coolingAudioBias    = Defaults::CoolingAudioBias;
    uint8_t bottomRowsForSparks = Defaults::BottomRowsForSparks;
    uint8_t transientHeatMax    = Defaults::TransientHeatMax;

    // Layout-specific parameters
    uint8_t spreadDistance      = 12;     // Heat spread distance for linear/random layouts
    float   heatDecay          = 0.92f;   // Heat decay factor for linear layouts
    uint8_t maxSparkPositions  = 3;      // Max simultaneous spark positions
    bool    useMaxHeatOnly     = false;   // Use max heat instead of additive (linear layouts)
};

class UnifiedFireGenerator : public Generator {
public:
    UnifiedFireGenerator();
    virtual ~UnifiedFireGenerator();

    // Generator interface implementation
    virtual void generate(EffectMatrix& matrix, float energy = 0.0f, float hit = 0.0f) override;
    virtual void reset() override;
    virtual const char* getName() const override { return "UnifiedFireGenerator"; }

    // UnifiedFireGenerator specific methods
    bool begin(int width, int height);
    bool begin(int width, int height, LayoutType layoutType);
    void update();
    void setAudioInput(float energy, bool hit);

    // Layout configuration
    void setLayoutType(LayoutType layoutType);
    void setOrientation(MatrixOrientation orientation);

    // Parameter configuration
    void setParams(const UnifiedFireParams& params);
    void resetToDefaults();

    // Individual parameter setters
    void setBaseCooling(uint8_t cooling);
    void setSparkParams(uint8_t heatMin, uint8_t heatMax, float chance);
    void setAudioParams(float sparkBoost, uint8_t heatBoostMax, int8_t coolingBias);

private:
    // Layout-specific heat propagation algorithms
    void updateMatrixFire();     // Traditional 2D upward propagation
    void updateLinearFire();     // 1D lateral propagation
    void updateRandomFire();     // Omnidirectional propagation

    // Helper functions
    void generateSparks();
    void propagateHeat();
    void applyCooling();
    uint32_t heatToColor(uint8_t heat);
    int coordsToIndex(int x, int y);
    void indexToCoords(int index, int& x, int& y);

    // State variables
    int width_, height_;
    int numLeds_;
    uint8_t* heat_;
    unsigned long lastUpdateMs_;

    // Configuration
    LayoutType layoutType_;
    MatrixOrientation orientation_;
    UnifiedFireParams params_;

    // Audio input
    float audioEnergy_;
    bool audioHit_;

    // Layout-specific state
    uint8_t* sparkPositions_;   // For random layout spark tracking
    uint8_t numActivePositions_;
};

// Factory function to create and configure generator based on device config
UnifiedFireGenerator* createFireGenerator(const DeviceConfig& config);
