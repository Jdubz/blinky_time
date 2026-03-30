#include "Fire.h"
#include "../physics/PhysicsContext.h"
#include "../physics/EdgeSpawnRegion.h"
#include "../physics/RandomSpawnRegion.h"
#include "../physics/KillBoundary.h"
#include "../physics/WrapBoundary.h"
#include "../physics/MatrixForceAdapter.h"
#include "../physics/LinearForceAdapter.h"
#include <Arduino.h>

Fire::Fire()
    : params_(), paletteBias_(0.0f),
      gridW_(FIRE_GRID_W), gridH_(FIRE_GRID_H),
      cellW_(1.0f), cellH_(1.0f) {
    memset(heatGrid_, 0, sizeof(heatGrid_));
}

Fire::~Fire() {
    // Physics components use placement new, no delete needed
}

bool Fire::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;
    initGrid();
    return true;
}

void Fire::initPhysicsContext() {
    drag_ = params_.drag;

    bool wrap = PhysicsContext::shouldWrapByDefault(layout_);

    spawnRegion_ = PhysicsContext::createSpawnRegion(
        layout_, GeneratorType::FIRE, width_, height_, spawnBuffer_);

    boundary_ = PhysicsContext::createBoundary(
        layout_, GeneratorType::FIRE, wrap, boundaryBuffer_);

    forceAdapter_ = PhysicsContext::createForceAdapter(layout_, forceBuffer_);
    if (forceAdapter_) {
        forceAdapter_->setWind(0.0f, scaledWindVar());
    }
}

void Fire::generate(PixelMatrix& matrix, const AudioControl& audio) {
    // Smooth palette bias toward target (energy × rhythmStrength)
    float targetBias = audio.energy * audio.rhythmStrength;
    paletteBias_ += (targetBias - paletteBias_) * min(1.0f, 2.0f / 60.0f);  // ~0.5s tau

    // Modulate wind turbulence by audio (phase breathing + transient gusts + onset density)
    if (forceAdapter_) {
        float phasePulse = audio.phaseToPulse();
        float phaseWind = 0.3f + 0.7f * phasePulse;          // calms between beats
        float transientGust = 1.0f + 2.0f * audio.pulse;     // spike on hits
        float mod = 1.0f * (1.0f - audio.rhythmStrength) +
                    phaseWind * transientGust * audio.rhythmStrength;
        float densityNorm = min(1.0f, audio.onsetDensity / 6.0f);
        float densityWindMod = 1.0f + 0.6f * densityNorm;
        forceAdapter_->setWind(0.0f, scaledWindVar() * mod * densityWindMod);
    }

    // Cool grid and splat heat from current particle positions
    updateHeatGrid();

    ParticleGenerator::generate(matrix, audio);
}

void Fire::reset() {
    ParticleGenerator::reset();
    paletteBias_ = 0.0f;
    memset(heatGrid_, 0, sizeof(heatGrid_));
}

