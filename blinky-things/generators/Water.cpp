#include "Water.h"
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

Water::Water() : params_(), noiseTime_(0.0f), background_(nullptr) {}

bool Water::begin(const DeviceConfig& config) {
    if (!ParticleGenerator::begin(config)) return false;

    noiseTime_ = 0.0f;

    return true;
}

void Water::initPhysicsContext() {
    // Set physics parameters from WaterParams (dimension-scaled)
    gravity_ = params_.gravity * traversalDim_;
    drag_ = params_.drag;

    // Create layout-appropriate physics components
    bool wrap = PhysicsContext::shouldWrapByDefault(layout_);

    // Spawn region: top edge for matrix, random for linear
    spawnRegion_ = PhysicsContext::createSpawnRegion(
        layout_, GeneratorType::WATER, width_, height_, spawnBuffer_);

    // Boundary: kill for matrix (splash handled separately), wrap for linear
    boundary_ = PhysicsContext::createBoundary(
        layout_, GeneratorType::WATER, wrap, boundaryBuffer_);

    // Force adapter: 2D for matrix, 1D for linear
    forceAdapter_ = PhysicsContext::createForceAdapter(layout_, forceBuffer_);
    if (forceAdapter_) {
        forceAdapter_->setWind(params_.windBase, scaledWindVar());
    }

    // Background model: water surface with height variation for matrix, uniform for linear
    background_ = PhysicsContext::createBackground(
        layout_, BackgroundStyle::WATER, backgroundBuffer_);
}

