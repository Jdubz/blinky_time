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

    HeatFireParams() {
        baseHeat = 0.7f;
        audioHeatBoost = 1.5f;
        beatHeatPulse = 0.8f;
        baseCooling = 0.08f;
        coolingVariation = 0.3f;
        diffusionSpread = 1.2f;
        burstHeat = 0.9f;
        organicTransientMin = 0.25f;
        musicBeatDepth = 0.9f;
        windDrift = 0.8f;
        noiseSpeed = 0.015f;
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
    // Heat propagation
    void propagateHeat2D();
    void propagateHeat1D();

    // Audio-driven heat injection
    void injectHeat(const AudioControl& audio);

    // Render heat buffer to pixel matrix
    void renderHeat(PixelMatrix& matrix);

    // Map heat value to fire color (dual palette with downbeat shift)
    uint32_t heatToColor(uint8_t heat) const;

    // Beat detection (same logic as particle Fire)
    bool beatHappened() const {
        return audio_.phase < 0.2f && prevPhase_ > 0.8f;
    }

    // Heat buffer — statically allocated at max device size
    // Largest supported: 32×64 = 2048 cells (covers all current devices)
    static constexpr uint16_t MAX_HEAT_CELLS = 2048;
    uint8_t heat_[MAX_HEAT_CELLS];

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

    // Palette state
    float paletteBias_;          // Smoothed energy*rhythm for warm/hot blend

    // Per-frame computed values (avoid recomputing in inner loops)
    float effectiveCooling_;
    float effectiveSpread_;
};
