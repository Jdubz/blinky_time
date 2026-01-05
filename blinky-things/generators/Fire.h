#pragma once

#include "../particles/ParticleGenerator.h"
#include "../types/ColorPalette.h"

/**
 * FireParams - Fire-specific particle parameters
 */
struct FireParams {
    // Spawn behavior
    float baseSpawnChance;        // Baseline spark spawn probability (0-1)
    float audioSpawnBoost;        // Audio reactivity multiplier (0-2)
    uint8_t maxParticles;         // Pool size (48 typical)

    // Lifecycle
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
        baseSpawnChance = 0.15f;
        audioSpawnBoost = 0.6f;
        maxParticles = 48;
        defaultLifespan = 60;  // ~2 seconds at 30 FPS
        intensityMin = 160;
        intensityMax = 255;
        gravity = -8.0f;  // Negative = upward (fire rises)
        windBase = 0.0f;
        windVariation = 0.5f;
        drag = 0.96f;
        musicSpawnPulse = 0.6f;
        organicTransientMin = 0.5f;
        burstSparks = 8;

        trailHeatFactor = 60;  // 60% of particle intensity left as trail
        trailDecay = 40;       // Moderate decay rate

        sparkVelocityMin = 1.5f;
        sparkVelocityMax = 3.5f;
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
class Fire : public ParticleGenerator<48> {
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
     * Blend heat buffer with particle rendering
     */
    void blendHeatToMatrix(PixelMatrix& matrix);

    uint8_t* heat_;               // Heat field buffer
    FireParams params_;
    uint8_t beatCount_;           // Beat counter for downbeat detection
};
