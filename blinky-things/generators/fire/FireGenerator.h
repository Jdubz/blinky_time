#pragma once
#include "../../core/Generator.h"
#include "../../core/EffectMatrix.h"

/**
 * FireGenerator - Fire simulation pattern generator
 *
 * Generates realistic fire animation using heat diffusion simulation.
 * This creates the base fire pattern that can then be modified by effects
 * (hue rotation, brightness modulation, etc.) before rendering.
 *
 * Architecture: FireGenerator -> Effects -> Renderer -> Hardware
 */
class FireGenerator : public Generator {
public:
    // Fire parameters (same as original FireEffect)
    struct FireParams {
        uint8_t baseCooling;
        uint8_t sparkHeatMin;
        uint8_t sparkHeatMax;
        float sparkChance;
        float audioSparkBoost;
        uint8_t audioHeatBoostMax;
        int8_t coolingAudioBias;
        uint8_t bottomRowsForSparks;
        uint8_t transientHeatMax;
    };

    FireParams params;

private:
    int width_;
    int height_;
    float* heat_;
    unsigned long lastUpdateMs_;
    float currentEnergy_;
    float currentHit_;

    // Helper functions
    float& getHeatRef(int x, int y);
    float getHeatValue(int x, int y) const;
    RGB heatToColor(float heat) const;
    int wrapX(int x) const;
    int wrapY(int y) const;

public:
    FireGenerator();
    virtual ~FireGenerator();

    // Generator interface
    virtual void begin(int width, int height) override;
    virtual void generate(EffectMatrix* matrix) override;
    virtual void update() override;
    virtual const char* getName() const override { return "Fire"; }

    // Audio input for fire dynamics
    void setAudioInput(float energy, float hit);

    // Testing helpers
    void setHeat(int x, int y, float heat);
    float getHeat(int x, int y) const;
    void clearHeat();
    void restoreDefaults();
};
