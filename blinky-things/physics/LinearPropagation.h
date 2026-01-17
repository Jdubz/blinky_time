#pragma once

#include "PropagationModel.h"
#include <Arduino.h>

/**
 * LinearPropagation - Lateral heat propagation for 1D linear layouts
 *
 * Heat spreads left and right from each position with weighted
 * averaging from neighboring cells. Supports wrapping for circular
 * arrangements (like a hat brim).
 *
 * Propagation pattern (for each cell):
 *   - 2x weight from self (center persistence)
 *   - 1x weight from left neighbor
 *   - 1x weight from right neighbor
 *   - 1x weight from 2 positions left
 *   - 1x weight from 2 positions right
 */
class LinearPropagation : public PropagationModel {
public:
    explicit LinearPropagation(bool wrap = true) : wrap_(wrap) {}

    void propagate(uint8_t* heat, uint16_t width, uint16_t height,
                  float decayFactor) override {
        uint16_t numLeds = width * height;

        // Use static buffer to avoid allocation
        // LIMITATION: Max 256 LEDs supported for linear propagation
        static uint8_t tempHeat[256];
        if (numLeds > 256 || numLeds == 0) {
            // Cannot propagate: buffer too small or no LEDs
            // Skip propagation rather than corrupt memory or produce incorrect results
            return;
        }

        for (int i = 0; i < numLeds; i++) {
            uint16_t totalHeat = (uint16_t)heat[i] * 2;  // Self weight 2
            uint8_t divisor = 2;

            // Left neighbor
            int leftIdx = wrapIndex(i - 1, numLeds);
            if (leftIdx >= 0) {
                totalHeat += heat[leftIdx];
                divisor++;
            }

            // Right neighbor
            int rightIdx = wrapIndex(i + 1, numLeds);
            if (rightIdx >= 0) {
                totalHeat += heat[rightIdx];
                divisor++;
            }

            // Two positions left (for wider spread)
            int left2Idx = wrapIndex(i - 2, numLeds);
            if (left2Idx >= 0) {
                totalHeat += heat[left2Idx];
                divisor++;
            }

            // Two positions right
            int right2Idx = wrapIndex(i + 2, numLeds);
            if (right2Idx >= 0) {
                totalHeat += heat[right2Idx];
                divisor++;
            }

            // Apply decay and store in temp buffer
            uint16_t newHeat = (uint16_t)((totalHeat / divisor) * decayFactor);
            tempHeat[i] = min(255, (int)newHeat);
        }

        // Copy back to heat buffer
        memcpy(heat, tempHeat, numLeds);
    }

    uint8_t getNeighbors(int index, uint16_t width, uint16_t height,
                         uint16_t numLeds, int* neighbors, float* weights) override {
        uint8_t count = 0;

        // Left neighbor
        int leftIdx = wrapIndex(index - 1, numLeds);
        if (leftIdx >= 0) {
            neighbors[count] = leftIdx;
            weights[count] = 1.0f;
            count++;
        }

        // Right neighbor
        int rightIdx = wrapIndex(index + 1, numLeds);
        if (rightIdx >= 0) {
            neighbors[count] = rightIdx;
            weights[count] = 1.0f;
            count++;
        }

        // Two left (same weight as propagate uses)
        int left2Idx = wrapIndex(index - 2, numLeds);
        if (left2Idx >= 0) {
            neighbors[count] = left2Idx;
            weights[count] = 1.0f;
            count++;
        }

        // Two right (same weight as propagate uses)
        int right2Idx = wrapIndex(index + 2, numLeds);
        if (right2Idx >= 0) {
            neighbors[count] = right2Idx;
            weights[count] = 1.0f;
            count++;
        }

        return count;
    }

private:
    bool wrap_;

    /**
     * Wrap or clamp index based on wrap_ setting
     * Returns -1 if index is out of bounds and wrap is disabled
     */
    int wrapIndex(int idx, uint16_t numLeds) const {
        // CRITICAL: Guard against zero numLeds to prevent infinite loop
        if (numLeds == 0) return -1;

        if (wrap_) {
            // Wrap around using modulo (safe, no infinite loop)
            idx = idx % (int)numLeds;
            if (idx < 0) idx += numLeds;
            return idx;
        } else {
            // Clamp to bounds, return -1 if out
            if (idx < 0 || idx >= numLeds) return -1;
            return idx;
        }
    }
};