void Water::generate(PixelMatrix& matrix, const AudioControl& audio) {
    // Frame time. Everything below is scaled to it: the old per-frame
    // constants (tuned at ~60 fps) made the water run faster/slower and
    // decay at different rates whenever loop speed changed.
    uint32_t nowMs = millis();
    float dt = (lastFrameMs_ == 0) ? (1.0f / 60.0f) : (nowMs - lastFrameMs_) / 1000.0f;
    lastFrameMs_ = nowMs;
    if (dt > 0.1f) dt = 0.1f;
    const float frames60 = dt * 60.0f;  // "how many 60 fps frames elapsed"

    // Beat state for this frame (shared with spawnParticles via the base).
    observeBeat(audio);

    // Advance noise animation time (blend between organic and music-driven)
    float organicSpeed = 0.012f + 0.008f * audio.energy;
    float musicSpeed = 0.03f + 0.02f * audio.energy;
    float timeSpeed = organicSpeed * (1.0f - audio.rhythmStrength) + musicSpeed * audio.rhythmStrength;

    // Phase-driven wave speed breathing: water flows faster on-beat
    float phaseBreathing = 0.5f + 0.5f * audio.phaseToPulse();
    float blendedBreathing = 1.0f * (1.0f - audio.rhythmStrength) + phaseBreathing * audio.rhythmStrength;
    noiseTime_ += timeSpeed * blendedBreathing * frames60;

    // Render noise background first (tropical sea underlayer)
    if (background_) {
        background_->setIntensity(params_.backgroundIntensity);
        background_->render(matrix, width_, height_, noiseTime_, audio);
    }

    // --- Depth gradient (matrix only): darken lower rows for depth perception ---
    if (PhysicsContext::isPrimaryAxisVertical(layout_) && height_ > 2) {
        for (int y = 0; y < height_; y++) {
            // Surface (y=0) is brightest, bottom is darkest
            float depthFade = 1.0f - 0.6f * ((float)y / (float)(height_ - 1));
            for (int x = 0; x < width_; x++) {
                RGB px = matrix.getPixel(x, y);
                matrix.setPixel(x, y,
                    (uint8_t)(px.r * depthFade),
                    (uint8_t)(px.g * depthFade),
                    (uint8_t)(px.b * depthFade));
            }
        }
    }

    // --- Ripple simulation (2-buffer demoscene algorithm) ---
    int rippleSize = min((int)numLeds_, (int)MAX_RIPPLE_LEDS);
    if (rippleSize > 2) {
        // Propagate ripples at a FIXED 60 Hz step (the 2-buffer wave
        // equation is inherently per-step): new[i] = avg(neighbors) -
        // current[i], ~3% damping per step. Substepping keeps wave speed
        // and damping independent of loop rate.
        rippleAccum_ += dt;
        int steps = 0;
        while (rippleAccum_ >= RIPPLE_STEP_S && steps < 6) {
            rippleAccum_ -= RIPPLE_STEP_S;
            steps++;
            int16_t* c = rippleFlip_ ? rippleA_ : rippleB_;
            int16_t* p = rippleFlip_ ? rippleB_ : rippleA_;
            for (int i = 1; i < rippleSize - 1; i++) {
                int16_t avg = (p[i - 1] + p[i + 1]) >> 1;
                c[i] = avg - c[i];
                c[i] -= c[i] >> 5;
            }
            rippleFlip_ = !rippleFlip_;
        }
        if (rippleAccum_ > RIPPLE_STEP_S) rippleAccum_ = 0.0f;  // long stall: drop backlog
        // After the last step, the most recently written buffer is the
        // one the next step reads as `prev`.
        int16_t* curr = rippleFlip_ ? rippleB_ : rippleA_;

        // Inject one ripple per detected drum hit (edge-triggered). The
        // old `pulse > 0.3` injected on every frame of the 240 ms envelope —
        // 10-20 splashes per kick, more at higher fps — smearing onsets.
        if (audio.onset > 0.0f) {
            float s = (audio.onset > audio.pulse) ? audio.onset : audio.pulse;
            int pos = random(1, rippleSize - 1);
            curr[pos] = (int16_t)(s * 500);
        }
        // Stronger impulse on beat
        if (beatHappened() && audio.rhythmStrength > 0.3f) {
            // Edge sweep: inject along one edge
            for (int i = 0; i < min(3, rippleSize); i++) {
                curr[i] = (int16_t)(audio.energy * 800);
            }
        }

        // Composite ripples as additive blue-cyan brightness modulation
        for (int i = 0; i < rippleSize; i++) {
            int16_t r = curr[i];
            if (r > 0) {
                uint8_t brightness = (uint8_t)min(80, (int)(r >> 2));
                int x = i % width_;
                int y = i / width_;
                if (x < width_ && y < height_) {
                    RGB px = matrix.getPixel(x, y);
                    matrix.setPixel(x, y,
                        px.r,
                        min(255, px.g + brightness),
                        min(255, px.b + (brightness >> 1)));
                }
            }
        }
    }

    // --- Bioluminescence: blue-green glow on pulse events ---
    {
        int glowSize = min((int)numLeds_, (int)MAX_RIPPLE_LEDS);

        // Decay existing glow: 5% per 60 fps frame, time-scaled
        const float glowDecay = powf(0.95f, frames60);
        for (int i = 0; i < glowSize; i++) {
            if (glowBuffer_[i] > 0) {
                glowBuffer_[i] = (uint8_t)(glowBuffer_[i] * glowDecay);
                if (glowBuffer_[i] < 2) glowBuffer_[i] = 0;
            }
        }

        // Spawn glow once per drum hit (edge-triggered, see ripples)
        if (audio.onset > 0.0f) {
            float s = (audio.onset > audio.pulse) ? audio.onset : audio.pulse;
            int pos = random(glowSize);
            // Clamp before the cast: float->uint8 out of range is undefined
            // behaviour (inputs are 0..1 from AudioTracker, but scripted /
            // amplified AudioControl can exceed that).
            float gi = s * audio.energy * 200.0f;
            uint8_t intensity = (uint8_t)(gi < 0.0f ? 0.0f : (gi > 255.0f ? 255.0f : gi));
            glowBuffer_[pos] = max(glowBuffer_[pos], intensity);
            // Spread to neighbors
            if (pos > 0) glowBuffer_[pos - 1] = max(glowBuffer_[pos - 1], (uint8_t)(intensity >> 1));
            if (pos < glowSize - 1) glowBuffer_[pos + 1] = max(glowBuffer_[pos + 1], (uint8_t)(intensity >> 1));
        }

        // Render glow as additive blue-green
        for (int i = 0; i < glowSize; i++) {
            if (glowBuffer_[i] > 5) {
                int x = i % width_;
                int y = i / width_;
                if (x < width_ && y < height_) {
                    uint8_t g = glowBuffer_[i];
                    uint8_t b = (uint8_t)(g * 0.6f);
                    RGB px = matrix.getPixel(x, y);
                    matrix.setPixel(x, y,
                        px.r,
                        min(255, px.g + g),
                        min(255, px.b + b));
                }
            }
        }
    }

    // Run particle system (spawns, updates, renders particles)
    ParticleGenerator::generate(matrix, audio);
}

