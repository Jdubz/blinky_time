#include "Fire.h"
#include "../math/SimplexNoise.h"
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
    // Physics components use placement new, no delete needed.
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
    audio_ = audio;
    uint32_t currentMs = millis();
    float dt = (currentMs - lastUpdateMs_) / 1000.0f;
    lastUpdateMs_ = currentMs;
    if (dt > 0.1f) dt = 0.1f;  // clamp on first frame / long stalls

    // PR #149 review: removed Fire's internal matrix.clear() — the pipeline
    // already clears at RenderPipeline.cpp before calling generate(), so
    // this was a double-clear that violated the generator contract (other
    // generators don't self-clear). Harmless today but breaks compositing
    // if Fire is ever used as a layer.

    // === Per-frame amplitude envelope (b200) ===
    float es = audio.energy - params_.silenceFloor;
    float range = max(0.05f, 1.0f - params_.silenceFloor);
    frameOvershoot_ = es < 0.0f ? 0.0f : (es > range ? 1.0f : es / range);

    // Palette bias driven by energy
    float targetBias = audio.energy;
    paletteBias_ += (targetBias - paletteBias_) * min(1.0f, 2.0f / 60.0f);

    // === Audio envelope for heightmap (b202) ===
    // Use audio.pulse (already has attack-decay envelope from AudioTracker
    // and now gated by AdaptiveMic noise gate). Additional smoothing here
    // for the height response — fast attack so impulses register, slower
    // decay so the launch is visible and "settles back down quickly."
    float impulseTarget = max((float)audio.pulse, frameOvershoot_);
    if (impulseTarget > audioEnvelope_) {
        audioEnvelope_ = impulseTarget;       // instant attack
    } else {
        // Exponential decay: ~250ms time constant for "settles back down quickly"
        float decay = expf(-dt / 0.25f);
        audioEnvelope_ *= decay;
    }

    // === Noise time advancement ===
    // Noise animates faster during loud audio so the smolder visibly
    // "wiggles harder" when there's music — per operator spec.
    float noiseSpeed = params_.noiseBaseSpeed *
                       (1.0f + params_.noiseAudioSpeedMult * frameOvershoot_);
    noiseTime_ += dt * noiseSpeed;

    // === Ember-floor render ===
    // Both layouts derive their ember floor from the SAME audio-driven flame
    // amount (audioFlameAmount()); only the visual mapping differs:
    //   matrix → per-column flame HEIGHT (bright base, dim tip)
    //   linear → "floating noise" COVERAGE along the strip (louder = more)
    const bool isLinear = (matrix.getHeight() <= 1);
    if (isLinear) {
        renderLinearEmber(matrix);
    } else {
        renderMatrixHeightmap(matrix);
    }

    // === Particle accent layer — BOTH layouts ===
    // Same edge-triggered burst logic (spawnParticles) for every layout. The
    // layout-appropriate spawn region + force adapter (chosen in
    // initPhysicsContext) make the bursts read correctly per layout:
    //   matrix → sparks launch upward from the bottom edge and fade
    //   linear → "splashes" spawn at random points and travel outward, fade
    // Layered ON TOP of the ember floor as the "launch spike" accent.
    if (forceAdapter_) {
        forceAdapter_->update(dt);
        // PR #149 review: restore audio-reactive wind gust amplitude.
        // The old generate() modulated wind by (1 + 2-3 × pulse/plpPulse)
        // so curl-noise turbulence visibly intensified on beats; the
        // initial heightmap rewrite removed that, leaving wind fixed
        // at scaledWindVar(). audioEnvelope_ provides the same beat-
        // tracking envelope used by the heightmap height-boost, so
        // wind now breathes with the same envelope.
        float gust = 1.0f + 2.5f * audioEnvelope_;
        forceAdapter_->setWind(0.0f, scaledWindVar() * gust);
    }
    // The heat grid is a 2D Eulerian buoyancy field (sparks → hot columns →
    // upward reinforcement). It has no meaning on a 1D strip where splashes
    // travel outward rather than rising, so it's matrix-only. With the grid
    // left cool on linear, applyGridForce() reads zeros and is a no-op.
    if (!isLinear) updateHeatGrid();
    spawnParticles(dt);
    updateParticles(dt);
    renderParticles(matrix);

    prevPhase_ = audio_.phase;
}

void Fire::reset() {
    ParticleGenerator::reset();
    paletteBias_ = 0.0f;
    memset(heatGrid_, 0, sizeof(heatGrid_));
}

