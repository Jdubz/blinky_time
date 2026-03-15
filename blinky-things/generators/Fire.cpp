#include "Fire.h"
#include "../physics/PhysicsContext.h"
#include "../physics/MatrixBackground.h"
#include "../physics/LinearBackground.h"
#include "../physics/EdgeSpawnRegion.h"
#include "../physics/RandomSpawnRegion.h"
#include "../physics/KillBoundary.h"
#include "../physics/WrapBoundary.h"
#include "../physics/MatrixForceAdapter.h"
#include "../physics/LinearForceAdapter.h"
#include <Arduino.h>

Fire::Fire()
    : params_(), beatCount_(0), noiseTime_(0.0f),
      downbeatSpreadMult_(1.0f), downbeatColorShift_(0.0f),
      downbeatVelBoost_(0.0f), spawnBias_(0.0f),
      background_(nullptr) {}

Fire::~Fire() {
    // Physics components use placement new, no delete needed
}

bool Fire::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;

    beatCount_ = 0;

    return true;
}

void Fire::initPhysicsContext() {
    // Set physics parameters from FireParams (dimension-scaled)
    gravity_ = params_.gravity * traversalDim_;
    drag_ = params_.drag;

    // Create layout-appropriate physics components
    bool wrap = PhysicsContext::shouldWrapByDefault(layout_);

    // Spawn region: bottom edge for matrix, random for linear
    spawnRegion_ = PhysicsContext::createSpawnRegion(
        layout_, GeneratorType::FIRE, width_, height_, spawnBuffer_);

    // Boundary: kill for matrix, wrap for linear
    boundary_ = PhysicsContext::createBoundary(
        layout_, GeneratorType::FIRE, wrap, boundaryBuffer_);

    // Force adapter: 2D for matrix, 1D for linear
    forceAdapter_ = PhysicsContext::createForceAdapter(layout_, forceBuffer_);
    if (forceAdapter_) {
        forceAdapter_->setWind(params_.windBase, scaledWindVar());
    }

    // Background model: height-falloff for matrix, uniform for linear
    background_ = PhysicsContext::createBackground(
        layout_, BackgroundStyle::FIRE, backgroundBuffer_);
}

void Fire::generate(PixelMatrix& matrix, const AudioControl& audio) {
    // onset density (0-10+): normalize to 0-1 for modulation
    float densityNorm = min(1.0f, audio.onsetDensity / 6.0f);

    // Advance noise animation time
    // High onset density → faster, more jittery noise; low → slow, languid
    float organicSpeed = (0.008f + 0.012f * densityNorm) + 0.005f * audio.energy;
    float musicSpeed   = (0.025f + 0.020f * densityNorm) + 0.020f * audio.energy;
    noiseTime_ += organicSpeed * (1.0f - audio.rhythmStrength) + musicSpeed * audio.rhythmStrength;

    // Render noise background first (underlayer)
    if (background_) {
        background_->setIntensity(params_.backgroundIntensity);
        background_->render(matrix, width_, height_, noiseTime_, audio);
    }

    // Modulate wind turbulence by audio (phase breathing + transient gusts + onset density)
    if (forceAdapter_) {
        float phasePulse = audio.phaseToPulse();  // 1.0 on beat, 0.0 off-beat
        // Wind breathes: 30% base + 70% phase modulation (dramatic calming between beats)
        float phaseWind = 0.3f + 0.7f * phasePulse;
        // Transient gusts: spike to 3x on strong hits
        float transientGust = 1.0f + 2.0f * audio.pulse;
        // Blend modulation by rhythmStrength (no modulation when no rhythm detected)
        float mod = 1.0f * (1.0f - audio.rhythmStrength) +
                    phaseWind * transientGust * audio.rhythmStrength;
        // High onset density → more turbulence (jittery energetic fire)
        float densityWindMod = 1.0f + 0.6f * densityNorm;
        forceAdapter_->setWind(params_.windBase, scaledWindVar() * mod * densityWindMod);
    }

    // Run particle system (spawn → physics → render)
    // Particles are the only visual primitive; no heat buffer, no secondary layer
    ParticleGenerator::generate(matrix, audio);
}

void Fire::reset() {
    ParticleGenerator::reset();
    beatCount_ = 0;
    noiseTime_ = 0.0f;
    downbeatSpreadMult_ = 1.0f;
    downbeatColorShift_ = 0.0f;
    downbeatVelBoost_   = 0.0f;
    spawnBias_          = 0.0f;
}

