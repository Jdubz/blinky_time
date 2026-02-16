#pragma once

#include "../particles/ParticleGenerator.h"
#include "../physics/BackgroundModel.h"
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
    uint8_t defaultLifespan;      // Default particle lifespan in centiseconds (0.01s units, 0-2.55s range)
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Physics
    float gravity;                // Gravity strength (positive = down, applied per frame)
    float windBase;               // Base wind force (applied per frame)
    float windVariation;          // Wind variation amount (applied per frame)
    float drag;                   // Drag coefficient (0-1, per frame damping)

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

    // Background
    float backgroundIntensity;    // Noise background brightness (0-1)

    WaterParams() {
        // RAIN EFFECT: Bright drops falling against dark background
        baseSpawnChance = 0.8f;   // HIGH spawn rate - always raining
        audioSpawnBoost = 0.3f;   // Some music response
        maxParticles = 30;        // Enough for visible rain
        defaultLifespan = 200;    // 2.0 seconds - time to fall (200 centiseconds)
        intensityMin = 180;       // BRIGHT drops
        intensityMax = 255;       // Maximum brightness
        gravity = 25.0f;          // LEDs/sec² - accelerates fall
        windBase = 0.0f;
        windVariation = 3.0f;     // Slight wind sway
        drag = 0.995f;            // Almost no drag
        musicSpawnPulse = 0.4f;
        organicTransientMin = 0.5f;
        backgroundIntensity = 0.15f;  // Visible but subtle background

        // Velocities: drops traverse 8-10 LEDs in ~2 seconds with acceleration
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
 * Layout-aware: works on both matrix (2D) and linear (1D) layouts.
 *
 * Features:
 * - Drops spawn from layout-appropriate source region
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

    // Sync physics parameters to force adapter (call after params change at runtime)
    void syncPhysicsParams() {
        gravity_ = params_.gravity;
        drag_ = params_.drag;
        if (forceAdapter_) {
            forceAdapter_->setWind(params_.windBase, params_.windVariation);
        }
    }

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
     * Spawn radial splash at position
     */
    void spawnSplash(float x, float y, uint8_t parentIntensity);

    WaterParams params_;
    float noiseTime_;             // Animation time for noise field

    // Water-specific physics components
    BackgroundModel* background_;

    // Static buffer for placement new
    uint8_t backgroundBuffer_[64];
};