// === MATRIX ember floor: per-column heightmap (b202 visual, UNCHANGED) ===
// Extracted verbatim from generate() so the linear layout can be a peer
// renderer rather than a slice. Each column draws a flame from the bottom up
// with a bright-base → dim-tip gradient; heights wiggle from spatial noise and
// audio impulses launch all columns higher. The arithmetic here is identical
// to the pre-b204 inline loop — do NOT "DRY" it through audioFlameAmount(),
// which would reorder the float adds and risk perturbing the matrix output.
void Fire::renderMatrixHeightmap(PixelMatrix& matrix) {
    const int W = matrix.getWidth();
    const int H = matrix.getHeight();
    const float audioBright = 0.40f + 0.60f * frameOvershoot_;
    for (int x = 0; x < W; x++) {
        float n01 = (SimplexNoise::noise2D(x * params_.noiseSpatialScale,
                                           noiseTime_) + 1.0f) * 0.5f;
        float smolderRange = params_.smolderHeight * params_.noiseAmplitude;
        float heightFrac = params_.smolderHeight + (n01 - 0.5f) * smolderRange * 2.0f;
        heightFrac += audioEnvelope_ * params_.audioHeightBoost;
        if (heightFrac < 0.02f) heightFrac = 0.02f;
        if (heightFrac > params_.maxFlameHeight) heightFrac = params_.maxFlameHeight;

        int heightLEDs = (int)(heightFrac * H + 0.5f);
        if (heightLEDs < 1) heightLEDs = 1;
        for (int y = 0; y < heightLEDs && y < H; y++) {
            float gradT = 1.0f - (float)y / (float)heightLEDs;
            uint8_t intensity = (uint8_t)(255.0f * gradT * audioBright);
            uint32_t color = particleColor(intensity);
            matrix.setPixel(x, H - 1 - y,
                            (color >> 16) & 0xFF,
                            (color >> 8) & 0xFF,
                            color & 0xFF);
        }
    }
}

// === LINEAR ember floor: "floating noise" sized by audio (b204) ===
// Same fire LOGIC as the matrix (audioFlameAmount() = smolder lifted by the
// shared audio envelope, animated by the shared noiseTime_), only the visual
// MAPPING differs: instead of becoming a column HEIGHT, the audio amount sets
// how much of the strip the glowing embers COVER. A spatial noise field
// selects which LEDs are "inside" the embers; because noiseTime_ advances
// (faster when loud), the lit blobs drift along the strip — "floating noise".
//
//   silence  → low coverage → a few dim, slowly-drifting embers
//   music     → coverage grows → more/larger embers, brighter
//   impulse   → coverage spikes toward the whole strip on each hit
//
// No vertical dimension is invented (the strip has none); coverage replaces
// height as the audio-reactive dimension. The pipeline pre-clears the matrix,
// so LEDs outside the embers stay dark.
void Fire::renderLinearEmber(PixelMatrix& matrix) {
    const int N = matrix.getWidth();
    if (N <= 0) return;

    // Coverage = the SAME audio-driven flame amount the matrix uses for
    // height, normalized by maxFlameHeight so a peak impulse can light the
    // whole strip while idle smolder lights only scattered embers.
    float amount = audioFlameAmount();
    if (amount > params_.maxFlameHeight) amount = params_.maxFlameHeight;
    float coverage = (params_.maxFlameHeight > 0.0f)
                     ? amount / params_.maxFlameHeight : 0.0f;   // 0..1
    if (coverage < 0.0f) coverage = 0.0f;
    if (coverage > 1.0f) coverage = 1.0f;

    // Brightness breathes with the audio envelope — same per-frame term the
    // matrix heightmap uses (frameOvershoot_), so both layouts dim/brighten
    // together.
    const float audioBright = 0.40f + 0.60f * frameOvershoot_;

    for (int i = 0; i < N; i++) {
        // Spatial noise (animated by the shared, audio-sped noiseTime_)
        // selects which LEDs fall "inside" the floating embers.
        float n01 = (SimplexNoise::noise2D(i * params_.noiseSpatialScale,
                                           noiseTime_) + 1.0f) * 0.5f;
        if (n01 < coverage) {
            // Depth below the coverage threshold → brighter toward blob center,
            // fading to nothing at the blob edge for soft, organic embers.
            float depth = (coverage > 0.0f) ? (coverage - n01) / coverage : 0.0f;
            uint8_t intensity = (uint8_t)(255.0f * depth * audioBright);
            uint32_t color = particleColor(intensity);
            matrix.setPixel(i, 0,
                            (color >> 16) & 0xFF,
                            (color >> 8) & 0xFF,
                            color & 0xFF);
        }
    }
}

