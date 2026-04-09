#pragma once

#include "Generator.h"
#include "../math/SimplexNoise.h"

/**
 * PlasmaGlobe - Continuous-field plasma globe generator
 *
 * Replaces the particle-based Lightning with a continuous plasma field.
 * Mostly dark with 2-4 bright purple/violet orbs drifting via noise steering.
 * Brief bright pulses on beats. Always-on but very dim ambient glow.
 *
 * NOT particle-based — extends Generator directly. Plasma is a continuous
 * field, not discrete spawn/kill events.
 *
 * Design constraints:
 * - ~15-25% lit pixels average (LEDs are extremely bright)
 * - Large areas OFF most of the time
 * - Brief high-impact moments on beats/transients
 * - Battery-conscious (fewer lit pixels = less power)
 */

static constexpr int PLASMA_MAX_ORBS = 4;

struct PlasmaGlobeParams {
    float backgroundDim;          // Ambient background brightness (0-1, very low)
    float orbBrightness;          // Peak orb brightness (0-1)
    float orbRadius;              // Orb radius as fraction of diagonal
    float driftSpeed;             // Noise-driven drift speed
    float pulseDecay;             // Beat pulse decay rate (per frame, 0-1)
    float pulseBrightness;        // Extra brightness on pulse (0-1)
    float pulseExpand;            // Radius expansion on pulse (fraction)

    PlasmaGlobeParams() {
        backgroundDim = 0.01f;     // Nearly invisible ambient
        orbBrightness = 0.75f;     // Bright orbs
        orbRadius = 0.06f;         // Tight orbs (~6% of diagonal)
        driftSpeed = 0.012f;       // Slow organic drift
        pulseDecay = 0.88f;        // ~8 frame pulse decay
        pulseBrightness = 0.5f;    // Moderate pulse flash
        pulseExpand = 0.3f;        // 30% radius expansion on pulse
    }
};

class PlasmaGlobe : public Generator {
public:
    PlasmaGlobe();
    ~PlasmaGlobe() override = default;

    bool begin(const DeviceConfig& config) override;
    void generate(PixelMatrix& matrix, const AudioControl& audio) override;
    void reset() override;
    const char* getName() const override { return "PlasmaGlobe"; }
    GeneratorType getType() const override { return GeneratorType::LIGHTNING; }

    void setParams(const PlasmaGlobeParams& p) { params_ = p; }
    const PlasmaGlobeParams& getParams() const { return params_; }
    PlasmaGlobeParams& getParamsMutable() { return params_; }

private:
    // Map intensity [0,1] to purple/violet RGB
    void purplePalette(float intensity, uint8_t& r, uint8_t& g, uint8_t& b) const;

    PlasmaGlobeParams params_;
    float noiseTime_;
    float pulseEnvelope_;         // Decaying pulse brightness
    float pulseRadiusEnv_;        // Decaying pulse radius expansion

    // Orb state — positions driven by noise field
    float orbX_[PLASMA_MAX_ORBS];
    float orbY_[PLASMA_MAX_ORBS];
    float orbPhaseOffset_[PLASMA_MAX_ORBS];  // Per-orb phase for individuality
    int numOrbs_;
};