void Water::reset() {
    ParticleGenerator::reset();
    noiseTime_ = 0.0f;
    rippleAccum_ = 0.0f;
    lastFrameMs_ = 0;
    memset(rippleA_, 0, sizeof(rippleA_));
    memset(rippleB_, 0, sizeof(rippleB_));
    memset(glowBuffer_, 0, sizeof(glowBuffer_));
    rippleFlip_ = false;
}

void Water::spawnParticles(float dt) {
    float spawnProb;
    uint8_t dropCount = 0;

    // MUSIC-DRIVEN behavior (rhythmStrength weighted)
    float phasePulse = audio_.phaseToPulse();
    float phaseWave = 0.4f + 0.6f * phasePulse;
    float musicSpawnProb = params_.baseSpawnChance * phaseWave + params_.audioSpawnBoost * audio_.pulse * phasePulse;

    // Wave burst on beat (scales with rhythmStrength, device width, and onset density)
    // Onset density scales burst: dance (4-6/s) = full wave, ambient (0-1/s) = gentle drip
    if (beatHappened() && audio_.rhythmStrength > 0.3f) {
        float densityScale = 0.5f + 0.5f * min(audio_.onsetDensity / 6.0f, 1.0f);
        uint8_t waveDrops = (uint8_t)min(8.0f, max(2.0f, 0.5f * crossDim_ * (1.0f + audio_.rhythmStrength)));
        dropCount += (uint8_t)(waveDrops * (0.5f + 0.5f * audio_.energy) * audio_.rhythmStrength * densityScale);
    }

    // ORGANIC-DRIVEN behavior (inverse rhythmStrength weighted)
    float smoothEnergy = 0.4f + 0.3f * audio_.energy;
    float organicSpawnProb = params_.baseSpawnChance * smoothEnergy;

    // Gentle transient response (only in organic mode to avoid double-triggering with beats)
    if (audio_.pulse > params_.organicTransientMin && audio_.rhythmStrength < 0.5f) {
        float transientStrength = (audio_.pulse - params_.organicTransientMin) /
                                 (1.0f - params_.organicTransientMin);
        dropCount += (uint8_t)(2 * transientStrength);  // 0-2 drops based on transient strength
    }

    // BLEND spawn probability between modes
    spawnProb = organicSpawnProb * (1.0f - audio_.rhythmStrength) +
                musicSpawnProb * audio_.rhythmStrength;

    // Random baseline spawning. spawnProb is per 60 fps frame; convert to
    // this frame's duration so the drop rate doesn't scale with loop speed.
    float p = spawnProb;
    if (p > 0.0f && p < 1.0f) p = 1.0f - powf(1.0f - p, dt * 60.0f);
    if (random(1000) < p * 1000) {
        dropCount++;
    }

    // Spawn drops using layout-aware spawn region
    uint16_t maxParts = scaledMaxParticles();
    for (uint8_t i = 0; i < dropCount && pool_.getActiveCount() < maxParts; i++) {
        float x, y;
        getSpawnPosition(x, y);

        // Get initial velocity from spawn region (dimension-scaled)
        float speed = scaledDropVelMin() +
                     random(100) * (scaledDropVelMax() - scaledDropVelMin()) / 100.0f;

        float vx, vy;
        getInitialVelocity(speed, vx, vy);

        // Add spread perpendicular to main direction (dimension-scaled)
        float spreadAmount = (random(200) - 100) * scaledDropSpread() / 100.0f;
        if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
            vx += spreadAmount;
        } else {
            vy += spreadAmount * 0.3f;
        }

        // Blend velocity multiplier between organic (0.7x) and music (1.0-1.2x)
        float organicVelMult = 0.7f;
        float musicVelMult = 1.0f + 0.2f * audio_.pulse;
        float velocityMult = organicVelMult * (1.0f - audio_.rhythmStrength) +
                            musicVelMult * audio_.rhythmStrength;
        vx *= velocityMult;
        vy *= velocityMult;

        uint8_t intensity = random(params_.intensityMin, params_.intensityMax + 1);

        pool_.spawn(x, y, vx, vy, intensity, params_.defaultLifespan, 1.0f,
                   ParticleFlags::GRAVITY | ParticleFlags::WIND |
                   ParticleFlags::FADE | ParticleFlags::SPLASH);
    }
}

