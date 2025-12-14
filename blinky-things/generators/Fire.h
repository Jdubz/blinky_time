#pragma once

#include "Generator.h"
#include "../config/TotemDefaults.h"

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
    uint8_t audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
    int8_t  coolingAudioBias    = Defaults::CoolingAudioBias;
    uint8_t bottomRowsForSparks = Defaults::BottomRowsForSparks;
    uint8_t transientHeatMax    = Defaults::TransientHeatMax;

    // Layout-specific parameters
    uint8_t spreadDistance      = 12;     // Heat spread distance for linear/random layouts
    float   heatDecay          = 0.92f;   // Heat decay factor for linear layouts
    uint8_t maxSparkPositions   = 16;     // Max simultaneous spark positions
    bool    useMaxHeatOnly      = false;  // Use max heat instead of additive (linear layouts)

    // Tuneable spark count on hits (burst mode)
    uint8_t hitSparkBase        = 2;      // Base sparks on hit
    uint8_t hitSparkMult        = 3;      // Multiplier for hit intensity (base + hit * mult)
    uint8_t burstSparks         = 8;      // Sparks generated on burst
    uint16_t suppressionMs      = 300;    // Suppress sparks for this long after burst

    // Ember noise floor (subtle ambient glow using noise)
    uint8_t emberHeatMax        = 18;     // Maximum ember heat (dim glow)
    float   emberNoiseSpeed     = 0.00033f; // How fast noise shifts (10% faster)
    float   emberAudioScale     = 0.2f;   // How much audio affects ember brightness
};

class Fire : public Generator {
public:
    Fire();
    virtual ~Fire();

    // Generator interface implementation
    virtual bool begin(const DeviceConfig& config) override;
    virtual void generate(PixelMatrix& matrix, float energy = 0.0f, float hit = 0.0f) override;
    virtual void reset() override;
    virtual const char* getName() const override { return "Fire"; }

    // Fire specific methods
    void update();
    void setAudioInput(float energy, float hit);

    // Parameter configuration
    void setParams(const FireParams& params);
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
    void applyEmbers();          // Subtle ambient ember glow
    uint32_t heatToColor(uint8_t heat);
    int coordsToIndex(int x, int y);
    void indexToCoords(int index, int& x, int& y);

    // State variables
    uint8_t* heat_;

    // Configuration
    FireParams params_;

    // Audio input
    float audioEnergy_;
    float audioHit_;
    float prevHit_;              // Previous hit value for edge detection
    uint32_t lastBurstMs_;       // When last burst occurred
    bool inSuppression_;         // Currently suppressing sparks after burst

    // Ember noise state
    float emberNoisePhase_;      // Phase for noise animation

    // Layout-specific state
    uint8_t* sparkPositions_;   // For random layout spark tracking
    uint8_t numActivePositions_;
};
