#include "HeatFire.h"
#include <Arduino.h>

HeatFire::HeatFire()
    : params_(), audio_(), noiseTime_(0.0f), prevPhase_(1.0f), beatCount_(0),
      downbeatSpreadMult_(1.0f),
      downbeatCoolSuppress_(0.0f), beat3Suppress_(0.0f), beat24Suppress_(0.0f),
      smoothedEnergy_(0.0f), pulseFlare_(0.0f),
      paletteBias_(0.0f) {}

bool HeatFire::begin(const DeviceConfig& config) {
    width_ = config.matrix.width;
    height_ = config.matrix.height;
    numLeds_ = width_ * height_;
    layout_ = config.matrix.layoutType;
    orientation_ = config.matrix.orientation;
    computeDimensionScales();
    lastUpdateMs_ = millis();
    return true;
}

void HeatFire::generate(PixelMatrix& matrix, const AudioControl& audio) {
    audio_ = audio;

    uint32_t currentMs = millis();
    float dt = (currentMs - lastUpdateMs_) / 1000.0f;
    lastUpdateMs_ = currentMs;
    dt = min(dt, 0.05f);

    // Smooth palette bias toward target (energy × rhythmStrength)
    // Fast ~0.15s time constant so hot yellows appear/fade visibly on each beat
    float targetBias = audio.energy * audio.rhythmStrength;
    paletteBias_ += (targetBias - paletteBias_) * min(1.0f, 6.0f * dt);

    // Decay all transient effect state
    downbeatSpreadMult_  = max(1.0f, downbeatSpreadMult_  - 3.00f * dt);  // 1.5 range / 0.5s
    downbeatCoolSuppress_= max(0.0f, downbeatCoolSuppress_- 2.00f * dt);  // 1.0 range / 0.5s
    beat3Suppress_       = max(0.0f, beat3Suppress_        - 1.25f * dt); // 0.5 range / 0.4s
    beat24Suppress_      = max(0.0f, beat24Suppress_       - 0.83f * dt); // 0.25 range / 0.3s

    // Pulse flare: transient → flame shoots upward briefly
    // burstStrength is reused as the flare height fraction (0–1 of display height)
    if (audio.pulse > params_.organicTransientMin) {
        float burst = (audio.pulse - params_.organicTransientMin) /
                      (1.0f - params_.organicTransientMin);
        float flare = params_.burstStrength * burst;
        if (flare > pulseFlare_) pulseFlare_ = flare;
    }
    pulseFlare_ = max(0.0f, pulseFlare_ - 4.0f * dt);  // τ ≈ 0.25s

    // Beat detection: accent patterns drive warp expansion + height flares
    if (beatHappened() && audio.rhythmStrength > 0.3f) {
        beatCount_++;

        // Downbeat (beat 1): maximum warp spread + biggest height flare
        if (audio.downbeat > 0.5f) {
            downbeatSpreadMult_   = 2.5f;
            downbeatCoolSuppress_ = 1.0f;
            pulseFlare_ = max(pulseFlare_, 0.30f);
        }

        // Beat 3: secondary accent
        if (audio.beatInMeasure == 3 && audio.rhythmStrength > 0.5f) {
            beat3Suppress_ = 0.5f;
            pulseFlare_ = max(pulseFlare_, 0.18f);
        }

        // Beats 2 & 4: lighter accent
        if ((audio.beatInMeasure == 2 || audio.beatInMeasure == 4) &&
                audio.rhythmStrength > 0.5f) {
            beat24Suppress_ = 0.25f;
            pulseFlare_ = max(pulseFlare_, 0.10f);
        }
    }
    prevPhase_ = audio.phase;

    // Advance noise scroll time — this is the visual "fire speed"
    // Audio energy is the primary driver: quiet = slow lazy drift, loud = fast roaring.
    // Pulse/onset creates sharp speed bursts for responsive hit feel.
    // Music mode adds phase-synced pulsing on top.
    float phasePulse = audio.phaseToPulse();
    float densityNorm = min(1.0f, audio.onsetDensity / 6.0f);

    // Energy scales speed 0.3x-2x: silence = slow embers, loud = lively roar
    float energyMult = 0.3f + 1.7f * smoothedEnergy_;
    // Pulse burst: sharp 1.5x speed kick on transient hits, decays naturally
    float pulseMult = 1.0f + 0.5f * audio.pulse;
    // Music phase modulation: scroll accelerates on-beat for rhythmic fire
    float phaseMod = 1.0f + params_.musicBeatDepth * phasePulse * audio.rhythmStrength;
    // Density boost: busier music = faster overall flicker
    float densityMult = 1.0f + params_.densityScrollBoost * densityNorm;

    float scrollRate = params_.noiseSpeed * energyMult * pulseMult * phaseMod * densityMult;
    noiseTime_ += scrollRate * dt;

    // Wrap to prevent float precision loss at large values.
    // Simplex noise has no exact period but wrapping at 1000 prevents the
    // gradual degradation that causes fire to fade over minutes.
    if (noiseTime_ > 1000.0f) noiseTime_ -= 1000.0f;

    renderNoiseFireField(matrix, audio, dt);
}

