#pragma once

#include <stdint.h>
#include <string.h>

/**
 * PropagationModel - Abstract heat/field propagation strategy
 *
 * Defines how heat spreads across the LED grid. Different layouts
 * require different propagation patterns:
 * - MATRIX: Heat rises upward (y decreases)
 * - LINEAR: Heat spreads laterally (both x directions)
 */
class PropagationModel {
public:
    virtual ~PropagationModel() = default;

    /**
     * Propagate heat values through the buffer
     * @param heat Heat buffer (numLeds elements)
     * @param width Grid width
     * @param height Grid height
     * @param decayFactor How much heat is retained per step (0.0-1.0)
     */
    virtual void propagate(uint8_t* heat, uint16_t width, uint16_t height,
                          float decayFactor) = 0;

    /**
     * Get neighbor indices for a given position
     * Returns indices that this position draws heat FROM
     * @param index Linear index of the position
     * @param width Grid width
     * @param height Grid height
     * @param numLeds Total number of LEDs
     * @param neighbors Output array (max 6 neighbors)
     * @param weights Output weights for each neighbor
     * @return Number of valid neighbors
     */
    virtual uint8_t getNeighbors(int index, uint16_t width, uint16_t height,
                                 uint16_t numLeds, int* neighbors, float* weights) = 0;

protected:
    /**
     * Convert 2D coordinates to linear index (row-major)
     */
    static int coordsToIndex(int x, int y, uint16_t width) {
        return y * width + x;
    }

    /**
     * Convert linear index to 2D coordinates
     */
    static void indexToCoords(int index, uint16_t width, int& x, int& y) {
        x = index % width;
        y = index / width;
    }
};
