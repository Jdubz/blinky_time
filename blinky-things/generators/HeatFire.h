#pragma once

#include "Generator.h"
#include "../math/SimplexNoise.h"
#include "../audio/AudioControl.h"

/**
 * HeatFireParams - Heat buffer fire parameters
 *
 * All params are device-independent. The heat buffer auto-sizes to
 * width_ x height_ from DeviceConfig.
 */
struct HeatFireParams {
    // Heat injection (bottom row)
    float baseHeat;              // Base heat injection level (0-1)
    float audioHeatBoost;        // Energy-driven heat boost multiplier (0-2)
    float beatHeatPulse;         // Phase modulation depth for injection (0-1)

    // Cooling
    float baseCooling;           // Base cooling per row per frame (0-0.5, maps to 0-128 uint8)
    float coolingVariation;      // Noise-driven spatial cooling variation (0-1)

    // Diffusion
    float diffusionSpread;       // Horizontal drift range for heat propagation (0-3 cells)

    // Transient response
    float burstHeat;             // Extra heat injected on pulse (0-1)
    float organicTransientMin;   // Min pulse to trigger burst (0-1)

    // Audio reactivity
    float musicBeatDepth;        // Beat sync depth for injection (0-1)

    // Wind
    float windDrift;             // Audio-reactive horizontal drift bias (0-3 cells)

    // Noise animation
    float noiseSpeed;            // Base noise evolution speed (0.001-0.1)

    // Output brightness
    float brightness;            // Master output brightness scale (0-1)

    HeatFireParams() {
        baseHeat = 0.2f;          // LOW base — audio must drive the fire, not idle heat
        audioHeatBoost = 3.0f;    // HIGH — energy makes a dramatic difference
        beatHeatPulse = 0.95f;    // Deep phase breathing (near-silent off-beat)
        baseCooling = 0.25f;      // Controls flame height: coolingMax = baseCooling*600/height + 1. Higher = shorter.
        coolingVariation = 0.4f;  // Reserved
        diffusionSpread = 1.5f;   // Moderate sway
        burstHeat = 1.0f;         // Full burst on transients — visible flame surge
        organicTransientMin = 0.25f;
        musicBeatDepth = 0.95f;   // Deep beat sync (matches particle Fire's musicSpawnPulse)
        windDrift = 1.2f;         // Audio-reactive lateral movement
        noiseSpeed = 0.08f;       // Fast noise — tongues flicker like fire, not lava
        brightness = 0.4f;        // 40% — solid wall of pixels needs less than sparse particles
    }
};

/**
 * HeatFire - DOOM-style heat buffer fire generator
 *
 * Uses per-pixel heat diffusion instead of particles. Each frame:
 * 1. Heat propagates upward: each cell averages from row below with drift and cooling
 * 2. Audio-driven heat injected at bottom row
 * 3. Heat values mapped through fire color palette
 *
 * Produces continuous flame tongues that split, merge, and narrow as they rise.
 * Layout-aware: 2D propagation for matrix, 1D for linear strings.
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
    // Noise-field fire: scrolling thresholded noise (primary algorithm)
    void renderNoiseFireField(PixelMatrix& matrix, const AudioControl& audio);

    // Map 0-1 intensity through fire color palette
    uint32_t intensityToFireColor(float intensity) const;

    // Legacy heat buffer (kept for reference)
    void propagateHeat2D();
    void propagateHeat1D();
    void injectHeat(const AudioControl& audio);
    void updateFlares(const AudioControl& audio);
    void applyGlow(int x, int y, int z);
    static uint32_t isqrt(uint32_t n);
    void renderHeat(PixelMatrix& matrix);
    uint32_t heatToColor(uint8_t heat) const;

    // Beat detection
    bool beatHappened() const {
        return audio_.phase < 0.2f && prevPhase_ > 0.8f;
    }

    // Heat buffer — values are 0 to NCOLORS-1 (not 0-255)
    static constexpr uint16_t MAX_HEAT_CELLS = 2048;
    static constexpr uint8_t NCOLORS = 11;       // Number of colors in fire palette
    static constexpr uint8_t FLARE_DECAY = 20;   // Vertical decay rate (horizontal is 3× faster)
    static constexpr uint8_t MAX_FLARES = 32;    // Max simultaneous flares
    uint8_t heat_[MAX_HEAT_CELLS];

    // Flare state: packed as (z<<16 | y<<8 | x)
    uint32_t flares_[MAX_FLARES];
    uint8_t nflare_ = 0;

    HeatFireParams params_;
    AudioControl audio_;

    // Animation state
    float noiseTime_;
    float prevPhase_;
    uint8_t beatCount_;

    // Downbeat transient state (same decay patterns as particle Fire)
    float downbeatSpreadMult_;   // Spread widening: 2.5 → 1.0 over 0.5s
    float downbeatColorShift_;   // White/blue tint: 1.0 → 0.0 over 0.5s
    float downbeatCoolSuppress_; // Cooling suppression: 1.0 → 0.0 over 0.5s
    float spawnBias_;            // Left/right injection bias per beat-in-measure

    // Smoothed audio state (fire has thermal inertia — no instant jumps)
    float smoothedEnergy_;       // Low-pass filtered energy (~0.5s time constant)
    float smoothedThreshold_;    // Low-pass filtered threshold (~0.3s time constant)

    // Palette state
    float paletteBias_;          // Smoothed energy*rhythm for warm/hot blend

    // Per-frame computed values (avoid recomputing in inner loops)
    float effectiveCooling_;
    float effectiveSpread_;
};
