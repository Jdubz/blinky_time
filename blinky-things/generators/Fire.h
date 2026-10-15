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

    // Audio reactivity (b196+ event-driven burst)
    float organicTransientMin;    // (legacy) Minimum transient to trigger burst (0-1)
    float burstSparks;            // (legacy) × crossDim → sparks per burst
    float burstThreshold;         // audio.pulse threshold to fire a burst (0-1, default 0.45)
    float burstVelMult;           // Burst-particle initial velocity multiplier (1-8, default 3.0)
    float burstLifeMult;          // Burst-particle lifespan multiplier vs base (0.1-2.0, default 0.6)
    float burstSizeBase;          // crossDim multiplier — base burst-particle count (default 2.0)
    float burstSizeGain;          // crossDim multiplier × strength → additional burst particles (default 6.0)
    float silenceFloor;           // audio.energy below this = silence (no bursts, no baseline spawn). Default 0.25.
    float audioBrightAmount;      // How much continuous amplitude tracks brightness (0-1). Default 0.7.

    // === Heightmap-flame params (b202 redesign) ===
    // Fire is now a per-column heightmap: each column has a flame whose
    // height + brightness respond to audio. Replaces particle spawning
    // for the smolder/baseline; particle bursts still fire on impulses.
    float smolderHeight;          // Idle flame height (fraction of tube). Default 0.15.
    float maxFlameHeight;         // Hard cap on peak flame height. Default 0.50.
    float audioHeightBoost;       // Max additional height from audio impulse. Default 0.30.
    float noiseAmplitude;         // Amount of noise variation in smolder shape (0-1). Default 0.50.
    float noiseSpatialScale;      // Spatial frequency of noise. Default 0.30 (low = wide patterns).
    float noiseBaseSpeed;         // Base noise time evolution (Hz-ish). Default 0.8.
    float noiseAudioSpeedMult;    // How much louder audio speeds the noise. Default 3.0.

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
        organicTransientMin = 0.25f;
        burstSparks = 0.8f;           // × crossDim → sparks per burst
        // b199 event-driven burst defaults (all runtime-tunable):
        burstThreshold = 0.45f;       // audio.pulse > this fires a burst
        burstVelMult   = 3.0f;        // burst particles spawn at 3x base velocity
        burstLifeMult  = 0.6f;        // burst particles live 0.6x base lifespan (short + bright)
        burstSizeBase  = 2.0f;        // base burst size = 2.0 × crossDim particles
        burstSizeGain  = 6.0f;        // strength bonus = 6.0 × crossDim × strength particles
        silenceFloor   = 0.25f;       // audio.energy below this = silence (no fire activity)
        // PR #149 review: default 0 so renderParticle doesn't dim burst
        // particles to ~29% between beats (regression on sparse tracks).
        // The heightmap layer already handles continuous amplitude tracking
        // — particles are pure impulse accents and should render at their
        // own intensity. Operators wanting per-particle audio dim can opt
        // in with `set audiobright 0.7`.
        audioBrightAmount = 0.0f;
        // Heightmap defaults
        smolderHeight   = 0.15f;      // idle 15% of tube
        maxFlameHeight  = 0.50f;      // hard cap at 50%
        audioHeightBoost = 0.30f;     // up to +30% on big impulse
        noiseAmplitude  = 0.50f;      // smolder height varies ±50% of smolderHeight
        noiseSpatialScale = 0.30f;    // wide noise patterns
        noiseBaseSpeed  = 0.8f;       // slow noise animation when quiet
        noiseAudioSpeedMult = 3.0f;   // loud audio makes noise animate 4x base
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

    // === Edge-triggered musical burst state (b196) ===
    // Each rising edge of a strong rhythmic pulse triggers a burst of
    // high-velocity, full-intensity particles that visibly rise + fade.
    // Carried across the inter-loop boundary because the burst is
    // requested in spawnParticles() pre-loop and consumed inside it.
    bool   lastBeat_ = false;        // for rising-edge detection
    uint8_t burstParticles_ = 0;     // count of burst-mode particles for this frame
    float  burstStrength_ = 0.0f;    // 0.55-1.0, drives burst velocity/lifespan
    float  frameOvershoot_ = 0.0f;   // per-frame normalized amplitude (0-1) above silenceFloor; shared between spawn-rate gating and render-brightness modulation
    float  noiseTime_ = 0.0f;        // heightmap noise time (advances per frame, audio-scaled)
    float  audioEnvelope_ = 0.0f;    // smoothed audio envelope for heightmap audio-boost (attack fast, decay slower)

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

    // === Linear ember persistence (b205) ===
    // Per-LED smoothed brightness for the linear ember floor. The b204 linear
    // renderer recomputed every LED from scratch each frame, so the global
    // audio-driven coverage threshold + audioBright made the whole strip
    // blink in lockstep (the "strobe"). This buffer eases each LED toward its
    // per-frame target with an asymmetric attack/decay, so embers glow up and
    // cool down instead of hard-switching. Sized like Water's ripple buffer;
    // physical strips never exceed this (largest is the 141-LED scarf).
    static constexpr int   MAX_LINEAR_LEDS = 256;
    static constexpr float kEmberAttackTau = 0.09f;  // ~90 ms swell on onsets
    static constexpr float kEmberDecayTau  = 0.40f;  // ~400 ms ember cool-down
    float emberLevel_[MAX_LINEAR_LEDS];              // 0..1 smoothed brightness

    void initGrid();
    void updateHeatGrid();
    void applyGridForce(Particle* p, float dt);

    // === Shared fire simulation (layout-agnostic) ===
    // Audio-driven flame "amount": idle smolder lifted by the audio envelope,
    // UNCLAMPED. This is the single quantity that both layouts derive their
    // visual from — MATRIX maps it to per-column flame HEIGHT, LINEAR maps it
    // to ember-noise COVERAGE along the strip. Same logic, different render.
    float audioFlameAmount() const {
        return params_.smolderHeight + audioEnvelope_ * params_.audioHeightBoost;
    }

    // === Layout-specific ember-floor renderers ===
    // Matrix: per-column heightmap flame, bright base → dim tip (b202 visual,
    //         unchanged — extracted verbatim so linear can be a peer, not a slice).
    void renderMatrixHeightmap(PixelMatrix& matrix);
    // Linear: "floating noise" embers whose coverage tracks audioFlameAmount(),
    // temporally smoothed per-LED (dt) so they fade rather than strobe.
    void renderLinearEmber(PixelMatrix& matrix, float dt);
};
