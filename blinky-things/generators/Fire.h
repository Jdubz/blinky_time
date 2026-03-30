#pragma once

#include "../particles/ParticleGenerator.h"
#include "../types/ColorPalette.h"
#include "../math/SimplexNoise.h"

/**
 * FireParams - Fire-specific particle parameters
 */
struct FireParams {
    // Spawn behavior (density: expected sparks per crossDim-unit per frame, scaled at use-time like burstSparks)
    float baseSpawnChance;        // Ambient spawn density (× crossDim → sparks/frame)
    float audioSpawnBoost;        // Additional density at max energy (× crossDim → sparks/frame)

    // Lifecycle
    float maxParticles;           // Fraction of numLeds for pool capacity (init-time only)
    uint8_t defaultLifespan;      // Default particle lifespan in centiseconds (0.01s units, 0-2.55s range)
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Physics (fractions × device dimensions, scaled at use-time)
    float windVariation;          // × crossDim → curl noise turbulence amplitude (LEDs/sec)
    float drag;                   // Drag coefficient (0-1, per frame damping)

    // Spark appearance (fractions × device dimensions, scaled at use-time)
    float sparkVelocityMin;       // × traversalDim → minimum upward velocity (LEDs/sec)
    float sparkVelocityMax;       // × traversalDim → maximum upward velocity (LEDs/sec)
    float sparkSpread;            // × crossDim → horizontal velocity variation (LEDs/sec)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation depth for spawn rate (0=flat, 1=full breathing)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)
    float burstSparks;            // × crossDim → sparks per burst

    // Thermal physics
    float thermalForce;           // × traversalDim → thermal buoyancy (LEDs/sec^2)

    // Fluid dynamics heat grid
    float gridCoolRate;           // Grid heat decay per frame (0=instant, 1=no decay; default 0.88)
    float buoyancyCoupling;       // Grid heat → additional upward force multiplier (default 1.0)
    float pressureCoupling;       // Lateral heat gradient → clustering force multiplier (default 0.5)

    FireParams() {
        // Velocity/force params are FRACTIONS of device dimensions, scaled at use-time.
        //
        // Design: 30% height at ambient (vigor=0.3, no audio), 100%+ on hits.
        // Per-particle vigor [0.3-1.0] × velocity gives organic variation.
        // Audio energy/pulse multiply ALL living particles in real time.
        baseSpawnChance = 0.12f;      // × crossDim → sparks/frame (denser than before)
        audioSpawnBoost = 0.25f;      // × crossDim → extra sparks at peak energy
        maxParticles = 0.75f;         // Pool sized at begin(); changing at runtime has no effect
        defaultLifespan = 80;         // 0.8s base (centiseconds), vigor-scaled at spawn
        intensityMin = 180;
        intensityMax = 255;
        windVariation = 1.5f;         // × crossDim → LEDs/sec of curl turbulence
        drag = 0.985f;            // Gentle deceleration (thermal removed, no acceleration to fight)
        musicSpawnPulse = 0.95f;      // Deep phase breathing (reserved for future rhythm)
        organicTransientMin = 0.25f;
        burstSparks = 0.8f;           // × crossDim → sparks per burst
        sparkVelocityMin = 1.0f;      // × traversalDim → min upward velocity (3x original)
        sparkVelocityMax = 2.0f;      // × traversalDim → max upward velocity (3x original)
        sparkSpread = 1.0f;           // × crossDim → horizontal scatter
        thermalForce = 3.0f;          // × traversalDim → buoyancy in LEDs/sec^2 (stronger)
        gridCoolRate = 0.88f;
        buoyancyCoupling = 1.0f;
        pressureCoupling = 0.5f;
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
        drag_ = params_.drag;
        if (forceAdapter_) {
            forceAdapter_->setWind(0.0f, scaledWindVar());
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

    // Palette blending state (low-pass filtered to avoid jarring shifts)
    float paletteBias_;          // 0.0 = warm/cool, 1.0 = hot (smoothed energy*rhythm)

    // Coarse heat grid for fluid dynamics (Eulerian velocity field)
    // Particles splat heat onto the grid; grid gradients push particles into flame columns.
    static constexpr int FIRE_GRID_W = 8;
    static constexpr int FIRE_GRID_H = 8;
    float heatGrid_[FIRE_GRID_W * FIRE_GRID_H];  // row-major [gy * FIRE_GRID_W + gx]
    int gridW_;      // actual used columns (≤ FIRE_GRID_W, clipped to width_)
    int gridH_;      // actual used rows    (≤ FIRE_GRID_H, clipped to height_)
    float cellW_;    // LED width of each grid cell
    float cellH_;    // LED height of each grid cell

    void initGrid();
    void updateHeatGrid();
    void applyGridForce(Particle* p, float dt);
};
