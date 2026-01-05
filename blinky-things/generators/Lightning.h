#pragma once

#include "../particles/ParticleGenerator.h"
#include "../types/ColorPalette.h"

/**
 * LightningParams - Lightning-specific particle parameters
 */
struct LightningParams {
    // Spawn behavior
    float baseSpawnChance;        // Baseline bolt spawn probability (0-1)
    float audioSpawnBoost;        // Audio reactivity multiplier (0-2)
    uint8_t maxParticles;         // Pool size (32 typical)

    // Lifecycle
    uint8_t defaultLifespan;      // Default particle age in frames (short-lived)
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Bolt appearance
    float boltVelocityMin;        // Minimum bolt speed (LEDs/sec)
    float boltVelocityMax;        // Maximum bolt speed (LEDs/sec)
    uint8_t fadeRate;             // Intensity decay per frame (0-255)

    // Branching behavior
    uint8_t branchChance;         // Probability of branching per frame (0-100)
    uint8_t branchCount;          // Number of branches per trigger (1-4)
    float branchAngleSpread;      // Angle variation for branches (radians)
    uint8_t branchIntensityLoss;  // Intensity reduction for branches (0-100%)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation for spawn rate (0-1)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)

    LightningParams() {
        baseSpawnChance = 0.15f;
        audioSpawnBoost = 0.5f;
        maxParticles = 32;
        defaultLifespan = 20;  // Short-lived (~0.6 seconds)
        intensityMin = 180;
        intensityMax = 255;
        musicSpawnPulse = 0.6f;
        organicTransientMin = 0.3f;

        boltVelocityMin = 4.0f;
        boltVelocityMax = 8.0f;
        fadeRate = 160;  // Fast fade

        branchChance = 30;
        branchCount = 2;
        branchAngleSpread = PI / 4.0f;  // 45 degree spread
        branchIntensityLoss = 40;  // Branches 40% dimmer
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
class Lightning : public ParticleGenerator<32> {
public:
    Lightning();
    virtual ~Lightning() override = default;

    // Generator interface
    bool begin(const DeviceConfig& config) override;
    const char* getName() const override { return "Lightning"; }
    GeneratorType getType() const override { return GeneratorType::LIGHTNING; }

    // Parameter access
    void setParams(const LightningParams& params) { params_ = params; }
    const LightningParams& getParams() const { return params_; }
    LightningParams& getParamsMutable() { return params_; }

protected:
    // ParticleGenerator hooks
    void spawnParticles(float dt) override;
    void updateParticle(Particle* p, float dt) override;
    void renderParticle(const Particle* p, PixelMatrix& matrix) override;
    uint32_t particleColor(uint8_t intensity) const override;

private:
    /**
     * Spawn branch particles from parent bolt
     */
    void spawnBranch(const Particle* parent);

    LightningParams params_;
};
