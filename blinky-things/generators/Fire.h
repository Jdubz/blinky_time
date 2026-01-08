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
        baseSpawnChance = 0.35f;  // Higher spawn rate for visible flames
        audioSpawnBoost = 0.5f;   // Music boost
        maxParticles = 40;        // More particles
        defaultLifespan = 60;     // 2 seconds at 30 FPS
        intensityMin = 70;        // Red range (visible)
        intensityMax = 150;       // Orange (no yellow/white)
        gravity = -50.0f;         // LEDs/sec² upward (gentle rise)
        windBase = 0.0f;
        windVariation = 8.0f;     // LEDs/sec² wind sway
        drag = 0.96f;             // Light drag
        musicSpawnPulse = 0.5f;
        organicTransientMin = 0.35f;
        burstSparks = 5;          // Sparks per burst

        trailHeatFactor = 20;     // Moderate trails for warmth
        trailDecay = 50;          // Moderate cooling

        // Velocities in LEDs/sec (physics uses dt in seconds!)
        sparkVelocityMin = 4.0f;  // ~4 LEDs/sec upward (slower = visible longer)
        sparkVelocityMax = 10.0f; // ~10 LEDs/sec upward
        sparkSpread = 5.0f;       // Horizontal spread in LEDs/sec
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
class Fire : public ParticleGenerator<40> {
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
