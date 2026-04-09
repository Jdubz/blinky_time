#include "PlasmaGlobe.h"
#include <Arduino.h>

PlasmaGlobe::PlasmaGlobe()
    : params_(), noiseTime_(0.0f), pulseEnvelope_(0.0f),
      pulseRadiusEnv_(0.0f), numOrbs_(3) {
    memset(orbX_, 0, sizeof(orbX_));
    memset(orbY_, 0, sizeof(orbY_));
    memset(orbPhaseOffset_, 0, sizeof(orbPhaseOffset_));
}

bool PlasmaGlobe::begin(const DeviceConfig& config) {
    width_ = config.matrix.width;
    height_ = config.matrix.height;
    numLeds_ = width_ * height_;
    layout_ = config.matrix.layoutType;
    orientation_ = config.matrix.orientation;

    // Scale orb count to display size
    numOrbs_ = (numLeds_ > 100) ? 4 : (numLeds_ > 30) ? 3 : 2;

    // Initialize orbs at spread-out positions
    float cx = width_ * 0.5f;
    float cy = height_ * 0.5f;
    for (int i = 0; i < numOrbs_; i++) {
        float angle = (float)i / numOrbs_ * 2.0f * PI;
        orbX_[i] = cx + cosf(angle) * width_ * 0.25f;
        orbY_[i] = cy + sinf(angle) * height_ * 0.25f;
        orbPhaseOffset_[i] = (float)i * 1.7f; // Golden-ratio-ish spacing
    }

    noiseTime_ = 0.0f;
    pulseEnvelope_ = 0.0f;
    pulseRadiusEnv_ = 0.0f;
    lastUpdateMs_ = millis();
    return true;
}

void PlasmaGlobe::reset() {
    noiseTime_ = 0.0f;
    pulseEnvelope_ = 0.0f;
    pulseRadiusEnv_ = 0.0f;
    lastUpdateMs_ = millis();
}