void Fire::spawnParticles(float dt) {
    float phasePulse = audio_.phaseToPulse();

    // Spawn probability: energy-driven, phase-modulated in music mode
    float phasePump = (1.0f - params_.musicSpawnPulse) + params_.musicSpawnPulse * phasePulse;
    float spawnProb = (params_.baseSpawnChance + params_.audioSpawnBoost * audio_.energy)
                    * (1.0f - audio_.rhythmStrength + audio_.rhythmStrength * phasePump);

    // Scale spawn rate by crossDim (same pattern as burstSparks, windVariation, etc.)
    // spawnProb is now density (expected sparks per crossDim-unit per frame).
    // Poisson-like sampling: integer part always spawns, fractional part spawns stochastically.
    float expectedSparks = spawnProb * crossDim_;
    uint8_t sparkCount = (uint8_t)min(expectedSparks, 255.0f);
    float frac = expectedSparks - sparkCount;
    if (frac > 0.0f && random(1000) < (int)(frac * 1000)) {
        sparkCount++;
    }

    // Transient burst
    uint8_t burst = scaledBurstSparks();
    if (audio_.pulse > params_.organicTransientMin) {
        float strength = (audio_.pulse - params_.organicTransientMin)
                       / (1.0f - params_.organicTransientMin);
        sparkCount += (uint8_t)(burst * strength);
    }

    // Beat burst in music mode
    if (beatHappened() && audio_.rhythmStrength > 0.3f) {
        sparkCount += (uint8_t)(burst * audio_.rhythmStrength);
    }

    // Density-based lifespan: ambient fire lingers, dense/fast fire burns quick
    float densityNorm = min(1.0f, audio_.onsetDensity / 6.0f);
    float densityLifeMult = 1.5f - 0.75f * densityNorm;

    uint16_t maxParts = scaledMaxParticles();
    for (uint8_t i = 0; i < sparkCount && pool_.getActiveCount() < maxParts; i++) {
        float x, y;
        getSpawnPosition(x, y);

        float baseSpeed = scaledVelMin() +
                         random(100) * (scaledVelMax() - scaledVelMin()) / 100.0f;
        float vx, vy;
        getInitialVelocity(baseSpeed, vx, vy);

        // Perpendicular spread
        float spreadAmount = (random(200) - 100) * scaledSpread() / 100.0f;
        if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
            vx += spreadAmount;
        } else {
            vy += spreadAmount * 0.3f;
        }

        // Velocity boost on-beat in music mode
        float velMult = 0.8f + 0.4f * phasePulse * audio_.rhythmStrength;
        vx *= velMult;
        vy *= velMult;

        // Intensity in [min, max], boosted on-beat
        int lo = min((int)params_.intensityMin, (int)params_.intensityMax);
        int hi = max((int)params_.intensityMin, (int)params_.intensityMax) + 1;
        uint8_t intensity = (uint8_t)random(lo, hi);
        if (audio_.rhythmStrength > 0.3f) {
            int boost = (int)(phasePulse * 35 * audio_.rhythmStrength);
            intensity = (uint8_t)min(255, (int)intensity + boost);
        }

        uint8_t lifespan = (uint8_t)min(255.0f, params_.defaultLifespan * densityLifeMult);

        pool_.spawn(x, y, vx, vy, intensity, lifespan, 1.0f,
                   ParticleFlags::GRAVITY | ParticleFlags::WIND | ParticleFlags::FADE);
    }
}

void Fire::updateParticle(Particle* p, float dt) {
    // Per-particle thermal buoyancy: hotter particles rise faster.
    // As FADE flag reduces intensity over lifetime, buoyancy decreases naturally.
    if (params_.thermalForce > 0.0f) {
        float heat = p->intensity / 255.0f;

        // Phase breathing in music mode: surge on-beat (phaseMod=1.0), hover off-beat (0.5)
        float phaseMod = 1.0f;
        if (audio_.rhythmStrength > 0.3f) {
            phaseMod = 0.5f + 0.5f * audio_.phaseToPulse();
        }

        float thermal = scaledThermalForce() * phaseMod;
        if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
            p->vy -= (heat * thermal / p->mass) * dt;  // Matrix: upward = negative Y
        } else {
            p->vx += (heat * thermal / p->mass) * dt;  // Linear: forward = positive X
        }
    }

    // Fluid grid forces: grid heat provides additional plume buoyancy and lateral clustering
    applyGridForce(p, dt);
}

void Fire::initGrid() {
    gridW_ = min(FIRE_GRID_W, (int)width_);
    gridH_ = min(FIRE_GRID_H, (int)height_);
    cellW_ = (float)width_ / gridW_;
    cellH_ = (float)height_ / gridH_;
    memset(heatGrid_, 0, sizeof(heatGrid_));
}

void Fire::updateHeatGrid() {
    // Cool all cells (exponential decay each frame)
    for (int i = 0; i < FIRE_GRID_W * FIRE_GRID_H; i++) {
        heatGrid_[i] *= params_.gridCoolRate;
    }

    // Splat heat from alive particles onto nearest grid cell.
    // SPLAT_GAIN chosen so a cell with ~3 particles at avg intensity reaches ~0.5 steady-state.
    // Steady-state: heat_ss = N * gain * intensity / (1 - coolRate)
    // At coolRate=0.88, 3 particles intensity=0.7: 3*0.04*0.7/0.12 = 0.7
    static const float SPLAT_GAIN = 0.04f;
    pool_.forEach([this](const Particle* p) {
        int gx = constrain((int)(p->x / cellW_), 0, gridW_ - 1);
        int gy = constrain((int)(p->y / cellH_), 0, gridH_ - 1);
        float& cell = heatGrid_[gy * FIRE_GRID_W + gx];
        cell += (p->intensity / 255.0f) * SPLAT_GAIN;
        if (cell > 1.0f) cell = 1.0f;
    });
}

