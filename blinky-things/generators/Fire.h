#pragma once

#include "../particles/ParticleGenerator.h"
#include "../physics/PropagationModel.h"
#include "../physics/BackgroundModel.h"
#include "../types/ColorPalette.h"
#include "../math/SimplexNoise.h"

/**
 * SparkType - Defines different visual behaviors for fire particles
 */
enum class SparkType : uint8_t {
    FAST_SPARK = 0,    // Short-lived, fast, minimal trail (primary sparks)
    SLOW_EMBER = 1,    // Long-lived, slow, heavy trail (glowing embers)
    BURST_SPARK = 2    // Medium speed, high intensity (transient bursts)
};

/**
 * FireParams - Fire-specific particle parameters
 */
struct FireParams {
    // Spawn behavior
    float baseSpawnChance;        // Baseline spark spawn probability (0-1)
    float audioSpawnBoost;        // Audio reactivity multiplier (0-2)

    // Lifecycle
    uint8_t maxParticles;         // Maximum active particles (1-48, default 48)
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

    // Heat trail behavior
    uint8_t trailHeatFactor;      // Heat multiplier for trail (0-100%)
    uint8_t trailDecay;           // Heat decay rate per frame (0-255)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation for spawn rate (0-1)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)
    uint8_t burstSparks;          // Sparks per burst

    // Background
    float backgroundIntensity;    // Noise background brightness (0-1)

    // Particle variety
    float fastSparkRatio;         // Ratio of fast sparks (0-1, rest are embers)

    FireParams() {
        // FIRE EFFECT: Bright sparks rising from source
        baseSpawnChance = 0.7f;   // HIGH spawn rate - constant sparks
        audioSpawnBoost = 0.4f;   // Music boost
        maxParticles = 35;        // Good spark coverage
        defaultLifespan = 170;    // 1.7 seconds to rise (170 centiseconds)
        intensityMin = 150;       // BRIGHT red/orange
        intensityMax = 220;       // Very bright (orange range)
        gravity = 0.0f;           // No gravity (disabled for linear layouts, confusing horizontal drift)
        windBase = 0.0f;
        windVariation = 25.0f;    // Strong turbulence (increased for visibility over spawn velocity)
        drag = 0.97f;             // Light drag
        musicSpawnPulse = 0.5f;
        organicTransientMin = 0.4f;
        burstSparks = 4;          // Sparks per burst
        backgroundIntensity = 0.15f;  // Visible but subtle background

        trailHeatFactor = 5;      // MINIMAL trails - discrete sparks
        trailDecay = 100;         // FAST cooling - no blob

        // Velocities: sparks rise ~8-10 LEDs in 1.7 seconds
        sparkVelocityMin = 5.0f;  // LEDs/sec upward
        sparkVelocityMax = 10.0f; // LEDs/sec upward
        sparkSpread = 4.0f;       // Some horizontal spread

        // Particle variety: 70% fast sparks, 30% slow embers
        fastSparkRatio = 0.7f;
    }
};

/**
 * Fire - Hybrid particle-based fire generator
 *
 * Uses particles for bright sparks and heat field for diffusion.
 * Layout-aware: works on both matrix (2D) and linear (1D) layouts.
 *
 * Features:
 * - Sparks spawn from layout-appropriate source region
 * - Heat trails left behind particles
 * - Layout-aware heat diffusion (upward for matrix, lateral for linear)
 * - Beat-synced burst spawning in music mode
 */
class Fire : public ParticleGenerator<35> {
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

    /**
     * Apply cooling to heat buffer
     */
    void applyCooling();

    /**
     * Blend heat buffer with particle rendering
     */
    void blendHeatToMatrix(PixelMatrix& matrix);

    uint8_t* heat_;               // Heat field buffer
    FireParams params_;
    uint8_t beatCount_;           // Beat counter for downbeat detection
    float noiseTime_;             // Animation time for noise field

    // Fire-specific physics components
    PropagationModel* propagation_;
    BackgroundModel* background_;

    // Static buffers for placement new
    uint8_t propagationBuffer_[64];
    uint8_t backgroundBuffer_[64];
};
