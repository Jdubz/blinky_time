#include "Fire.h"
#include "../math/SimplexNoise.h"
#include "../physics/PhysicsContext.h"
#include <new>           // std::nothrow
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
    if (linearHeat_) {
        delete[] linearHeat_;
        linearHeat_ = nullptr;
    }
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

    matrix.clear();

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

    // === Heightmap render ===
    // Per-column flame height = smolder + noise + audio-boost (capped).
    //   smolder: idle base height (10-15% by default)
    //   noise:   adds wiggly variation per column, animated with audio
    //   boost:   audio impulse adds height up to +30%
    //   cap:     never exceeds maxFlameHeight (default 50%)
    // Brightness gradient along each column: bright at base, fades toward tip.
    const int W = matrix.getWidth();
    const int H = matrix.getHeight();
    const bool isLinear = (H <= 1);

    if (isLinear) {
        // 1D heat-propagation fire (DOOM-style). Each LED has a heat value
        // that cools each frame and propagates toward the tip; the base
        // receives heat injection from the audio envelope. Visual result:
        // dim glow at base during silence, flame waves rising up the strip
        // on audio impulses, fading toward the tip. See renderLinear().
        renderLinear(matrix, dt);
    } else {
        // Matrix: per-column heightmap. Each column draws a flame from the
        // bottom up with a bright-to-dim gradient. Heights wiggle from
        // noise; audio impulses launch all columns higher.
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

    // === Particle burst layer (b202) — matrix only ===
    // Heightmap renders the always-on ember floor. Particles spawn on
    // audio impulses, surge upward, fade. Layered ON TOP of heightmap
    // to provide the "launch spike" accent.
    //
    // Skipped on linear layouts because particles have no vertical
    // dimension to travel in. The heat-propagation fire on linear
    // already provides the dramatic event response via heat injection
    // that visibly travels up the strip.
    if (!isLinear) {
        if (forceAdapter_) forceAdapter_->update(dt);
        updateHeatGrid();
        spawnParticles(dt);
        updateParticles(dt);
        renderParticles(matrix);
    }
    prevPhase_ = audio_.phase;
}

void Fire::reset() {
    ParticleGenerator::reset();
    paletteBias_ = 0.0f;
    memset(heatGrid_, 0, sizeof(heatGrid_));
    if (linearHeat_) {
        for (int i = 0; i < linearHeatCapacity_; i++) linearHeat_[i] = 0.0f;
    }
}

