#pragma once

#include "../particles/ParticleGenerator.h"
#include "../types/ColorPalette.h"
#include "../math/SimplexNoise.h"

/**
 * FireParams - Fire-specific particle parameters
 */
struct FireParams {
    // Spawn behavior
    float baseSpawnChance;        // Baseline spark spawn probability (0-1)
    float audioSpawnBoost;        // Audio reactivity multiplier (0-2)

    // Lifecycle
    uint8_t maxParticles;         // Maximum active particles (1-48, default 48)
    uint8_t defaultLifespan;      // Default particle age in frames
    uint8_t intensityMin;         // Minimum spawn intensity (0-255)
    uint8_t intensityMax;         // Maximum spawn intensity (0-255)

    // Physics
    float gravity;                // Gravity strength (negative = upward, applied per frame)
    float windBase;               // Base wind force (applied per frame)
    float windVariation;          // Wind variation amount (applied per frame)
    float drag;                   // Drag coefficient (0-1, per frame damping)

    // Spark appearance
    float sparkVelocityMin;       // Minimum upward velocity (LEDs/frame@30FPS)
    float sparkVelocityMax;       // Maximum upward velocity (LEDs/frame@30FPS)
    float sparkSpread;            // Horizontal velocity variation (LEDs/frame@30FPS)

    // Heat trail behavior
    uint8_t trailHeatFactor;      // Heat multiplier for trail (0-100%)
    uint8_t trailDecay;           // Heat decay rate per frame (0-255)

    // Audio reactivity
    float musicSpawnPulse;        // Phase modulation for spawn rate (0-1)
    float organicTransientMin;    // Minimum transient to trigger burst (0-1)
    uint8_t burstSparks;          // Sparks per burst

    FireParams() {
        baseSpawnChance = 0.18f;  // Baseline spark probability (tuned for ambient activity)
        audioSpawnBoost = 0.5f;   // Music mode boost
        maxParticles = 72;        // Tuned for 16x8 matrix coverage
        defaultLifespan = 75;     // ~2.5 seconds at 30 FPS
        intensityMin = 80;        // Start in red range (< 85)
        intensityMax = 180;       // Mostly orange with some yellow highlights
        gravity = -8.0f;          // Negative = upward (fire rises)
        windBase = 0.0f;
        windVariation = 0.6f;     // Gentle sway for organic feel
        drag = 0.96f;
        musicSpawnPulse = 0.6f;
        organicTransientMin = 0.28f;  // Lower threshold for ambient transients
        burstSparks = 8;

        trailHeatFactor = 50;     // Stronger particle trails
        trailDecay = 32;          // Slower cooling for persistent glow

        sparkVelocityMin = 1.0f;  // Slower sparks for matrix coverage
        sparkVelocityMax = 3.0f;
        sparkSpread = 0.8f;
    }
};

/**
 * Fire - Hybrid particle-based fire generator
 *
 * Uses particles for bright sparks and heat field for diffusion.
 * This combines the best of both approaches:
 * - Particles: Dynamic, physics-based sparks
 * - Heat field: Smooth diffusion and glow
 *
 * Features:
 * - Sparks rise from bottom with upward velocity
 * - Heat trails left behind particles
 * - Heat diffusion for smooth ember glow
 * - Beat-synced burst spawning in music mode
 */
class Fire : public ParticleGenerator<72> {
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

protected:
    // ParticleGenerator hooks
    void spawnParticles(float dt) override;
    void updateParticle(Particle* p, float dt) override;
    void renderParticle(const Particle* p, PixelMatrix& matrix) override;
    uint32_t particleColor(uint8_t intensity) const override;

private:
    /**
     * Apply cooling to heat buffer
     */
    void applyCooling();

    /**
     * Diffuse heat upward to create smooth gradients
     */
    void diffuseHeat();

    /**
     * Blend heat buffer with particle rendering
     */
    void blendHeatToMatrix(PixelMatrix& matrix);

    /**
     * Render simplex noise background with fire gradient
     * Creates organic, animated ember glow beneath particles
     */
    void renderNoiseBackground(PixelMatrix& matrix);

    uint8_t* heat_;               // Heat field buffer
    FireParams params_;
    uint8_t beatCount_;           // Beat counter for downbeat detection
    float noiseTime_;             // Animation time for noise field
};