void Fire::spawnParticles(float dt) {
    float spawnProb;
    uint8_t sparkCount = 0;

    // Decay downbeat transient effect state
    downbeatSpreadMult_ = max(1.0f, downbeatSpreadMult_ - 3.0f  * dt);  // 1.5 range / 0.5s
    downbeatColorShift_ = max(0.0f, downbeatColorShift_ - 2.0f  * dt);  // 1.0 range / 0.5s
    downbeatVelBoost_   = max(0.0f, downbeatVelBoost_   - 1.67f * dt);  // 0.5 range / 0.3s
    spawnBias_         *= max(0.0f, 1.0f - dt * 3.0f);                  // decay τ ≈ 0.33s

    // MUSIC-DRIVEN behavior (rhythmStrength weighted)
    float phasePulse = audio_.phaseToPulse();
    // musicSpawnPulse controls phase depth: 0=flat (no modulation), 1=full range (silent off-beat)
    float phasePump = (1.0f - params_.musicSpawnPulse) + params_.musicSpawnPulse * phasePulse;

    float musicSpawnProb = params_.baseSpawnChance * phasePump + params_.audioSpawnBoost * audio_.energy;

    // Transient response (stronger in music mode)
    uint8_t burst = scaledBurstSparks();
    if (audio_.pulse > params_.organicTransientMin) {
        float transientStrength = (audio_.pulse - params_.organicTransientMin) /
                                 (1.0f - params_.organicTransientMin);
        uint8_t musicSparks = (uint8_t)(burst * transientStrength);
        uint8_t organicSparks = 2;  // Small boost in organic mode
        sparkCount += (uint8_t)(organicSparks * (1.0f - audio_.rhythmStrength) +
                                musicSparks * audio_.rhythmStrength);
    }

    // Extra burst on predicted beats (only when rhythm is strong)
    if (beatHappened() && audio_.rhythmStrength > 0.3f) {
        beatCount_++;
        sparkCount += (uint8_t)(burst * audio_.rhythmStrength);

        // Downbeat (beat 1): maximum dramatic effect
        if (audio_.downbeat > 0.5f) {
            sparkCount += (uint8_t)(burst * audio_.downbeat);
            downbeatSpreadMult_ = 2.5f;   // Width expansion: 2.5× spread
            downbeatColorShift_ = 1.0f;   // Color temperature: hot white/blue tint
            downbeatVelBoost_   = 0.5f;   // Velocity burst: 1.5× for 0.3s
        }

        // Beat 3: secondary accent burst (50% of full beat burst)
        if (audio_.beatInMeasure == 3 && audio_.rhythmStrength > 0.5f) {
            sparkCount += (uint8_t)(burst * 0.5f * audio_.rhythmStrength);
        }

        // Beats 2 & 4: lighter spawn rate bump only
        if ((audio_.beatInMeasure == 2 || audio_.beatInMeasure == 4) &&
                audio_.rhythmStrength > 0.5f) {
            sparkCount += (uint8_t)(burst * 0.25f * audio_.rhythmStrength);
        }

        // Left/right rocking bias: odd beats lean one way, even beats the other
        if (audio_.beatInMeasure > 0 && audio_.rhythmStrength > 0.5f) {
            spawnBias_ = (audio_.beatInMeasure % 2 == 1) ? -0.25f : 0.25f;
        }
    }

    // ORGANIC-DRIVEN behavior (inverse rhythmStrength weighted)
    float organicSpawnProb = params_.baseSpawnChance + params_.audioSpawnBoost * audio_.energy;

    // Continuous spark rate proportional to energy (organic mode)
    if (audio_.energy > 0.05f) {
        uint8_t organicSparks = (uint8_t)((audio_.energy - 0.05f) * burst * 0.5f);
        sparkCount += (uint8_t)(organicSparks * (1.0f - audio_.rhythmStrength));
    }

    // BLEND spawn probability between modes
    spawnProb = organicSpawnProb * (1.0f - audio_.rhythmStrength) +
                musicSpawnProb * audio_.rhythmStrength;

    // Random baseline spawning
    if (random(1000) < spawnProb * 1000) {
        sparkCount++;
    }

    // Spawn sparks using layout-aware spawn region with variety
    uint8_t maxParts = scaledMaxParticles();
    for (uint8_t i = 0; i < sparkCount && pool_.getActiveCount() < maxParts; i++) {
        float x, y;
        getSpawnPosition(x, y);

        // Apply beat-in-measure left/right spawn bias (visual "rocking")
        if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
            x += spawnBias_ * crossDim_;
            x = max(0.0f, min((float)(width_ - 1), x));
        }

        // Base speed for this spark (dimension-scaled)
        float baseSpeed = scaledVelMin() +
                         random(100) * (scaledVelMax() - scaledVelMin()) / 100.0f;

        // Determine spark type (more variety during music mode)
        SparkType type;
        float varietyRoll = random(1000) / 1000.0f;

        // Music mode: favor burst sparks on transients, fast sparks otherwise
        // Organic mode: mix of fast sparks and slow embers
        if (audio_.rhythmStrength > 0.5f && audio_.pulse > 0.3f) {
            type = SparkType::BURST_SPARK;  // High-energy transient
        } else if (varietyRoll < params_.fastSparkRatio) {
            type = SparkType::FAST_SPARK;   // Primary sparks
        } else {
            type = SparkType::SLOW_EMBER;   // Glowing embers
        }

        spawnTypedParticle(type, x, y, baseSpeed);
    }
}

