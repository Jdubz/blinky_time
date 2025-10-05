#pragma once
#include "../Effect.h"

/**
 * NoOpEffect - A pass-through effect that does nothing
 *
 * This effect applies no transformation to the matrix data,
 * allowing generators to be displayed directly without modification.
 * Useful for testing and as a baseline effect.
 */
class NoOpEffect : public Effect {
public:
    NoOpEffect() = default;
    virtual ~NoOpEffect() = default;

    /**
     * Apply no transformation to the matrix
     * @param matrix The effect matrix to pass through unchanged
     */
    virtual void apply(EffectMatrix* matrix) override {
        // Intentionally do nothing - pass through the data unchanged
        (void)matrix; // Suppress unused parameter warning
    }

    /**
     * Reset effect state (no state to reset for NoOp)
     */
    virtual void reset() override {
        // Intentionally do nothing - no state to reset
    }

    /**
     * Get the name of this effect
     */
    virtual const char* getName() const override {
        return "NoOp";
    }

    /**
     * Get effect description
     */
    const char* getDescription() const {
        return "Pass-through effect - no transformation applied";
    }
};