// === 1D heat-propagation fire for LINEAR layouts (b203) ===
// Classic DOOM-style fire on a strip. Each LED has a heat value (0-1).
// Each frame:
//   1. Cool: heat[i] *= coolRate
//   2. Propagate toward tip: heat[i] = blend(heat[i], heat[i-1]) for i > 0
//      so heat moves from base (i=0) toward tip (i=N-1) over multiple frames.
//   3. Inject at base from audio:
//        baseHeat = smolderHeight + audioEnvelope * audioHeightBoost (capped)
//      With per-frame noise variation so the base isn't perfectly steady.
// Then render: each LED color = warm-palette lookup on its heat value.
//
// Result on a strip:
//   - silence:   gentle glow at the base, mostly dark toward tip (smolder)
//   - music:     heat injected continuously, rises up the strip as glowing
//                wave that fades toward the tip
//   - impulses:  big heat spike at base, propagates up and decays — visible
//                "flame wave" travelling along the strip per beat
//
// No "vertical dimension" needed because the strip IS the vertical axis,
// with position along the strip representing "height" in the fire.
void Fire::renderLinear(PixelMatrix& matrix, float dt) {
    const int N = matrix.getWidth();
    if (N <= 0) return;

    // Lazy-allocate heat buffer
    if (!linearHeat_ || linearHeatCapacity_ < N) {
        if (linearHeat_) delete[] linearHeat_;
        linearHeat_ = new(std::nothrow) float[N];
        linearHeatCapacity_ = linearHeat_ ? N : 0;
        if (linearHeat_) {
            for (int i = 0; i < N; i++) linearHeat_[i] = 0.0f;
        }
    }
    if (!linearHeat_) return;

    // Frame-rate-independent cool & propagate rates. params_.linearCoolRate
    // and params_.linearPropRate are expressed in 60fps-frame units, scaled
    // to actual dt so the visual is consistent across frame rates.
    float dt60 = dt * 60.0f;
    if (dt60 > 4.0f) dt60 = 4.0f;
    float coolPerFrame = powf(params_.linearCoolRate, dt60);
    float propMix = 1.0f - powf(1.0f - params_.linearPropRate, dt60);

    // 1. Cool every position
    for (int i = 0; i < N; i++) {
        linearHeat_[i] *= coolPerFrame;
    }

    // 2. Propagate from base toward tip — iterate from tip DOWN to avoid
    //    same-pass clobbering. heat[i] absorbs some heat from heat[i-1].
    for (int i = N - 1; i > 0; i--) {
        linearHeat_[i] = linearHeat_[i] * (1.0f - propMix) +
                         linearHeat_[i - 1] * propMix;
    }

    // 3. Inject at base from smolder + audio envelope, with noise variation.
    //    noise gives ±50% wiggle on the smolder injection so the base
    //    looks alive even in silence.
    float baseSmolder = params_.smolderHeight;
    float n = SimplexNoise::noise2D(0.0f, noiseTime_);    // -1..1
    baseSmolder *= 1.0f + 0.5f * n;
    float injection = baseSmolder + audioEnvelope_ * params_.audioHeightBoost;
    if (injection > 1.0f) injection = 1.0f;
    if (injection > linearHeat_[0]) {
        linearHeat_[0] = injection;     // monotonic on rising (don't undercut existing heat)
    }

    // 4. Render heat → warm palette
    for (int i = 0; i < N; i++) {
        float h = linearHeat_[i];
        if (h < 0.0f) h = 0.0f;
        if (h > 1.0f) h = 1.0f;
        uint8_t intensity = (uint8_t)(255.0f * h);
        uint32_t color = particleColor(intensity);
        matrix.setPixel(i, 0,
                        (color >> 16) & 0xFF,
                        (color >> 8) & 0xFF,
                        color & 0xFF);
    }
}

void Fire::spawnParticles(float dt) {
    // Silence gate + amplitude envelope are pre-computed once per frame in
    // generate(), shared with renderParticle so spawn-rate and brightness
    // breathe together with the music. See generate() for the math.
    float e = audio_.energy;
    float overshoot = frameOvershoot_;

    // b202: baseline spawn removed — the heightmap IS the always-on
    // ember layer. Particles are now pure event-driven accents.
    float rs = audio_.rhythmStrength;
    uint16_t totalSparks = 0;
    (void)e;  // still used in the burst code below for energy-driven sizing

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
    // Bursts ALSO gated by silence — phantom transients on amplified mic
    // noise would otherwise fire bursts during true quiet.
    bool hasSignal = (overshoot > 0.0f);
    bool isBeat = hasSignal && ((audio_.pulse > params_.burstThreshold) ||
                                (audio_.plpPulse > params_.burstThreshold + 0.10f));
    if (isBeat && !lastBeat_) {
        // Combined strength: direct audio is the floor, refinements stack.
        float strength = audio_.pulse;
        if (rs > 0.1f && audio_.plpPulse > strength) {
            strength = audio_.plpPulse;
        }
        if (strength > 1.0f) strength = 1.0f;
        if (strength < params_.burstThreshold) strength = params_.burstThreshold;

        // Burst size = crossDim * (base + gain * strength) — all tunable.
        uint8_t burstAdd = (uint8_t)(crossDim_ *
                                    (params_.burstSizeBase + params_.burstSizeGain * strength));
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