void PlasmaGlobe::generate(PixelMatrix& matrix, const AudioControl& audio) {
    uint32_t now = millis();
    float dt = (now - lastUpdateMs_) * 0.001f;
    if (dt <= 0.0f || dt > 0.5f) dt = 0.016f;
    lastUpdateMs_ = now;

    // --- Time advancement (dt-based for frame-rate independence) ---
    float speedMod = 0.7f + 0.3f * audio.energy;
    noiseTime_ += params_.driftSpeed * speedMod * dt;

    // --- Beat pulse envelope (dt-based decay) ---
    if (audio.pulse > 0.4f && audio.pulse > pulseEnvelope_) {
        pulseEnvelope_ = min(1.0f, audio.pulse);
        pulseRadiusEnv_ = min(1.0f, audio.pulse);
    }
    // Scale decay to ~60fps reference: decay^(dt/0.016) keeps timing consistent
    float decayScale = dt * 62.5f;  // normalize to ~60 Hz frame rate
    float decay = powf(params_.pulseDecay, decayScale);
    pulseEnvelope_ *= decay;
    pulseRadiusEnv_ *= powf(params_.pulseDecay * 0.95f, decayScale);

    // --- Update orb positions via noise field ---
    float diagonal = sqrtf((float)(width_ * width_ + height_ * height_));
    for (int i = 0; i < numOrbs_; i++) {
        float phase = orbPhaseOffset_[i];
        // Noise-driven velocity (different noise seed per orb)
        float vx = SimplexNoise::noise3D_01(orbX_[i] * 0.15f, orbY_[i] * 0.15f,
                                             noiseTime_ * 3.0f + phase) - 0.5f;
        float vy = SimplexNoise::noise3D_01(orbX_[i] * 0.15f + 10.0f, orbY_[i] * 0.15f + 10.0f,
                                             noiseTime_ * 3.0f + phase) - 0.5f;

        // Gentle pull toward center (prevents orbs from drifting off-screen)
        float cx = width_ * 0.5f;
        float cy = height_ * 0.5f;
        float pullX = (cx - orbX_[i]) * 0.02f;
        float pullY = (cy - orbY_[i]) * 0.02f;

        orbX_[i] += (vx * width_ * 0.3f + pullX) * dt;
        orbY_[i] += (vy * height_ * 0.3f + pullY) * dt;

        // Soft clamp to bounds
        orbX_[i] = constrain(orbX_[i], 0.0f, (float)(width_ - 1));
        orbY_[i] = constrain(orbY_[i], 0.0f, (float)(height_ - 1));
    }

    // --- Phase-driven orb breathing ---
    float breathe = audio.rhythmStrength > 0.2f
        ? 0.6f + 0.4f * audio.phaseToPulse()
        : 0.8f + 0.2f * sinf(noiseTime_ * 5.0f);

    // --- Render ---
    float baseRadius = params_.orbRadius * diagonal;
    float pulseRadius = baseRadius * (1.0f + params_.pulseExpand * pulseRadiusEnv_);
    float invRadiusSq = 1.0f / (pulseRadius * pulseRadius + 0.001f);

    // Pre-compute per-orb breathing (avoid repeated sinf in inner loop)
    float orbBreathVal[PlasmaGlobe::MAX_ORBS];
    for (int i = 0; i < numOrbs_; i++) {
        orbBreathVal[i] = breathe * (0.8f + 0.2f * sinf(noiseTime_ * 4.0f + orbPhaseOffset_[i]))
                        * params_.orbBrightness;
    }

    // Pre-compute pulse flash params (avoid branching per pixel)
    float cx = width_ * 0.5f;
    float cy = height_ * 0.5f;
    float maxDistSq = cx * cx + cy * cy + 0.001f;
    float invMaxDistSq = 1.0f / maxDistSq;
    bool hasPulse = pulseEnvelope_ > 0.01f;
    float pulseScale = pulseEnvelope_ * params_.pulseBrightness;

    // Cutoff: quadratic kernel 1 - distSq * invRadiusSq * 1.5 hits zero here
    float cutoffDistSq = 1.0f / (invRadiusSq * 1.5f);

    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            float intensity = 0.0f;

            // Accumulate orb contributions — skip orbs beyond cutoff distance
            for (int i = 0; i < numOrbs_; i++) {
                float dx = (float)x - orbX_[i];
                float dy = (float)y - orbY_[i];
                float distSq = dx * dx + dy * dy;

                if (distSq > cutoffDistSq) continue;

                // Quadratic approximation of Gaussian: max(0, 1 - distSq*k)^2
                // Much cheaper than expf, visually similar for tight orbs
                float t = 1.0f - distSq * invRadiusSq * 1.5f;
                if (t <= 0.0f) continue;
                intensity += t * t * orbBreathVal[i];
            }

            // Add pulse flash (radial from center, brief)
            if (hasPulse) {
                float dx = (float)x - cx;
                float dy = (float)y - cy;
                float distSq = dx * dx + dy * dy;
                float radialFade = 1.0f - distSq * invMaxDistSq;
                if (radialFade > 0.0f) {
                    intensity += pulseScale * radialFade * radialFade;
                }
            }

            // Skip ambient noise for pixels already above threshold (saves noise3D call)
            if (intensity < 0.15f) {
                // Very dim ambient (near-invisible noise shimmer)
                float ambient = SimplexNoise::noise3D_01(
                    (float)x * 0.4f, (float)y * 0.4f, noiseTime_ * 2.0f) * params_.backgroundDim;
                intensity += ambient;
            }

            // Clamp and apply threshold — pixels below threshold stay OFF
            if (intensity < 0.15f) {
                matrix.setPixel(x, y, 0, 0, 0);
                continue;
            }
            intensity = min(1.0f, intensity);

            uint8_t r, g, b;
            purplePalette(intensity, r, g, b);
            matrix.setPixel(x, y, r, g, b);
        }
    }
}

void PlasmaGlobe::purplePalette(float t, uint8_t& r, uint8_t& g, uint8_t& b) const {
    // 3-stop purple/violet gradient (t >= 0.15 guaranteed by caller threshold)
    // 0.15 = deep violet, 0.5 = purple, 0.8 = lavender, 1.0 = white-violet
    if (t < 0.5f) {
        float s = (t - 0.15f) / 0.35f;  // 0.0 at t=0.15, 1.0 at t=0.5
        r = (uint8_t)(30 + s * 80);
        g = (uint8_t)(s * 15);
        b = (uint8_t)(60 + s * 100);
    } else if (t < 0.8f) {
        float s = (t - 0.5f) / 0.3f;
        r = (uint8_t)(110 + s * 60);
        g = (uint8_t)(15 + s * 70);
        b = (uint8_t)(160 + s * 50);
    } else {
        float s = (t - 0.8f) / 0.2f;
        r = (uint8_t)(170 + s * 85);
        g = (uint8_t)(85 + s * 155);
        b = (uint8_t)(210 + s * 45);
    }
}
