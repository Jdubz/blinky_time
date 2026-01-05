#pragma once

#include "../particles/ParticleGenerator.h"
#include "../types/ColorPalette.h"

/**
 * WaterParams - Water-specific particle parameters
 */
struct WaterParams {
    // Spawn behavior
    float baseSpawnChance;        // Baseline drop spawn probability (0-1)
    float audioSpawnBoost;        // Audio reactivity multiplier (0-2)
    uint8_t maxParticles;         // Pool size (64 typical)

    // Lifecycle
    uint8_t defaultLifespan;      // Default particle age in frames
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Physics
    float gravity;                // Gravity strength (LEDs/sec^2, positive = down)
    float windBase;               // Base wind force (LEDs/sec^2)
    float windVariation;          // Wind variation amount (LEDs/sec^2)
    float drag;                   // Drag coefficient (0-1)

    // Drop appearance
    float dropVelocityMin;        // Minimum downward velocity (LEDs/sec)
    float dropVelocityMax;        // Maximum downward velocity (LEDs/sec)
    float dropSpread;             // Horizontal velocity variation (LEDs/sec)

    // Splash behavior
    uint8_t splashParticles;      // Number of particles spawned on splash (0-10)
    float splashVelocityMin;      // Minimum splash velocity (LEDs/sec)
    float splashVelocityMax;      // Maximum splash velocity (LEDs/sec)
    uint8_t splashIntensity;      // Splash particle intensity multiplier (0-255)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation for spawn rate (0-1)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)

    WaterParams() {
        baseSpawnChance = 0.25f;
        audioSpawnBoost = 0.4f;
        maxParticles = 64;
        defaultLifespan = 90;  // ~3 seconds at 30 FPS
        intensityMin = 80;
        intensityMax = 200;
        gravity = 5.0f;  // Positive = downward
        windBase = 0.0f;
        windVariation = 0.3f;
        drag = 0.99f;  // Low drag (water flows smoothly)
        musicSpawnPulse = 0.5f;
        organicTransientMin = 0.3f;

        dropVelocityMin = 0.5f;
        dropVelocityMax = 1.5f;
        dropSpread = 0.3f;

        splashParticles = 6;
        splashVelocityMin = 0.5f;
        splashVelocityMax = 2.0f;
        splashIntensity = 120;
    }
};

/**
 * Water - Particle-based water generator
 *
 * Features:
 * - Drops falling from top (primary behavior)
 * - Radial splashes on impact (transient-triggered)
 * - Beat-synced wave generation in music mode
 * - Smooth physics-based motion
 */
class Water : public ParticleGenerator<64> {
public:
    Water();
    virtual ~Water() override = default;

    // Generator interface
    bool begin(const DeviceConfig& config) override;
    const char* getName() const override { return "Water"; }
    GeneratorType getType() const override { return GeneratorType::WATER; }

    // Parameter access
    void setParams(const WaterParams& params) { params_ = params; }
    const WaterParams& getParams() const { return params_; }
    WaterParams& getParamsMutable() { return params_; }

protected:
    // ParticleGenerator hooks
    void spawnParticles(float dt) override;
    void updateParticle(Particle* p, float dt) override;
    void renderParticle(const Particle* p, PixelMatrix& matrix) override;
    uint32_t particleColor(uint8_t intensity) const override;

private:
    /**
     * Spawn radial splash at position
     */
    void spawnSplash(float x, float y, uint8_t parentIntensity);

    WaterParams params_;
};