void Fire::applyGridForce(Particle* p, float dt) {
    if (params_.buoyancyCoupling <= 0.0f && params_.pressureCoupling <= 0.0f) return;

    int gx = constrain((int)(p->x / cellW_), 0, gridW_ - 1);
    int gy = constrain((int)(p->y / cellH_), 0, gridH_ - 1);
    float heat = heatGrid_[gy * FIRE_GRID_W + gx];

    // Plume buoyancy: grid hot cells reinforce upward velocity in existing columns
    if (params_.buoyancyCoupling > 0.0f) {
        float buoyScale = traversalDim_ * params_.buoyancyCoupling;
        if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
            p->vy -= (heat * buoyScale / p->mass) * dt;  // upward = negative Y
        } else {
            p->vx += (heat * buoyScale / p->mass) * dt;  // forward = positive X
        }
    }

    // Lateral pressure: heat gradient pulls particles toward hot columns.
    // dHeat/dx > 0 means more heat to the right → push particle right (toward heat).
    if (params_.pressureCoupling > 0.0f) {
        float dHeatDx = 0.0f;
        if (gx > 0 && gx < gridW_ - 1) {
            dHeatDx = (heatGrid_[gy * FIRE_GRID_W + (gx + 1)] - heatGrid_[gy * FIRE_GRID_W + (gx - 1)]) * 0.5f;
        } else if (gx == 0 && gridW_ > 1) {
            dHeatDx = heatGrid_[gy * FIRE_GRID_W + 1] - heat;
        } else if (gx > 0) {
            dHeatDx = heat - heatGrid_[gy * FIRE_GRID_W + (gx - 1)];
        }

        float pressScale = crossDim_ * params_.pressureCoupling;
        if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
            p->vx += (dHeatDx * pressScale / p->mass) * dt;
        } else {
            p->vy += (dHeatDx * pressScale / p->mass) * dt;
        }
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
    // Audio-driven gamma: subtle remap to show more ember detail when loud.
    // paletteBias_ is already smoothed (0 = cool/warm, 1 = hot).
    float gamma = 1.1f - 0.2f * paletteBias_;  // Range 1.1 (cool) → 0.9 (hot)
    float normalized = powf(intensity / 255.0f, gamma);
    uint8_t remapped = (uint8_t)(normalized * 255.0f);

    // Two palettes blended by paletteBias_:
    //   bias low: warm (default campfire)
    //   bias high: hot (brighter oranges/yellows, for high energy + rhythm)
    // Hot palette stays fire-colored (no white/blue) to avoid washing out
    // with additive blending.
    struct ColorStop { uint8_t position, r, g, b; };

    // Warm palette: black → deep red → red → orange → yellow-orange → bright yellow
    static const ColorStop warm[] = {
        {0,   0,   0,   0},
        {51,  64,  0,   0},
        {102, 255, 0,   0},
        {153, 255, 128, 0},
        {204, 255, 200, 0},
        {255, 255, 255, 64}
    };

    // Hot palette: black → red → bright orange → intense yellow → hot yellow-white
    // Stays in warm hues — no blue/white that would wash out under additive blending
    static const ColorStop hot[] = {
        {0,   0,   0,   0},
        {51,  128, 8,   0},
        {102, 255, 80,  0},
        {153, 255, 180, 10},
        {204, 255, 230, 40},
        {255, 255, 255, 100}
    };

    const int paletteSize = 6;

    // Look up color in both palettes
    auto lookup = [&](const ColorStop* pal, uint8_t val, uint8_t& ro, uint8_t& go, uint8_t& bo) {
        int lo = 0, hi = 1;
        for (int i = 0; i < paletteSize - 1; i++) {
            if (val >= pal[i].position && val <= pal[i+1].position) {
                lo = i; hi = i + 1; break;
            }
        }
        float range = pal[hi].position - pal[lo].position;
        float t = (range > 0) ? (float)(val - pal[lo].position) / range : 0.0f;
        ro = (uint8_t)(pal[lo].r + t * (pal[hi].r - pal[lo].r));
        go = (uint8_t)(pal[lo].g + t * (pal[hi].g - pal[lo].g));
        bo = (uint8_t)(pal[lo].b + t * (pal[hi].b - pal[lo].b));
    };

    uint8_t wr, wg, wb, hr, hg, hb;
    lookup(warm, remapped, wr, wg, wb);
    lookup(hot,  remapped, hr, hg, hb);

    // Blend between warm and hot based on paletteBias_
    // Dead zone: warm is default, hot only kicks in above 0.4 (loud + rhythmic)
    float blend = constrain((paletteBias_ - 0.4f) / 0.5f, 0.0f, 1.0f);
    uint8_t r = (uint8_t)(wr + blend * (hr - wr));
    uint8_t g = (uint8_t)(wg + blend * (hg - wg));
    uint8_t b = (uint8_t)(wb + blend * (hb - wb));

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

