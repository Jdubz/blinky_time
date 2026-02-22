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
    uint8_t maxParticles;         // Maximum active particles (1-64, default 48)
    uint8_t defaultLifespan;      // Default particle lifespan in centiseconds (0.01s units, 0-2.55s range)
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Physics
    float gravity;                // Gravity strength (negative = upward, applied per frame)
    float windBase;               // Base wind force (applied per frame)
    float windVariation;          // Wind variation amount (applied per frame)
    float drag;                   // Drag coefficient (0-1, per frame damping)

    // Spark appearance
    float sparkVelocityMin;       // Minimum upward velocity (LEDs/sec)
    float sparkVelocityMax;       // Maximum upward velocity (LEDs/sec)
    float sparkSpread;            // Horizontal velocity variation (LEDs/sec)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation for spawn rate (0-1)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)
    uint8_t burstSparks;          // Sparks per burst

    // Background
    float backgroundIntensity;    // Noise background brightness (0-1)

    // Particle variety
    float fastSparkRatio;         // Ratio of fast sparks (0-1, rest are embers)
    float thermalForce;           // Thermal buoyancy strength in LEDs/sec^2 (0-200)

    FireParams() {
        // Defaults must match ConfigStorage::loadSettingsDefaults()
        baseSpawnChance = 0.5f;   // Continuous sparks for constant fire
        audioSpawnBoost = 1.5f;   // Strong audio response
        maxParticles = 48;        // Good spark coverage (pool capacity = 64)
        defaultLifespan = 170;    // 1.7 seconds to rise (170 centiseconds)
        intensityMin = 150;       // BRIGHT red/orange
        intensityMax = 220;       // Very bright (orange range)
        gravity = 0.0f;           // No gravity (thermal force provides upward push)
        windBase = 0.0f;
        windVariation = 25.0f;    // Turbulence as LEDs/sec advection (visible swirl)
        drag = 0.985f;            // Smoother flow
        musicSpawnPulse = 0.95f;  // Deep phase breathing (0=flat, 1=full off-beat silence)
        organicTransientMin = 0.25f;  // Responsive to softer transients
        burstSparks = 8;          // Visible burst on hits
        backgroundIntensity = 0.15f;  // Subtle noise background

        // Velocities: sparks rise ~8-10 LEDs in 1.7 seconds
        sparkVelocityMin = 5.0f;  // LEDs/sec upward
        sparkVelocityMax = 10.0f; // LEDs/sec upward
        sparkSpread = 4.0f;       // Good spread

        // Particle variety: 70% fast sparks, 30% slow embers
        fastSparkRatio = 0.7f;
        thermalForce = 30.0f;     // Thermal buoyancy (LEDs/sec^2 at max heat)
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
class Fire : public ParticleGenerator<64> {
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
     * Spawn a particle with specific type characteristics
     */
    void spawnTypedParticle(SparkType type, float x, float y, float baseSpeed);

    FireParams params_;
    uint8_t beatCount_;           // Beat counter for downbeat detection
    float noiseTime_;             // Animation time for noise field

    // Fire-specific physics components
    BackgroundModel* background_;

    // Static buffer for placement new
    uint8_t backgroundBuffer_[64];
};
