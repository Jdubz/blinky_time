#pragma once
#include "VisualEffect.h"
#include "EffectMatrix.h"

/**
 * FireVisualEffect - Fire simulation visual effect
 * 
 * Generates realistic fire animation using heat diffusion simulation.
 * This is a refactored version that outputs to EffectMatrix instead
 * of directly to LEDs, enabling easier testing and effect composition.
 */
class FireVisualEffect : public VisualEffect {
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
    
    // Helper functions
    float& getHeatRef(int x, int y);
    float getHeatValue(int x, int y) const;
    RGB heatToColor(float heat) const;
    int wrapX(int x) const;
    int wrapY(int y) const;
    
public:
    FireVisualEffect();
    virtual ~FireVisualEffect();
    
    // VisualEffect interface
    virtual void begin(int width, int height) override;
    virtual void update(float energy, float hit) override;
    virtual void render(EffectMatrix& matrix) override;
    virtual void restoreDefaults() override;
    virtual const char* getName() const override { return "Fire"; }
    
    // Testing helpers
    void setHeat(int x, int y, float heat);
    float getHeat(int x, int y) const;
    void clearHeat();
};