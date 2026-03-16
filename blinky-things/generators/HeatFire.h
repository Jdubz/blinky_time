#pragma once

#include "Generator.h"
#include "../math/SimplexNoise.h"
#include "../audio/AudioControl.h"

/**
 * HeatFireParams - Noise-field fire parameters
 *
 * All params control the scrolling thresholded simplex noise renderer.
 * No per-device calibration needed — behavior auto-adapts to display size.
 */
struct HeatFireParams {
    // Intensity threshold (controls fire size and audio reactivity)
    float silenceThreshold;      // Noise threshold at silence (0.3-0.8). Higher = less fire. Default: 0.58.
    float energyThresholdDrop;   // Max threshold reduction from energy (0.1-0.6). Default: 0.22.
    float beatPulseDepth;        // Phase breathing height amplitude (0-0.5). Default: 0.40.
    float burstStrength;         // Transient (pulse) height flare (0-0.5). Default: 0.25.
    float organicTransientMin;   // Minimum pulse value to trigger burst (0-1). Default: 0.25.

    // Flame shape
    float flameBaseHeight;       // Flame height fraction at silence (0.1-0.8). Default: 0.20.
    float warpStrength;          // Domain warp amplitude — controls lateral sway (0-1). Default: 0.40.

    // Animation
    float noiseSpeed;            // Base noise scroll speed (0.01-0.5). Default: 0.25.
    float musicBeatDepth;        // Beat sync depth for scroll speed (0-1). Default: 0.95.
    float densityScrollBoost;    // OnsetDensity → extra scroll speed (0-1.0). Default: 0.50.

    // Output
    float brightness;            // Master output brightness scale (0-1). Default: 0.38.

    HeatFireParams() {
        silenceThreshold    = 0.58f;  // Higher threshold = fewer lit pixels = more dark space
        energyThresholdDrop = 0.22f;  // Modest density variation from energy
        beatPulseDepth      = 0.40f;  // Per-beat height breathing amplitude (primary music sync)
        burstStrength       = 0.25f;  // Height flare on transient (fraction of display height)
        organicTransientMin = 0.25f;  // Responsive to softer transients
        flameBaseHeight     = 0.20f;  // 20% rest height — room for full phase-breathing range
        warpStrength        = 0.40f;  // Moderate lateral sway
        noiseSpeed          = 0.25f;  // Scroll speed — fast enough to look like fire
        musicBeatDepth      = 0.95f;  // Deep beat sync for scroll speed modulation
        densityScrollBoost  = 0.50f;  // 50% faster flicker at max onset density
        brightness          = 0.38f;  // Lower base — beat breathing creates the peaks
    }
};

/**
 * HeatFire - Noise-field fire generator
 *
 * Renders fire as a scrolling, thresholded simplex noise field with a vertical
 * gradient mask. Each frame:
 *   1. A 2D noise field scrolls upward (scroll speed = audio-modulated noiseSpeed)
 *   2. A vertical height mask fades fire out toward the top
 *   3. Pixels above the threshold are lit with the fire color palette
 *   4. Threshold drops on energy/beat/pulse → fire grows and surges
 *
 * Produces continuous flame tongues that split, merge, and sway organically.
 * All 7 AudioControl signals consumed:
 *   energy        → flame height + threshold range + scroll speed + palette blend
 *   phase         → scroll speed breathing + on-beat threshold dip
 *   pulse         → transient threshold surge
 *   downbeat      → max threshold drop + warp expansion + color tint
 *   rhythmStrength→ organic/music blend for scroll and breathing
 *   onsetDensity  → scroll speed + tongue width
 *   beatInMeasure → beat 1/3/2+4 accent threshold patterns + left/right bias
 *
 * Layout-aware: 2D gradient for matrix layouts, 1D horizontal for linear strings.
 */
class HeatFire : public Generator {
public:
    HeatFire();
    virtual ~HeatFire() override = default;

    // Generator interface
    bool begin(const DeviceConfig& config) override;
    void generate(PixelMatrix& matrix, const AudioControl& audio) override;
    void reset() override;
    const char* getName() const override { return "HeatFire"; }
    GeneratorType getType() const override { return GeneratorType::HEAT_FIRE; }

    // Parameter access
    void setParams(const HeatFireParams& params) { params_ = params; }
    const HeatFireParams& getParams() const { return params_; }
    HeatFireParams& getParamsMutable() { return params_; }

private:
    void renderNoiseFireField(PixelMatrix& matrix, const AudioControl& audio, float dt);

    // Map 0-1 intensity through dual fire palette (warm/hot blend by paletteBias_)
    // Mirrors particle Fire's particleColor() for visual consistency.
    uint32_t intensityToFireColor(float intensity, float gamma) const;

    // Beat detection via phase crossing (phase wraps 0→1 each beat)
    bool beatHappened() const {
        return audio_.phase < 0.2f && prevPhase_ > 0.8f;
    }

    HeatFireParams params_;
    AudioControl audio_;

    // Animation state
    float noiseTime_;
    float prevPhase_;
    uint8_t beatCount_;

    // Downbeat transient state (matching particle Fire decay patterns)
    float downbeatSpreadMult_;   // Warp widening: 2.5 → 1.0 over 0.5s
    float downbeatCoolSuppress_; // Threshold suppression: 1.0 → 0.0 over 0.5s
    float beat3Suppress_;        // Beat 3 threshold suppression: 0.5 → 0.0 over 0.4s
    float beat24Suppress_;       // Beats 2/4 threshold suppression: 0.25 → 0.0 over 0.3s

    // Smoothed audio state
    float smoothedEnergy_;       // Low-pass filtered energy (rises ~0.1s, falls ~0.25s)
    float pulseFlare_;           // Beat/transient height flare: briefly boosts flameHeight

    // Palette state (same approach as particle Fire)
    float paletteBias_;          // Smoothed energy*rhythmStrength for warm/hot palette blend
};