void Fire::spawnTypedParticle(SparkType type, float x, float y, float baseSpeed) {
    float vx, vy;
    getInitialVelocity(baseSpeed, vx, vy);

    // Add spread perpendicular to main direction (dimension-scaled)
    // Downbeat widens the flame: spread multiplier decays from 2.5× back to 1.0×
    float spreadAmount = (random(200) - 100) * scaledSpread() * downbeatSpreadMult_ / 100.0f;
    if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
        vx += spreadAmount;  // Matrix: horizontal spread
    } else {
        vy += spreadAmount * 0.3f;  // Linear: minimal vertical spread
    }

    // Blend velocity multiplier between organic and music modes
    float organicVelMult = 0.8f;
    float phasePulse = audio_.phaseToPulse();
    float musicVelMult = 0.8f + 0.4f * phasePulse + 0.3f * audio_.pulse;  // Faster on-beat + transient boost
    float velocityMult = organicVelMult * (1.0f - audio_.rhythmStrength) +
                        musicVelMult * audio_.rhythmStrength;

    // Onset density modulates particle character (0=ambient/languid, 1=dense/jittery)
    float densityNorm = min(1.0f, audio_.onsetDensity / 6.0f);
    // High density: shorter lifespan (0.75×), low density: longer (1.5×)
    float densityLifeMult = 1.5f - 0.75f * densityNorm;

    // Type-specific characteristics
    uint8_t intensity, lifespan;
    float speedMult;

    switch (type) {
    case SparkType::FAST_SPARK: {
        // Sort min/max so random() always gets valid (lo, hi) even if misconfigured via serial
        int lo = min((int)params_.intensityMin, (int)params_.intensityMax);
        int hi = max((int)params_.intensityMin, (int)params_.intensityMax) + 1;
        intensity = (uint8_t)random(lo, hi);
        lifespan = (uint8_t)min(255.0f, params_.defaultLifespan * densityLifeMult);
        speedMult = 1.0f;
        break;
    }

    case SparkType::SLOW_EMBER: {
        // Embers are dimmer than sparks; guard bounds to prevent inverted/zero range
        // which would cause UB (random() requires max > min)
        int lo = max(0, (int)params_.intensityMin - 30);
        int hi = max(0, (int)params_.intensityMax - 50);
        if (hi <= lo) hi = lo + 1;              // Prevent random(x, x) or inverted range
        intensity = (uint8_t)max(1L, random(lo, hi));  // max(1,...) ensures spawn succeeds
        // Embers already long-lived; density further modulates: languid embers linger even longer
        lifespan = (uint8_t)min(255.0f, params_.defaultLifespan * 1.5f * densityLifeMult);
        speedMult = 0.6f;  // 40% slower
        break;
    }

    case SparkType::BURST_SPARK:
        // Maximum brightness on transient
        intensity = params_.intensityMax;
        lifespan = (uint8_t)(params_.defaultLifespan * 0.8f);  // always short-lived
        speedMult = 1.0f;
        break;

    default:
        intensity = params_.intensityMax;
        lifespan = params_.defaultLifespan;
        speedMult = 1.0f;
        break;
    }

    // Phase-driven intensity boost: brighter on-beat, dimmer off-beat
    if (audio_.rhythmStrength > 0.3f) {
        int boost = (int)(phasePulse * 35 * audio_.rhythmStrength);
        intensity = (uint8_t)min(255, (int)intensity + boost);
    }

    // Downbeat velocity burst: up to 1.5× at peak, decays over 0.3s
    float velBurst = 1.0f + downbeatVelBoost_;

    // Apply speed and velocity multipliers
    vx *= velocityMult * speedMult * velBurst;
    vy *= velocityMult * speedMult * velBurst;

    // Spawn the particle (trailHeatFactor=0: heat is per-particle intensity, not heat buffer)
    pool_.spawn(x, y, vx, vy, intensity, lifespan, 1.0f,
               ParticleFlags::GRAVITY | ParticleFlags::WIND | ParticleFlags::FADE,
               0);
}