void HeatFire::reset() {
    noiseTime_            = 0.0f;
    prevPhase_            = 1.0f;
    beatCount_            = 0;
    downbeatSpreadMult_   = 1.0f;
    downbeatCoolSuppress_ = 0.0f;
    beat3Suppress_        = 0.0f;
    beat24Suppress_       = 0.0f;
    smoothedEnergy_       = 0.0f;
    pulseFlare_           = 0.0f;
    paletteBias_          = 0.0f;
    audio_                = AudioControl();
}

// ============================================================================
// Noise-field fire: scrolling noise with height-adaptive threshold
//
// Key design principle: the threshold *increases* toward the flame tip rather
// than the noise value being multiplied by a mask. This means only the
// brightest noise peaks can exist near the top — naturally forming tapered
// tongue tips rather than hitting a hard ceiling.
// ============================================================================

void HeatFire::renderNoiseFireField(PixelMatrix& matrix, const AudioControl& audio, float dt) {
    float phasePulse  = audio.phaseToPulse();
    float densityNorm = min(1.0f, audio.onsetDensity / 6.0f);

    // Smooth energy: asymmetric thermal inertia.
    // Fast rise (~0.1s): fire responds immediately to hits.
    // Moderate fall (~0.25s): flame clearly drops between beats at 120+ BPM.
    float riseRate = 10.0f * dt;
    float fallRate =  4.0f * dt;
    if (audio.energy > smoothedEnergy_) {
        smoothedEnergy_ += (audio.energy - smoothedEnergy_) * min(1.0f, riseRate);
    } else {
        smoothedEnergy_ += (audio.energy - smoothedEnergy_) * min(1.0f, fallRate);
    }

    // Threshold controls fire density. Lower threshold = more lit pixels.
    // Energy drives large threshold swings: silence = sparse embers, loud = solid fire.
    float threshold = params_.silenceThreshold
                    - params_.energyThresholdDrop * smoothedEnergy_;

    // Beat-accent threshold dips: brief density bursts on hits
    if (downbeatCoolSuppress_ > 0.0f) threshold -= 0.10f * downbeatCoolSuppress_;
    if (beat3Suppress_ > 0.0f)        threshold -= 0.06f * beat3Suppress_;
    if (beat24Suppress_ > 0.0f)       threshold -= 0.03f * beat24Suppress_;

    // Pulse (transient hit) drives immediate threshold drop for big visual burst
    threshold -= 0.15f * audio.pulse;

    threshold = constrain(threshold, 0.10f, 0.65f);

    // Noise spatial scales
    float xScale = 0.12f + 0.04f * densityNorm;  // Tongue width (~4 tongues across 32px)
    float yScale = 0.15f;                          // Vertical feature density

    // Domain warp: slow sway (0.2× scroll rate keeps it smooth, not jerky)
    float warpAmount = params_.warpStrength * (0.75f + audio.energy * downbeatSpreadMult_);

    if (layout_ == LINEAR_LAYOUT) {
        // 1D strip: simple threshold + contrast
        float gamma = 1.1f - 0.2f * paletteBias_;
        for (int x = 0; x < width_; x++) {
            float nx = x * xScale;
            float val = SimplexNoise::noise3D_01(nx, noiseTime_, 0.0f) * 0.7f
                      + SimplexNoise::noise3D_01(nx * 2.5f, noiseTime_ * 1.5f, 3.0f) * 0.3f;
            if (val > threshold) {
                float intensity = (val - threshold) / (1.0f - threshold);
                intensity *= intensity;
                uint32_t color = intensityToFireColor(intensity, gamma);
                matrix.setPixel(x, 0, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
            }
        }
    } else {
        // Flame height: hybrid audio-reactive + music-synced
        //
        // Design principle: raw audio (energy) is the source of truth and ALWAYS
        // drives flame height. Music mode (phase, rhythm) ENHANCES and synchronizes
        // but never suppresses the raw audio response.
        //
        //   1. Energy (primary): mic level directly drives flame height (full range)
        //   2. Phase breath (enhancement): when music detected, sync flame to beat
        //   3. Pulse flare (enhancement): sharp burst on transients

        // Energy drives full range: silence → baseHeight, loud → 1.0
        float energyHeight = params_.flameBaseHeight
                           + (1.0f - params_.flameBaseHeight) * smoothedEnergy_;

        // Music enhancement: phase breathing adds on top, scaled by rhythm confidence.
        // At rhythmStrength=0 (no music), this is zero — pure energy-reactive.
        // At rhythmStrength=0.85, adds up to beatPulseDepth (0.4) of oscillation.
        float phaseBreath = params_.beatPulseDepth * phasePulse * audio.rhythmStrength;

        float flameHeight = energyHeight + phaseBreath + pulseFlare_;
        flameHeight = constrain(flameHeight, 0.0f, 1.0f);

        // Beat brightness: slight enhancement on-beat, never dims below 1.0.
        // At rhythmStrength=0: brightMult = 1.0 (no modulation).
        // At rhythmStrength=0.85, on-beat: 1.0 + 0.7*0.85 ≈ 1.6 (brighter).
        // At rhythmStrength=0.85, off-beat: 1.0 + 0.0 = 1.0 (no dimming).
        float brightMult = 1.0f + 0.7f * audio.rhythmStrength * phasePulse;

        // Render zone extends 0.20 above the nominal flame height so tongues can
        // flicker upward past the "expected" boundary. The adaptive threshold prevents
        // widespread fire in this extension — only bright peaks poke through.
        float zoneHeight = min(1.0f, flameHeight + 0.20f);
        float zoneTop    = 1.0f - zoneHeight;  // normalizedY where zone begins

        // Hoist gamma out of per-pixel intensityToFireColor — paletteBias_ is constant per frame
        float gamma = 1.1f - 0.2f * paletteBias_;

        for (int y = 0; y < height_; y++) {
            float normalizedY = (height_ > 1) ? (float)y / (height_ - 1) : 0.0f;  // 0=top, 1=bottom

            // heightProgress: 0.0 at zone top (flame tip region), 1.0 at bottom (base)
            float heightProgress = constrain(
                (normalizedY - zoneTop) / max(zoneHeight, 0.001f), 0.0f, 1.0f);

            // Height-adaptive threshold: gentle rise toward tip.
            // At base (heightProgress=1): localThreshold = threshold (full density).
            // At tip (heightProgress=0): localThreshold rises, creating natural taper.
            // Using cubic for a gentler curve — fire stays dense through most of the
            // flame zone, only tapering in the top ~25%.
            float tipPos = 1.0f - heightProgress;  // 0 at base, 1 at tip
            float tipFade = tipPos * tipPos * tipPos;  // cubic: gentle taper
            float localThreshold = threshold + (1.0f - threshold) * 0.6f * tipFade;

            for (int x = 0; x < width_; x++) {
                // Domain warp: gentle horizontal sway, slow time evolution
                float warp = SimplexNoise::noise3D(
                    x * 0.1f, y * 0.15f, noiseTime_ * 0.2f) * warpAmount;

                float nx = (x + warp) * xScale;
                float ny = (y * yScale) + noiseTime_;  // +noiseTime_ scrolls upward

                // Two octaves: large structure (65%) + fine flicker detail (35%)
                float val = SimplexNoise::noise3D_01(nx,        ny,        1.0f) * 0.65f
                          + SimplexNoise::noise3D_01(nx * 2.5f, ny * 2.0f, 4.0f) * 0.35f;

                if (val > localThreshold) {
                    float intensity = (val - localThreshold) / (1.0f - localThreshold);
                    // Dim toward tip: gradual brightness gradient from base to tongue tips
                    intensity *= sqrtf(heightProgress);
                    // Quadratic boost: dim pixels collapse to black, bright pixels pop.
                    // Creates hard separation between tongue cores and background.
                    intensity = min(1.0f, intensity * intensity * 3.0f);
                    if (intensity > 0.01f) {
                        uint32_t color = intensityToFireColor(intensity, gamma);
                        // Beat breathing: dims to ~50% between beats, ~160% on beat.
                        // Applied to final color so boost isn't lost to the 1.0 intensity clamp.
                        // Organic mode (rhythmStrength=0): brightMult stays at 1.0.
                        uint8_t r = min(255, (int)(((color >> 16) & 0xFF) * brightMult));
                        uint8_t g = min(255, (int)(((color >> 8)  & 0xFF) * brightMult));
                        uint8_t b = min(255, (int)( (color        & 0xFF) * brightMult));
                        matrix.setPixel(x, y, r, g, b);
                    }
                }
                // Below localThreshold = black (matrix cleared before generate())
            }
        }
    }
}

// ============================================================================
// Color: dual-palette warm/hot blend with audio-driven gamma
// Mirrors particle Fire's particleColor() for visual consistency.
// ============================================================================

uint32_t HeatFire::intensityToFireColor(float intensity, float gamma) const {
    float normalized = powf(intensity, gamma);
    uint8_t remapped = (uint8_t)(normalized * 255.0f);

    // Two palettes blended by paletteBias_:
    //   low bias  (quiet, arrhythmic): warm campfire
    //   high bias (loud, rhythmic):    hot white-yellow
    // Hot palette stays in warm hues — no blue/white wash-out under direct blending.
    struct ColorStop { uint8_t position, r, g, b; };

    // Warm palette: black → deep red → red → orange → yellow-orange → bright yellow
    static const ColorStop warm[] = {
        {  0,   0,   0,   0},
        { 51,  64,   0,   0},
        {102, 255,   0,   0},
        {153, 255, 128,   0},
        {204, 255, 200,   0},
        {255, 255, 255,  64}
    };

    // Hot palette: black → red → bright orange → intense yellow → hot yellow-white
    static const ColorStop hot[] = {
        {  0,   0,   0,   0},
        { 51, 128,   8,   0},
        {102, 255,  80,   0},
        {153, 255, 180,  10},
        {204, 255, 230,  40},
        {255, 255, 255, 100}
    };

    const int paletteSize = 6;

    auto lookup = [&](const ColorStop* pal, uint8_t val,
                      uint8_t& ro, uint8_t& go, uint8_t& bo) {
        int lo = 0, hi = 1;
        for (int i = 0; i < paletteSize - 1; i++) {
            if (val >= pal[i].position && val <= pal[i + 1].position) {
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

    // Hot palette blends in above paletteBias_ = 0.2 so yellows appear at moderate energy
    float blend = constrain((paletteBias_ - 0.2f) / 0.6f, 0.0f, 1.0f);
    uint8_t r = (uint8_t)(wr + blend * (hr - wr));
    uint8_t g = (uint8_t)(wg + blend * (hg - wg));
    uint8_t b = (uint8_t)(wb + blend * (hb - wb));

    // Master brightness
    float br = params_.brightness;
    r = (uint8_t)(r * br);
    g = (uint8_t)(g * br);
    b = (uint8_t)(b * br);

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
