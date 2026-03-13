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
    float maxParticles;           // Fraction of numLeds for max active particles (scaled, clamped to pool)
    uint8_t defaultLifespan;      // Default particle lifespan in centiseconds (0.01s units, 0-2.55s range)
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Physics (fractions × device dimensions, scaled at use-time)
    float gravity;                // × traversalDim → downward acceleration (LEDs/sec²)
    float windBase;               // Base wind force (absolute, typically 0)
    float windVariation;          // × crossDim → turbulence amplitude (LEDs/sec)
    float drag;                   // Drag coefficient (0-1, per frame damping)

    // Drop appearance (fractions × device dimensions, scaled at use-time)
    float dropVelocityMin;        // × traversalDim → minimum downward velocity (LEDs/sec)
    float dropVelocityMax;        // × traversalDim → maximum downward velocity (LEDs/sec)
    float dropSpread;             // × crossDim → horizontal velocity variation (LEDs/sec)

    // Splash behavior
    float splashParticles;        // × crossDim → particles spawned on splash
    float splashVelocityMin;      // × traversalDim → minimum splash velocity (LEDs/sec)
    float splashVelocityMax;      // × traversalDim → maximum splash velocity (LEDs/sec)
    uint8_t splashIntensity;      // Splash particle intensity multiplier (0-255)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation for spawn rate (0-1)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)

    // Background
    float backgroundIntensity;    // Noise background brightness (0-1)

    WaterParams() {
        // All velocity/force/count params are FRACTIONS of device dimensions.
        // The generator multiplies by traversalDim_ or crossDim_ at use-time.
        baseSpawnChance = 0.8f;       // HIGH spawn rate - always raining
        audioSpawnBoost = 0.3f;       // Some music response
        maxParticles = 0.5f;          // Fraction of numLeds (clamped to pool capacity 30)
        defaultLifespan = 200;        // 2.0 seconds to fall (centiseconds, device-independent)
        intensityMin = 180;           // BRIGHT drops
        intensityMax = 255;           // Maximum brightness
        gravity = 1.67f;             // × traversalDim → LEDs/sec² downward acceleration
        windBase = 0.0f;
        windVariation = 0.2f;         // × crossDim → slight wind sway
        drag = 0.995f;               // Almost no drag
        musicSpawnPulse = 0.4f;
        organicTransientMin = 0.5f;
        backgroundIntensity = 0.15f;

        // Velocities: fraction of traversal per second (~0.4-0.67 → traverse in 1.5-2.5s)
        dropVelocityMin = 0.4f;       // × traversalDim → LEDs/sec starting velocity
        dropVelocityMax = 0.67f;      // × traversalDim → LEDs/sec
        dropSpread = 0.375f;          // × crossDim → horizontal drift

        splashParticles = 0.75f;      // × crossDim → particles per splash
        splashVelocityMin = 0.27f;    // × traversalDim → LEDs/sec
        splashVelocityMax = 0.53f;    // × traversalDim → LEDs/sec
        splashIntensity = 150;        // Bright splash
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
        gravity_ = params_.gravity * traversalDim_;
        drag_ = params_.drag;
        if (forceAdapter_) {
            forceAdapter_->setWind(params_.windBase, scaledWindVar());
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

    // Dimension-derived parameter accessors (params × device dimensions)
    float scaledDropVelMin() const { return params_.dropVelocityMin * traversalDim_; }
    float scaledDropVelMax() const { return params_.dropVelocityMax * traversalDim_; }
    float scaledDropSpread() const { return params_.dropSpread * crossDim_; }
    float scaledWindVar() const { return params_.windVariation * crossDim_; }
    uint8_t scaledMaxParticles() const {
        return (uint8_t)min(30.0f, max(8.0f, params_.maxParticles * numLeds_));
    }
    float scaledSplashVelMin() const { return params_.splashVelocityMin * traversalDim_; }
    float scaledSplashVelMax() const { return params_.splashVelocityMax * traversalDim_; }
    uint8_t scaledSplashParticles() const {
        return (uint8_t)min(10.0f, params_.splashParticles * crossDim_);
    }

    WaterParams params_;
    float noiseTime_;             // Animation time for noise field

    // Water-specific physics components
    BackgroundModel* background_;

    // Static buffer for placement new
    uint8_t backgroundBuffer_[64];
};