void Fire::updateParticle(Particle* p, float dt) {
    if (params_.thermalForce <= 0.0f) return;

    // Thermal buoyancy: hotter particles rise faster.
    // As FADE flag reduces intensity over lifetime, buoyancy decreases naturally.
    float heat = p->intensity / 255.0f;

    // Phase-driven breathing: surge upward on-beat (1.0), hover off-beat (0.5)
    // Only in music mode; in organic mode phasePulse is ~0.5 regardless
    float phaseMod = 1.0f;
    if (audio_.rhythmStrength > 0.3f) {
        float phasePulse = audio_.phaseToPulse();  // 1.0 on-beat, 0.0 off-beat
        phaseMod = (0.5f + 0.5f * phasePulse) *        // 0.5 off-beat → 1.0 on-beat
                   (1.0f - audio_.rhythmStrength) +     // organic contribution
                   (0.5f + 0.5f * phasePulse) *         // blended
                    audio_.rhythmStrength;
        phaseMod = 0.5f + 0.5f * phasePulse;           // simplified: 0.5–1.0 range
    }

    // At full intensity (1.0): applies params_.thermalForce LEDs/sec^2 upward.
    // p->mass is clamped to 0.01 minimum by ParticlePool::spawn, no div-by-zero risk.
    float thermal = scaledThermalForce() * phaseMod;
    if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
        p->vy -= (heat * thermal / p->mass) * dt;  // Matrix: upward = negative Y
    } else {
        p->vx += (heat * thermal / p->mass) * dt;  // Linear: forward = positive X
    }

    // Phase-driven drag: slightly more drag off-beat (lazy float), less on-beat (fast rise)
    if (audio_.rhythmStrength > 0.3f) {
        float phasePulse = audio_.phaseToPulse();
        float extraDrag = 1.0f - 0.015f * (1.0f - phasePulse) * audio_.rhythmStrength;
        p->vx *= extraDrag;
        p->vy *= extraDrag;
    }
}

void Fire::renderParticle(const Particle* p, PixelMatrix& matrix) {
    int x = (int)p->x;
    int y = (int)p->y;

    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        uint32_t color = particleColor(p->intensity);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        // ADDITIVE BLENDING: Particles add to existing colors
        RGB existing = matrix.getPixel(x, y);
        matrix.setPixel(x, y,
                       min(255, (int)existing.r + r),
                       min(255, (int)existing.g + g),
                       min(255, (int)existing.b + b));
    }
}

uint32_t Fire::particleColor(uint8_t intensity) const {
    // Smooth fire palette with 6 color stops (black → deep red → red → orange → yellow-orange → bright yellow)
    // Each stop is at a specific intensity value with RGB color
    struct ColorStop {
        uint8_t position;
        uint8_t r, g, b;
    };

    const ColorStop palette[] = {
        {0,   0,   0,   0},     // Black
        {51,  64,  0,   0},     // Deep red (20%)
        {102, 255, 0,   0},     // Red (40%)
        {153, 255, 128, 0},     // Orange (60%)
        {204, 255, 200, 0},     // Yellow-orange (80%)
        {255, 255, 255, 64}     // Bright yellow (100%)
    };
    const int paletteSize = 6;

    // Find surrounding color stops
    int lowerIdx = 0;
    int upperIdx = 1;

    for (int i = 0; i < paletteSize - 1; i++) {
        if (intensity >= palette[i].position && intensity <= palette[i+1].position) {
            lowerIdx = i;
            upperIdx = i + 1;
            break;
        }
    }

    // Interpolate between stops
    const ColorStop& lower = palette[lowerIdx];
    const ColorStop& upper = palette[upperIdx];

    float range = upper.position - lower.position;
    float t = (range > 0) ? (float)(intensity - lower.position) / range : 0.0f;

    uint8_t r = (uint8_t)(lower.r + t * (upper.r - lower.r));
    uint8_t g = (uint8_t)(lower.g + t * (upper.g - lower.g));
    uint8_t b = (uint8_t)(lower.b + t * (upper.b - lower.b));

    // Downbeat color temperature shift: push toward hot white/pale blue
    // downbeatColorShift_ decays 1.0→0.0 over 0.5s after each downbeat
    if (downbeatColorShift_ > 0.0f) {
        float s = downbeatColorShift_;
        r = (uint8_t)min(255, (int)r + (int)(s * 40));   // brighten red channel
        g = (uint8_t)min(255, (int)g + (int)(s * 50));   // push toward white
        b = (uint8_t)min(255, (int)b + (int)(s * 80));   // blue tint → "hotter than fire"
    }

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

