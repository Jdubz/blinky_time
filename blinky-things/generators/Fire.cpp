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
    // Palette bias driven by energy
    float targetBias = audio.energy;
    paletteBias_ += (targetBias - paletteBias_) * min(1.0f, 2.0f / 60.0f);

    // Wind: baseline turbulence + transient gusts
    // Music mode: stronger, beat-synced gusts. Organic mode: pulse-driven gusts.
    if (forceAdapter_) {
        float organicGust = 1.0f + 2.0f * audio.pulse;
        float musicGust = 1.0f + 3.0f * audio.plpPulse;
        float gust = organicGust * (1.0f - audio.rhythmStrength) +
                     musicGust * audio.rhythmStrength;
        forceAdapter_->setWind(0.0f, scaledWindVar() * gust);
    }

    updateHeatGrid();

    ParticleGenerator::generate(matrix, audio);
}

void Fire::reset() {
    ParticleGenerator::reset();
    paletteBias_ = 0.0f;
    memset(heatGrid_, 0, sizeof(heatGrid_));
}

void Fire::spawnParticles(float dt) {
    // Energy is the PRIMARY driver. Ambient (energy ~0.1) = small embers.
    // Full music (energy ~0.9) = roaring flames filling 100% of height.
    float e = audio_.energy;

    // Spawn rate scales dramatically with energy: 3x at full vs ambient
    // Music mode: plpPulse modulates spawn rate for breathing flame at beat tempo
    float spawnMod = 0.5f + 2.5f * e;
    if (audio_.rhythmStrength > 0.3f) {
        float breathe = 0.5f + 0.5f * audio_.plpPulse;  // 0.5-1.0 at beat rate
        spawnMod *= (1.0f - audio_.rhythmStrength) + breathe * audio_.rhythmStrength;
    }
    float spawnProb = params_.baseSpawnChance * spawnMod;
    float expectedSparks = spawnProb * crossDim_;
    uint8_t sparkCount = (uint8_t)min(expectedSparks, 255.0f);
    float frac = expectedSparks - sparkCount;
    if (frac > 0.0f && random(1000) < (int)(frac * 1000)) sparkCount++;

    // MUSIC-DRIVEN burst: beat-synced spark bursts (scales with rhythmStrength)
    if (audio_.rhythmStrength > 0.3f && audio_.pulse > params_.organicTransientMin) {
        float strength = (audio_.pulse - params_.organicTransientMin)
                       / (1.0f - params_.organicTransientMin);
        sparkCount += (uint8_t)(scaledBurstSparks() * strength * (0.5f + 0.5f * audio_.rhythmStrength));
    }

    // ORGANIC burst: transient-driven (fades as rhythm takes over)
    if (audio_.rhythmStrength < 0.5f && audio_.pulse > params_.organicTransientMin) {
        float strength = (audio_.pulse - params_.organicTransientMin)
                       / (1.0f - params_.organicTransientMin);
        sparkCount += (uint8_t)(scaledBurstSparks() * strength * (1.0f - audio_.rhythmStrength));
    }

    uint16_t maxParts = scaledMaxParticles();
    for (uint8_t i = 0; i < sparkCount && pool_.getActiveCount() < maxParts; i++) {
        float x, y;
        getSpawnPosition(x, y);

        // Per-particle random vigor adds organic variation (±20%)
        float vigor = 0.8f + random(400) * 0.001f;  // 0.8-1.2

        // Velocity: energy drives 30-100% of full speed. Vigor adds variation.
        float baseSpeed = scaledVelMin() +
                         random(100) * (scaledVelMax() - scaledVelMin()) / 100.0f;
        float velMult = (0.3f + 0.7f * e) * vigor;
        baseSpeed *= velMult;

        float vx, vy;
        getInitialVelocity(baseSpeed, vx, vy);

        // Perpendicular spread
        float spreadAmount = (random(200) - 100) * scaledSpread() / 100.0f;
        if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
            vx += spreadAmount;
        } else {
            vy += spreadAmount * 0.3f;
        }

        // Intensity: energy-driven base + vigor variation
        int lo = min((int)params_.intensityMin, (int)params_.intensityMax);
        int hi = max((int)params_.intensityMin, (int)params_.intensityMax) + 1;
        uint8_t intensity = (uint8_t)min(255.0f, random(lo, hi) * (0.3f + 0.7f * e) * vigor);

        // Lifespan: energy-driven. Low energy = short embers, high = tall flames.
        float lifeMult = 0.4f + 0.8f * e;  // 0.4x at ambient, 1.2x at max
        uint8_t lifespan = (uint8_t)min(255.0f, params_.defaultLifespan * lifeMult * vigor);

        // Store vigor in mass field (used by updateParticle for sustained buoyancy)
        float mass = 1.0f / max(0.3f, vigor);  // High vigor = low mass = more buoyancy

        pool_.spawn(x, y, vx, vy, intensity, lifespan, mass,
                   ParticleFlags::GRAVITY | ParticleFlags::WIND | ParticleFlags::FADE);
    }
}

void Fire::updateParticle(Particle* p, float dt) {
    // No per-frame intensity modification — fade handles brightness decay,
    // spawn handles initial brightness. Energy modulation happens at render time.

    // Extend lifespan when energy is high (all particles get more life = taller flame)
    if (audio_.energy > 0.5f && p->maxAge < 250) {
        p->maxAge += 1;
    }

    // Fluid grid forces
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
    // Sub-pixel splat: distribute particle color to up to 4 neighboring pixels
    // based on fractional position. Creates glow instead of sharp dots.
    float fx = p->x - 0.5f;  // Center on pixel
    float fy = p->y - 0.5f;
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    float dx = fx - x0;  // Fractional offset 0-1
    float dy = fy - y0;

    uint32_t color = particleColor(p->intensity);
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    // Energy modulation at render time: whole flame breathes with audio.
    // Organic: energy + pulse. Music: energy + plpPulse breathing.
    float organicBright = 0.4f + 0.6f * audio_.energy + 0.4f * audio_.pulse;
    float musicBright = 0.3f + 0.4f * audio_.energy + 0.5f * audio_.plpPulse;
    float brightnessScale = organicBright * (1.0f - audio_.rhythmStrength) +
                           musicBright * audio_.rhythmStrength;
    if (brightnessScale > 1.0f) brightnessScale = 1.0f;
    r = (uint8_t)(r * brightnessScale);
    g = (uint8_t)(g * brightnessScale);
    b = (uint8_t)(b * brightnessScale);

    // Bilinear weights for 4 pixels
    float w00 = (1.0f - dx) * (1.0f - dy);
    float w10 = dx * (1.0f - dy);
    float w01 = (1.0f - dx) * dy;
    float w11 = dx * dy;

    // Splat to each pixel with weight
    auto splat = [&](int px, int py, float w) {
        if (px >= 0 && px < width_ && py >= 0 && py < height_ && w > 0.01f) {
            RGB existing = matrix.getPixel(px, py);
            matrix.setPixel(px, py,
                min(255, (int)existing.r + (int)(r * w)),
                min(255, (int)existing.g + (int)(g * w)),
                min(255, (int)existing.b + (int)(b * w)));
        }
    };

    splat(x0, y0, w00);
    splat(x0 + 1, y0, w10);
    splat(x0, y0 + 1, w01);
    splat(x0 + 1, y0 + 1, w11);
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