void Fire::spawnParticles(float dt) {
    // b202: baseline spawn removed — the heightmap IS the always-on
    // ember layer. Particles are now pure event-driven accents.
    // PR #149 review: dropped the local `overshoot` and `(void)e` —
    // `overshoot` became unused after removing the hasSignal gate, and
    // `e` is referenced directly via audio_.energy below.
    float e = audio_.energy;
    float rs = audio_.rhythmStrength;
    uint16_t totalSparks = 0;

    // === EDGE-TRIGGERED AUDIO BURST (b197) ===
    // Hierarchy: direct audio (audio_.pulse, derived from spectral flux of
    // raw mic input) is the PRIMARY trigger because it's the most reliable
    // signal we have — always available regardless of rhythm-lock state or
    // NN model performance. plpPulse and rhythmStrength REFINE the burst
    // strength when they're available, but never gate it. NN onset, when
    // it fires, also amplifies. This means the visual always reacts to
    // actual audio events, with extra punch when the rhythm tracker agrees.
    //
    // Earlier designs gated the trigger on rhythm-lock, which silenced
    // bursts entirely whenever rhythmStrength was low — at low-SNR sites
    // that meant bursts almost never fired. Operator feedback: "you're only
    // using nn and plpPulse outputs; direct audio is the most reliable."
    // PR #149 review: removed the `hasSignal = overshoot > 0` gate. It
    // was over-strict — `overshoot` requires audio.energy > silenceFloor
    // (0.25), but audio.pulse has a 240ms envelope that can persist above
    // burstThreshold long after energy noise-gates back to the smolder
    // floor. Result: sparse-kick tracks (one drum every second of quiet)
    // were having bursts suppressed on every kick. The upstream micGate
    // now zeroes audio.pulse during true silence (b201 + smolder gate
    // fix), so a redundant overshoot check here just blocks legitimate
    // sub-floor kicks.
    bool isBeat = (audio_.pulse > params_.burstThreshold) ||
                  (audio_.plpPulse > params_.burstThreshold + 0.10f);
    if (isBeat && !lastBeat_) {
        // Combined strength: direct audio is the floor, refinements stack.
        float strength = audio_.pulse;
        if (rs > 0.1f && audio_.plpPulse > strength) {
            strength = audio_.plpPulse;
        }
        if (strength > 1.0f) strength = 1.0f;
        if (strength < params_.burstThreshold) strength = params_.burstThreshold;

        // Burst size = crossDim * (base + gain * strength) — all tunable.
        // PR #149 review: compute in uint16_t then clamp. Previously cast
        // straight to uint8_t which silently wrapped to 0 on wider devices
        // when operators tuned burstsizegain up (e.g. crossDim=16 *
        // (2 + 14×1.0) = 256 → wrapped to 0, bursts silently stopped).
        // burstParticles_ stays uint8_t (the spawn loop iterates uint8_t)
        // so cap at 255.
        float rawCount = crossDim_ *
                         (params_.burstSizeBase + params_.burstSizeGain * strength);
        if (rawCount < 0.0f) rawCount = 0.0f;
        if (rawCount > 255.0f) rawCount = 255.0f;
        uint8_t burstAdd = (uint8_t)rawCount;
        burstParticles_ = burstAdd;
        burstStrength_  = strength;
        totalSparks    += burstAdd;
    }
    lastBeat_ = isBeat;

    uint8_t sparkCount = (uint8_t)min((int)totalSparks, 255);
    uint16_t maxParts = scaledMaxParticles();
    // Burst particles are spawned FIRST so their dramatic params don't get
    // crowded out if the pool is near capacity.
    uint8_t burstRemaining = burstParticles_;
    burstParticles_ = 0;  // consumed
    for (uint8_t i = 0; i < sparkCount && pool_.getActiveCount() < maxParts; i++) {
        const bool isBurst = (i < burstRemaining);
        float x, y;
        getSpawnPosition(x, y);

        // Per-particle random vigor adds organic variation (±20%)
        float vigor = 0.8f + random(400) * 0.001f;  // 0.8-1.2

        // Velocity: baseline particles use energy-driven speed; BURST
        // particles get a major velocity multiplier so they visibly
        // surge up the tube and arrive at the top before fading. That
        // visible rise IS the "movement" half of the user's mental model.
        float baseSpeed = scaledVelMin() +
                         random(100) * (scaledVelMax() - scaledVelMin()) / 100.0f;
        float velMult;
        if (isBurst) {
            // Burst velocity is params_.burstVelMult × vigor, scaled by
            // strength so weak pulses produce gentle bursts, strong pulses
            // produce dramatic ones. Tunable via `set burstvelmult <N>`.
            velMult = params_.burstVelMult * (0.5f + 0.5f * burstStrength_) * vigor;
        } else {
            velMult = (0.5f + 0.5f * e) * vigor;
        }
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

        // === LINEAR "splash" mapping (b204) ===
        // On a strip the matrix's upward spark becomes a splash: spawned at a
        // random point (x already random from the spawn region) that travels
        // OUTWARD along the strip. The strip is a single row, so we pin the
        // particle to the row center and put all velocity on the strip axis —
        // otherwise the spawn region's vertical velocity component carries the
        // particle off the 1-LED-tall row and it vanishes within a frame, and
        // a random spawn-y would dim it through the bilinear splat. Direction
        // is randomized per particle so a burst sprays both ways from its
        // origin = "splashes travelling outward at random locations".
        if (height_ <= 1) {
            y = 0.5f;
            float dir = (random(2) == 0) ? -1.0f : 1.0f;
            vx = dir * baseSpeed;
            vy = 0.0f;
        }

        // Intensity: baseline sparks orange-to-yellow per params; BURST
        // sparks slam to full white-hot so they're visually distinct from
        // ambient embers — that's the "heat + brightness" half of the
        // user's mental model of a musical pulse.
        int lo = min((int)params_.intensityMin, (int)params_.intensityMax);
        int hi = max((int)params_.intensityMin, (int)params_.intensityMax) + 1;
        uint8_t intensity;
        if (isBurst) {
            intensity = (uint8_t)(220 + random(36));  // 220-255, full bright
        } else {
            intensity = (uint8_t)min(255.0f, random(lo, hi) * vigor);
        }

        // Lifespan: burst-mode uses params_.burstLifeMult (tunable). Shorter
        // life makes pulses pop and disappear; longer life makes them linger.
        float lifeMult = isBurst ? (params_.burstLifeMult * (0.7f + 0.5f * burstStrength_))
                                 : (0.6f + 0.6f * e);
        uint8_t lifespan = (uint8_t)min(255.0f, params_.defaultLifespan * lifeMult * vigor);

        // Store vigor in mass field (used by updateParticle for sustained buoyancy)
        float mass = 1.0f / max(0.3f, vigor);  // High vigor = low mass = more buoyancy

        pool_.spawn(x, y, vx, vy, intensity, lifespan, mass,
                   ParticleFlags::GRAVITY | ParticleFlags::WIND | ParticleFlags::FADE);
    }
}

