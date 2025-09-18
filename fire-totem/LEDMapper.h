#pragma once
#include "configs/DeviceConfig.h"

class LEDMapper {
private:
    int width, height, totalPixels;
    MatrixOrientation orientation;
    int* positionToIndex;  // [y][x] -> LED index
    int* indexToX;         // LED index -> x coordinate
    int* indexToY;         // LED index -> y coordinate

public:
    LEDMapper() : width(0), height(0), totalPixels(0), orientation(HORIZONTAL),
                  positionToIndex(nullptr), indexToX(nullptr), indexToY(nullptr) {}

    ~LEDMapper() {
        if (positionToIndex) delete[] positionToIndex;
        if (indexToX) delete[] indexToX;
        if (indexToY) delete[] indexToY;
    }

    void begin(const DeviceConfig& config) {
        width = config.matrix.width;
        height = config.matrix.height;
        totalPixels = width * height;
        orientation = config.matrix.orientation;

        // Allocate mapping arrays
        positionToIndex = new int[totalPixels];
        indexToX = new int[totalPixels];
        indexToY = new int[totalPixels];

        // Generate the mapping based on orientation and wiring pattern
        generateMapping();
    }

    // Get LED index from matrix coordinates (x, y)
    int getIndex(int x, int y) const {
        if (x < 0 || x >= width || y < 0 || y >= height) return -1;
        return positionToIndex[y * width + x];
    }

    // Get matrix coordinates from LED index
    int getX(int index) const {
        if (index < 0 || index >= totalPixels) return -1;
        return indexToX[index];
    }

    int getY(int index) const {
        if (index < 0 || index >= totalPixels) return -1;
        return indexToY[index];
    }

    // Get matrix dimensions
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    int getTotalPixels() const { return totalPixels; }

    // Helper function to wrap coordinates
    int wrapX(int x) const {
        return (x % width + width) % width;
    }

    int wrapY(int y) const {
        return (y % height + height) % height;
    }

private:
    void generateMapping() {
        if (orientation == VERTICAL && width == 4 && height == 15) {
            // Tube light: 4x15 zigzag pattern
            // Col 0: LEDs 0-14 (top to bottom)
            // Col 1: LEDs 29-15 (bottom to top)
            // Col 2: LEDs 30-44 (top to bottom)
            // Col 3: LEDs 59-45 (bottom to top)

            for (int x = 0; x < width; x++) {
                for (int y = 0; y < height; y++) {
                    int ledIndex;

                    if (x % 2 == 0) {
                        // Even columns (0,2): normal top-to-bottom
                        ledIndex = x * height + y;
                    } else {
                        // Odd columns (1,3): bottom-to-top (reversed)
                        ledIndex = x * height + (height - 1 - y);
                    }

                    // Store both mappings
                    positionToIndex[y * width + x] = ledIndex;
                    indexToX[ledIndex] = x;
                    indexToY[ledIndex] = y;
                }
            }
        } else {
            // Standard row-major mapping (fire totem style)
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int ledIndex = y * width + x;
                    positionToIndex[y * width + x] = ledIndex;
                    indexToX[ledIndex] = x;
                    indexToY[ledIndex] = y;
                }
            }
        }
    }
};