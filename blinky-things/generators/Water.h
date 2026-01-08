#pragma once

#include "../particles/ParticleGenerator.h"
#include "../types/ColorPalette.h"
#include "../math/SimplexNoise.h"

/**
 * WaterParams - Water-specific particle parameters
 */
struct WaterParams {
    // Spawn behavior
    float baseSpawnChance;        // Baseline drop spawn probability (0-1)
    float audioSpawnBoost;        // Audio reactivity multiplier (0-2)

    // Lifecycle
    uint8_t maxParticles;         // Maximum active particles (1-64, default 64)
    uint8_t defaultLifespan;      // Default particle age in frames
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Physics
    float gravity;                // Gravity strength (positive = down, applied per frame)
    float windBase;               // Base wind force (applied per frame)
    float windVariation;          // Wind variation amount (applied per frame)
    float drag;                   // Drag coefficient (0-1, per frame damping)

    // Drop appearance
    float dropVelocityMin;        // Minimum downward velocity (LEDs/frame@30FPS)
    float dropVelocityMax;        // Maximum downward velocity (LEDs/frame@30FPS)
    float dropSpread;             // Horizontal velocity variation (LEDs/frame@30FPS)

    // Splash behavior
    uint8_t splashParticles;      // Number of particles spawned on splash (0-10)
    float splashVelocityMin;      // Minimum splash velocity (LEDs/frame@30FPS)
    float splashVelocityMax;      // Maximum splash velocity (LEDs/frame@30FPS)
    uint8_t splashIntensity;      // Splash particle intensity multiplier (0-255)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation for spawn rate (0-1)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)

    WaterParams() {
        // RAIN EFFECT: Bright drops falling against dark background
        baseSpawnChance = 0.8f;   // HIGH spawn rate - always raining
        audioSpawnBoost = 0.3f;   // Some music response
        maxParticles = 30;        // Enough for visible rain
        defaultLifespan = 60;     // 2 seconds - time to fall
        intensityMin = 180;       // BRIGHT drops
        intensityMax = 255;       // Maximum brightness
        gravity = 25.0f;          // LEDs/sec² - accelerates fall
        windBase = 0.0f;
        windVariation = 3.0f;     // Slight wind sway
        drag = 0.995f;            // Almost no drag
        musicSpawnPulse = 0.4f;
        organicTransientMin = 0.5f;

        // Velocities: drops should traverse 8-pixel height in ~1 second
        dropVelocityMin = 6.0f;   // LEDs/sec starting velocity
        dropVelocityMax = 10.0f;  // LEDs/sec
        dropSpread = 1.5f;        // Slight horizontal drift

        splashParticles = 3;      // Small splash
        splashVelocityMin = 4.0f;
        splashVelocityMax = 8.0f;
        splashIntensity = 150;    // Bright splash
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
class Water : public ParticleGenerator<30> {
public:
    Water();
    virtual ~Water() override = default;

    // Generator interface
    bool begin(const DeviceConfig& config) override;
    void generate(PixelMatrix& matrix, const AudioControl& audio) override;
    void reset() override;
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

    /**
     * Render simplex noise background with tropical sea colors
     * Blue/green/cyan gradient simulating tropical water
     */
    void renderNoiseBackground(PixelMatrix& matrix);

    WaterParams params_;
    float noiseTime_;             // Animation time for noise field
};
