#pragma once

#include "../particles/ParticleGenerator.h"
#include "../physics/BackgroundModel.h"
#include "../types/ColorPalette.h"
#include "../math/SimplexNoise.h"

/**
 * LightningParams - Lightning-specific particle parameters
 */
struct LightningParams {
    // Spawn behavior
    float baseSpawnChance;        // Baseline bolt spawn probability (0-1)
    float audioSpawnBoost;        // Audio reactivity multiplier (0-2)

    // Lifecycle
    uint8_t maxParticles;         // Maximum active particles (1-32, default 32)
    uint8_t defaultLifespan;      // Default particle age in frames (short-lived)
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Bolt appearance
    float boltVelocityMin;        // Minimum bolt speed (LEDs/frame@30FPS)
    float boltVelocityMax;        // Maximum bolt speed (LEDs/frame@30FPS)
    uint8_t fadeRate;             // Intensity decay per frame (0-255)

    // Branching behavior
    uint8_t branchChance;         // Probability of branching per frame (0-100)
    uint8_t branchCount;          // Number of branches per trigger (1-4)
    float branchAngleSpread;      // Angle variation for branches (radians)
    uint8_t branchIntensityLoss;  // Intensity reduction for branches (0-100%)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation for spawn rate (0-1)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)

    // Background
    float backgroundIntensity;    // Noise background brightness (0-1)

    LightningParams() {
        // LIGHTNING EFFECT: Dramatic bright flashing bolts
        baseSpawnChance = 0.15f;  // Regular strikes
        audioSpawnBoost = 0.8f;   // Strong music response
        maxParticles = 40;        // Enough for bolts + branches
        defaultLifespan = 10;     // Quick flash (lightning is fast!)
        intensityMin = 220;       // VERY BRIGHT
        intensityMax = 255;       // MAXIMUM brightness
        musicSpawnPulse = 0.7f;   // Phase modulation
        organicTransientMin = 0.35f;
        backgroundIntensity = 0.15f;  // Visible but subtle background

        boltVelocityMin = 4.0f;   // Not used (bolts are stationary)
        boltVelocityMax = 8.0f;   // Not used (bolts are stationary)
        fadeRate = 30;            // Fast fade - lightning is quick

        branchChance = 35;        // More branching for realism
        branchCount = 2;          // Branches per bolt
        branchAngleSpread = PI / 3.0f;  // 60 degree spread
        branchIntensityLoss = 25;       // Branches 25% dimmer (still bright)
    }
};

/**
 * Lightning - Particle-based lightning generator
 *
 * Features:
 * - Fast-moving bolts with random directions
 * - Branching behavior (particles spawn child particles)
 * - Fast fade for snappy lightning effect
 * - Beat-synced bolt generation in music mode
 */
class Lightning : public ParticleGenerator<40> {
public:
    Lightning();
    virtual ~Lightning() override = default;

    // Generator interface
    bool begin(const DeviceConfig& config) override;
    void generate(PixelMatrix& matrix, const AudioControl& audio) override;
    void reset() override;
    const char* getName() const override { return "Lightning"; }
    GeneratorType getType() const override { return GeneratorType::LIGHTNING; }

    // Parameter access
    void setParams(const LightningParams& params) { params_ = params; }
    const LightningParams& getParams() const { return params_; }
    LightningParams& getParamsMutable() { return params_; }

protected:
    // Physics context initialization
    void initPhysicsContext() override;

    // ParticleGenerator hooks
    void spawnParticles(float dt) override;
    void updateParticle(Particle* p, float dt) override;
    void renderParticle(const Particle* p, PixelMatrix& matrix) override;
    uint32_t particleColor(uint8_t intensity) const override;

private:
    /**
     * Spawn a coherent lightning bolt (connected particle chain)
     */
    void spawnBolt();

    /**
     * Spawn branch particles from parent bolt
     */
    void spawnBranch(const Particle* parent);

    LightningParams params_;
    float noiseTime_;             // Animation time for noise field

    // Lightning-specific physics components
    BackgroundModel* background_;

    // Static buffer for placement new
    uint8_t backgroundBuffer_[64];
};
