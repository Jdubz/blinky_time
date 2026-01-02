#pragma once

#include "../IDetector.h"
#include <math.h>  // For expf() in inline function

/**
 * DrummerDetector - Time-domain amplitude spike detection
 *
 * The "Drummer's Algorithm" detects MUSICAL hits (kicks, snares, bass drops) by:
 * 1. LOUD: Significantly louder than local median (adaptive threshold)
 * 2. SUDDEN: Rapidly rising compared to ~50-70ms ago (ring buffer lookback)
 * 3. INFREQUENT: Cooldown prevents double-triggers
 *
 * This is a pure time-domain detector that doesn't require spectral data.
 * Works well on clear amplitude spikes with precise timing.
 *
 * Ported from AdaptiveMic::detectDrummer()
 *
 * Parameters (configurable via SerialConsole):
 * - threshold: Detection threshold as ratio (default 2.5, was 2.813 in hybrid mode)
 * - attackMultiplier: Required rise from baseline (default 1.1 = 10% rise)
 * - cooldownMs: Minimum time between detections (default 80ms)
 * - averageTau: Time constant for tracking average level (default 0.8s)
 *
 * Memory: ~100 bytes
 * CPU: <0.1ms per frame (no FFT needed)
 */
class DrummerDetector : public BaseDetector {
public:
    DrummerDetector();

    // IDetector interface
    DetectionResult detect(const AudioFrame& frame, float dt) override;
    DetectorType type() const override { return DetectorType::DRUMMER; }
    const char* name() const override { return "drummer"; }
    bool requiresSpectralData() const override { return false; }

    // Drummer-specific parameters
    void setAttackMultiplier(float mult) { attackMultiplier_ = mult; }
    float getAttackMultiplier() const { return attackMultiplier_; }

    void setAverageTau(float tau) { averageTau_ = tau; }
    float getAverageTau() const { return averageTau_; }

    void setCooldownMs(uint16_t ms) { cooldownMs_ = ms; }
    uint16_t getCooldownMs() const { return cooldownMs_; }

    // Debug access
    float getRecentAverage() const { return recentAverage_; }
    float getBaselineLevel() const { return attackBuffer_[attackBufferIdx_]; }

protected:
    void resetImpl() override;

private:
    // Attack detection ring buffer (compare against level from ~50-70ms ago)
    static constexpr int ATTACK_BUFFER_SIZE = 4;  // 4 frames @ 60Hz = ~67ms lookback
    float attackBuffer_[ATTACK_BUFFER_SIZE];
    int attackBufferIdx_;
    bool attackBufferInitialized_;

    // Recent average level (EMA)
    float recentAverage_;

    // Parameters
    float attackMultiplier_;   // Required rise from baseline (1.1 = 10% rise)
    float averageTau_;         // EMA time constant in seconds
    uint16_t cooldownMs_;      // Cooldown between detections

    // Helper: exponential factor
    static float expFactor(float dt, float tau) {
        // Use fast approximation for small dt/tau ratios
        float ratio = dt / tau;
        if (ratio < 0.1f) {
            return ratio;  // Linear approximation for small values
        }
        // Full exponential for larger ratios
        return 1.0f - expf(-ratio);
    }

    // Compute confidence based on signal-to-noise ratio
    float computeConfidence(float rawLevel, float median, float ratio) const;
};
