#pragma once

#include "Generator.h"
#include "../config/TotemDefaults.h"

// Forward declaration
class MusicMode;

/**
 * Fire - Realistic fire simulation generator
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
 * - Realistic fire color palette (red/orange/yellow)
 * - Automatic algorithm selection based on layout type
 */

struct FireParams {
    uint8_t baseCooling         = Defaults::BaseCooling;
    uint8_t sparkHeatMin        = Defaults::SparkHeatMin;
    uint8_t sparkHeatMax        = Defaults::SparkHeatMax;
    float   sparkChance         = Defaults::SparkChance;
    float   audioSparkBoost     = Defaults::AudioSparkBoost;
    int8_t  coolingAudioBias    = Defaults::CoolingAudioBias;
    uint8_t bottomRowsForSparks = Defaults::BottomRowsForSparks;

    // Layout-specific parameters
    uint8_t spreadDistance      = Defaults::SpreadDistance;
    float   heatDecay           = Defaults::HeatDecay;
    uint8_t maxSparkPositions   = 16;     // Max simultaneous spark positions
    bool    useMaxHeatOnly      = false;  // Use max heat instead of additive (linear layouts)

    // Burst mode parameters
    uint8_t burstSparks         = 8;      // Sparks generated on burst
    uint16_t suppressionMs      = 300;    // Suppress sparks for this long after burst

    // Ember noise floor (subtle ambient glow using noise)
    uint8_t emberHeatMax        = Defaults::EmberHeatMax;
    float   emberNoiseSpeed     = Defaults::EmberNoiseSpeed;
};

class Fire : public Generator {
public:
    Fire();
    virtual ~Fire() override;

    // Generator interface implementation
    virtual bool begin(const DeviceConfig& config) override;
    virtual void generate(PixelMatrix& matrix, float energy = 0.0f, float hit = 0.0f) override;
    virtual void reset() override;
    virtual const char* getName() const override { return "Fire"; }

    // Fire specific methods
    void update();
    void setAudioInput(const float energy, const float hit);

    // Parameter configuration
    void setParams(const FireParams& params);
    void resetToDefaults();
    const FireParams& getParams() const { return params_; }
    FireParams& getParamsMutable() { return params_; }  // For SerialConsole and config loading

    // Individual parameter setters
    void setBaseCooling(const uint8_t cooling);
    void setSparkParams(const uint8_t heatMin, const uint8_t heatMax, const float chance);
    void setAudioParams(const float sparkBoost, const int8_t coolingBias);

    // Layout configuration (post-begin adjustments)
    void setLayoutType(LayoutType layoutType);
    void setOrientation(MatrixOrientation orientation);

    // Music mode integration
    void setMusicMode(MusicMode* music);

private:
    MatrixOrientation orientation_ = HORIZONTAL;
    // Layout-specific heat propagation algorithms
    void updateMatrixFire();     // Traditional 2D upward propagation
    void updateLinearFire();     // 1D lateral propagation
    void updateRandomFire();     // Omnidirectional propagation

    // Helper functions
    void generateSparks();
    void propagateHeat();
    void applyCooling();
    void applyEmbers();          // Subtle ambient ember glow
    uint32_t heatToColor(uint8_t heat);
    int coordsToIndex(int x, int y);
    void indexToCoords(int index, int& x, int& y);

    // State variables
    uint8_t* heat_;
    uint8_t* tempHeat_;  // Pre-allocated temp buffer for heat propagation

    // Configuration
    FireParams params_;

    // Audio input
    float audioEnergy_;
    float audioHit_;
    uint32_t lastBurstMs_;       // When last burst occurred
    bool inSuppression_;         // Currently suppressing sparks after burst

    // Ember noise state
    float emberNoisePhase_;      // Phase for noise animation

    // Layout-specific state
    uint8_t* sparkPositions_;   // For random layout spark tracking
    uint8_t numActivePositions_;

    // Music mode integration
    MusicMode* music_;          // Optional music mode for beat-synced effects
};
