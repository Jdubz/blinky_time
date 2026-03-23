#pragma once

#include "../particles/ParticleGenerator.h"
#include "../physics/BackgroundModel.h"
#include "../types/ColorPalette.h"
#include "../math/SimplexNoise.h"

/**
 * SparkType - Defines different visual behaviors for fire particles
 */
enum class SparkType : uint8_t {
    FAST_SPARK = 0,    // Short-lived, bright, normal speed (primary sparks)
    SLOW_EMBER = 1,    // Long-lived, dim, slow speed (glowing embers)
    BURST_SPARK = 2    // Medium speed, maximum brightness (transient bursts)
};

/**
 * FireParams - Fire-specific particle parameters
 */
struct FireParams {
    // Spawn behavior
    float baseSpawnChance;        // Baseline spark spawn probability (0-1)
    float audioSpawnBoost;        // Audio reactivity multiplier (0-2)

    // Lifecycle
    float maxParticles;           // Fraction of numLeds for max active particles
    uint8_t defaultLifespan;      // Default particle lifespan in centiseconds (0.01s units, 0-2.55s range)
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Physics (fractions × device dimensions, scaled at use-time)
    float gravity;                // × traversalDim → gravity strength (negative = upward)
    float windBase;               // Base wind force (absolute, typically 0)
    float windVariation;          // × crossDim → turbulence amplitude
    float drag;                   // Drag coefficient (0-1, per frame damping)

    // Spark appearance (fractions × device dimensions, scaled at use-time)
    float sparkVelocityMin;       // × traversalDim → minimum upward velocity (LEDs/sec)
    float sparkVelocityMax;       // × traversalDim → maximum upward velocity (LEDs/sec)
    float sparkSpread;            // × crossDim → horizontal velocity variation (LEDs/sec)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation for spawn rate (0-1)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)
    float burstSparks;            // × crossDim → sparks per burst

    // Background
    float backgroundIntensity;    // Noise background brightness (0-1)

    // Particle variety
    float fastSparkRatio;         // Ratio of fast sparks (0-1, rest are embers)
    float thermalForce;           // × traversalDim → thermal buoyancy (LEDs/sec^2)

    FireParams() {
        // All velocity/force/count params are stored as FRACTIONS of device
        // dimensions.  The generator multiplies by traversalDim_ or crossDim_
        // at use-time, so no per-device calibration is needed.
        baseSpawnChance = 0.5f;       // Continuous sparks for constant fire
        audioSpawnBoost = 1.5f;       // Strong audio response
        maxParticles = 0.75f;         // Fraction of numLeds (pool auto-sized in begin())
        defaultLifespan = 170;        // 1.7 seconds to rise (centiseconds, time-based, device-independent)
        intensityMin = 150;           // BRIGHT red/orange
        intensityMax = 220;           // Very bright (orange range)
        gravity = 0.0f;              // No gravity (thermal force provides upward push)
        windBase = 0.0f;
        windVariation = 1.5f;         // × crossDim → turbulence amplitude in LEDs/sec
        drag = 0.985f;               // Smoother flow
        musicSpawnPulse = 0.95f;      // Deep phase breathing (0=flat, 1=full off-beat silence)
        organicTransientMin = 0.25f;  // Responsive to softer transients
        burstSparks = 0.5f;           // × crossDim → sparks per burst
        backgroundIntensity = 0.15f;  // Subtle noise background

        // Velocities: fraction of traversal per second (~0.33-0.67 → traverse in 1.5-3s)
        sparkVelocityMin = 0.33f;     // × traversalDim → LEDs/sec upward
        sparkVelocityMax = 0.67f;     // × traversalDim → LEDs/sec upward
        sparkSpread = 1.0f;           // × crossDim → horizontal scatter in LEDs/sec

        // Particle variety: 70% fast sparks, 30% slow embers
        fastSparkRatio = 0.7f;
        thermalForce = 2.0f;          // × traversalDim → buoyancy in LEDs/sec^2
    }
};

/**
 * Fire - Particle-only fire generator
 *
 * Sparks are the only visual primitive; heat is a per-particle property
 * (intensity) that drives both rendering and thermal buoyancy physics.
 * Layout-aware: works on both matrix (2D) and linear (1D) layouts.
 *
 * Features:
 * - Sparks spawn from layout-appropriate source region
 * - Thermal buoyancy: hotter sparks rise faster (dims → slows naturally)
 * - Wind turbulence visible as sparks sway (no static heat underlayer)
 * - Beat-synced burst spawning in music mode
 */
class Fire : public ParticleGenerator {
public:
    Fire();
    virtual ~Fire() override;

    // Generator interface
    bool begin(const DeviceConfig& config) override;
    void generate(PixelMatrix& matrix, const AudioControl& audio) override;
    void reset() override;
    const char* getName() const override { return "Fire"; }
    GeneratorType getType() const override { return GeneratorType::FIRE; }

    // Parameter access
    void setParams(const FireParams& params) { params_ = params; }
    const FireParams& getParams() const { return params_; }
    FireParams& getParamsMutable() { return params_; }

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
     * Spawn a particle with specific type characteristics
     */
    void spawnTypedParticle(SparkType type, float x, float y, float baseSpeed);

    // Pool sizing: density comes from params_.maxParticles
    float particleDensity() const override { return params_.maxParticles; }

    // Dimension-derived parameter accessors.
    // Params are stored as normalized rates/fractions; these multiply by
    // the actual device dimensions so the visual effect auto-adapts.
    float scaledVelMin() const { return params_.sparkVelocityMin * traversalDim_; }
    float scaledVelMax() const { return params_.sparkVelocityMax * traversalDim_; }
    float scaledSpread() const { return params_.sparkSpread * crossDim_; }
    float scaledWindVar() const { return params_.windVariation * crossDim_; }
    float scaledThermalForce() const { return params_.thermalForce * traversalDim_; }
    uint16_t scaledMaxParticles() const { return pool_.getCapacity(); }
    uint8_t scaledBurstSparks() const {
        return (uint8_t)max(1.0f, params_.burstSparks * crossDim_);
    }

    FireParams params_;
    float noiseTime_;             // Animation time for noise field

    // Palette blending state (low-pass filtered to avoid jarring shifts)
    float paletteBias_;          // 0.0 = warm/cool, 1.0 = hot (smoothed energy*rhythm)

    // Fire-specific physics components
    BackgroundModel* background_;

    // Static buffer for placement new
    uint8_t backgroundBuffer_[64];
};
