#pragma once

#include "PropagationModel.h"
#include <Arduino.h>

/**
 * MatrixPropagation - Upward heat propagation for 2D matrix layouts
 *
 * Heat flows upward (y decreases) with weighted averaging from
 * cells below and to the sides. This creates the classic fire
 * effect where flames rise from the bottom.
 *
 * Propagation pattern (for each cell):
 *   - 1x weight from direct below (y+1)
 *   - 2x weight from two below (y+2)
 *   - 1x weight from left-below (x-1, y+1)
 *   - 1x weight from right-below (x+1, y+1)
 */
class MatrixPropagation : public PropagationModel {
public:
    void propagate(uint8_t* heat, uint16_t width, uint16_t height,
                  float decayFactor) override {
        // Need at least 3 rows for upward propagation
        if (height < 3) return;

        // Process from top to bottom to avoid feedback loops
        for (int y = 0; y < height - 2; y++) {
            for (int x = 0; x < width; x++) {
                int currentIdx = coordsToIndex(x, y, width);

                // Weighted sum from cells below
                uint16_t totalHeat = 0;
                uint8_t divisor = 0;

                // Direct below (weight 1)
                int belowIdx = coordsToIndex(x, y + 1, width);
                totalHeat += heat[belowIdx];
                divisor++;

                // Two below (weight 2)
                int below2Idx = coordsToIndex(x, y + 2, width);
                totalHeat += (uint16_t)heat[below2Idx] * 2;
                divisor += 2;

                // Horizontal spread from below row
                if (x > 0) {
                    int leftIdx = coordsToIndex(x - 1, y + 1, width);
                    totalHeat += heat[leftIdx];
                    divisor++;
                }
                if (x < width - 1) {
                    int rightIdx = coordsToIndex(x + 1, y + 1, width);
                    totalHeat += heat[rightIdx];
                    divisor++;
                }

                // Apply decay and store
                uint16_t newHeat = (uint16_t)((totalHeat / divisor) * decayFactor);
                heat[currentIdx] = min(255, (int)newHeat);
            }
        }
    }

    uint8_t getNeighbors(int index, uint16_t width, uint16_t height,
                         uint16_t numLeds, int* neighbors, float* weights) override {
        int x, y;
        indexToCoords(index, width, x, y);

        uint8_t count = 0;

        // Cell below
        if (y + 1 < height) {
            neighbors[count] = coordsToIndex(x, y + 1, width);
            weights[count] = 1.0f;
            count++;
        }

        // Cell two below
        if (y + 2 < height) {
            neighbors[count] = coordsToIndex(x, y + 2, width);
            weights[count] = 2.0f;
            count++;
        }

        // Left-below
        if (x > 0 && y + 1 < height) {
            neighbors[count] = coordsToIndex(x - 1, y + 1, width);
            weights[count] = 1.0f;
            count++;
        }

        // Right-below
        if (x < width - 1 && y + 1 < height) {
            neighbors[count] = coordsToIndex(x + 1, y + 1, width);
            weights[count] = 1.0f;
            count++;
        }

        return count;
    }
};