void Water::updateParticle(Particle* p, float dt) {
    // Check for splash on bottom collision (matrix) or wrap point (linear)
    if (p->hasFlag(ParticleFlags::SPLASH)) {
        bool shouldSplash = false;

        if (PhysicsContext::isPrimaryAxisVertical(layout_)) {
            // Matrix: splash at bottom
            shouldSplash = p->y >= height_ - 1;
        } else {
            // Linear: splash at edges (before wrap)
            shouldSplash = p->x < 0.5f || p->x >= width_ - 0.5f;
        }

        if (shouldSplash) {
            spawnSplash(p->x, p->y, p->intensity);
            pool_.kill(p);
        }
    }
}

void Water::renderParticle(const Particle* p, PixelMatrix& matrix) {
    int x = (int)p->x;
    int y = (int)p->y;

    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        uint32_t color = particleColor(p->intensity);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        // MAX BLENDING: Drops take brightest value to pop against dark background
        RGB existing = matrix.getPixel(x, y);
        matrix.setPixel(x, y,
                       max(existing.r, r),
                       max(existing.g, g),
                       max(existing.b, b));
    }
}

uint32_t Water::particleColor(uint8_t intensity) const {
    // White/light cyan drops for visibility against blue background
    uint8_t white = intensity;
    uint8_t blue = (uint8_t)min(255, (int)intensity + 40);
    uint8_t green = (uint8_t)((uint16_t)intensity * 3 / 4);
    return ((uint32_t)white << 16) | ((uint32_t)green << 8) | blue;
}

void Water::spawnSplash(float x, float y, uint8_t parentIntensity) {
    // Calculate available slots (dimension-scaled)
    uint16_t maxParts = scaledMaxParticles();
    uint8_t available = maxParts > pool_.getActiveCount()
                        ? maxParts - pool_.getActiveCount()
                        : 0;
    uint8_t splashCount = min(scaledSplashParticles(), available);

    // Guard against division by zero in angle calculation
    if (splashCount == 0) return;

    for (uint8_t i = 0; i < splashCount; i++) {
        // Radial splash pattern
        float angle = (i * TWO_PI / splashCount) + random(100) * 0.01f;
        float speed = scaledSplashVelMin() +
                     random(100) * (scaledSplashVelMax() - scaledSplashVelMin()) / 100.0f;

        float vx = cos(angle) * speed;
        float vy = sin(angle) * speed - 1.0f;  // Slight upward component

        uint8_t intensity = (uint8_t)min((uint32_t)255, (uint32_t)parentIntensity * params_.splashIntensity / 255);

        pool_.spawn(x, y, vx, vy, intensity, 30, 0.5f,
                   ParticleFlags::GRAVITY | ParticleFlags::FADE);
    }
}