void Fire::updateParticle(Particle* p, float dt) {
    // No per-frame audio coupling here — that produces a uniform wash.
    // Audio events express themselves through SPAWN (count, velocity,
    // intensity at birth), then particles execute their own trajectory.
    // Extend lifespan when energy is high (all particles get more life = taller flame)
    if (audio_.energy > 0.5f && p->maxAge < 250) {
        p->maxAge += 1;
    }
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

    // Ember pulsing: slow particles pulse sinusoidally instead of fading linearly.
    // Creates "breathing coals" effect. Phase derived from position (deterministic,
    // no extra storage). Only applied to dim particles (embers, not bright sparks).
    uint8_t renderIntensity = p->intensity;
    if (p->intensity < 120 && p->intensity > 10) {
        float phase = p->x * 2.3f + p->y * 1.7f;  // Deterministic phase from position
        float age = (float)(255 - p->intensity) / 255.0f;  // 0=new, 1=dying
        float pulse = 0.7f + 0.3f * sinf(age * 12.0f + phase);
        renderIntensity = (uint8_t)(p->intensity * pulse);
    }

    uint32_t color = particleColor(renderIntensity);
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    // === CONTINUOUS AMPLITUDE GATE (b200) ===
    // Brightness smoothly tracks the per-frame amplitude envelope
    // (frameOvershoot_, computed once in generate() and shared with the
    // spawn-rate gate). This is the "Ruben's-tube feel" layer:
    //   silence  → gate near 0 → particles invisible (matches no-spawn state)
    //   quiet    → gate low    → flame dim
    //   loud     → gate ≈ 1    → particles render at near-full intensity
    //
    // Independent of the event-burst layer in spawnParticles: bursts spawn
    // bright/fast particles at musical events; this gate makes the whole
    // flame breathe with the audio envelope between bursts. Tunable via
    // `set audiobright <0..1>` — 0 disables the gate (constant brightness),
    // 1 makes brightness fully amplitude-driven.
    //
    // Uses the SMOOTH audio.energy envelope (not the noisy pulse signal)
    // so the gate doesn't flicker on amplified mic noise.
    float amount = params_.audioBrightAmount;
    float continuousGate = (1.0f - amount) + amount * frameOvershoot_;
    float brightnessScale = continuousGate * 0.95f;  // 0.95 ceiling for headroom
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

