#pragma once
#include "../devices/DeviceConfig.h"
#include <new>

class LEDMapper {
private:
    int width, height, totalPixels;
    MatrixOrientation orientation;
    int* positionToIndex;  // [y][x] -> LED index
    int* indexToX;         // LED index -> x coordinate
    int* indexToY;         // LED index -> y coordinate

    void cleanup() {
        delete[] positionToIndex;
        delete[] indexToX;
        delete[] indexToY;
        positionToIndex = nullptr;
        indexToX = nullptr;
        indexToY = nullptr;
    }

public:
    LEDMapper() : width(0), height(0), totalPixels(0), orientation(HORIZONTAL),
                  positionToIndex(nullptr), indexToX(nullptr), indexToY(nullptr) {}

    // Copy constructor
    LEDMapper(const LEDMapper& other) : width(other.width), height(other.height),
                                        totalPixels(other.totalPixels), orientation(other.orientation),
                                        positionToIndex(nullptr), indexToX(nullptr), indexToY(nullptr) {
        if (other.totalPixels > 0 && other.positionToIndex && other.indexToX && other.indexToY) {
            positionToIndex = new(std::nothrow) int[totalPixels];
            indexToX = new(std::nothrow) int[totalPixels];
            indexToY = new(std::nothrow) int[totalPixels];
            // Check all allocations succeeded before copying
            if (positionToIndex && indexToX && indexToY) {
                for (int i = 0; i < totalPixels; i++) {
                    positionToIndex[i] = other.positionToIndex[i];
                    indexToX[i] = other.indexToX[i];
                    indexToY[i] = other.indexToY[i];
                }
            } else {
                // Allocation failed - clean up and reset state
                cleanup();
                width = 0;
                height = 0;
                totalPixels = 0;
            }
        }
    }

    // Assignment operator
    LEDMapper& operator=(const LEDMapper& other) {
        if (this != &other) {
            cleanup();
            width = other.width;
            height = other.height;
            totalPixels = other.totalPixels;
            orientation = other.orientation;

            if (other.totalPixels > 0 && other.positionToIndex && other.indexToX && other.indexToY) {
                positionToIndex = new(std::nothrow) int[totalPixels];
                indexToX = new(std::nothrow) int[totalPixels];
                indexToY = new(std::nothrow) int[totalPixels];
                // Check all allocations succeeded before copying
                if (positionToIndex && indexToX && indexToY) {
                    for (int i = 0; i < totalPixels; i++) {
                        positionToIndex[i] = other.positionToIndex[i];
                        indexToX[i] = other.indexToX[i];
                        indexToY[i] = other.indexToY[i];
                    }
                } else {
                    // Allocation failed - clean up and reset state
                    cleanup();
                    width = 0;
                    height = 0;
                    totalPixels = 0;
                }
            }
        }
        return *this;
    }

    ~LEDMapper() {
        cleanup();
    }

    bool begin(const DeviceConfig& config) {
        cleanup(); // Clean up any existing allocation

        width = config.matrix.width;
        height = config.matrix.height;
        totalPixels = width * height;
        orientation = config.matrix.orientation;

        if (totalPixels <= 0) return false;

        // Allocate mapping arrays with error checking
        positionToIndex = new(std::nothrow) int[totalPixels];
        if (!positionToIndex) return false;

        indexToX = new(std::nothrow) int[totalPixels];
        if (!indexToX) {
            delete[] positionToIndex;
            positionToIndex = nullptr;
            return false;
        }

        indexToY = new(std::nothrow) int[totalPixels];
        if (!indexToY) {
            delete[] positionToIndex;
            delete[] indexToX;
            positionToIndex = nullptr;
            indexToX = nullptr;
            return false;
        }

        // Generate the mapping based on orientation and wiring pattern
        generateMapping();
        return true;
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

    // Check if mapper is properly initialized (useful after copy operations)
    bool isValid() const {
        return totalPixels > 0 && positionToIndex && indexToX && indexToY;
    }

    // Helper function to wrap coordinates
    int wrapX(int x) const {
        return (x % width + width) % width;
    }

    int wrapY(int y) const {
        return (y % height + height) % height;
    }

private:
    void generateMapping() {
        if (orientation == VERTICAL) {
            // Vertical column-major zigzag pattern (tube lights)
            // Each column is a continuous strip of `height` LEDs.
            // Even columns (0,2,...): top to bottom
            // Odd columns (1,3,...): bottom to top (zigzag wiring)
            //
            // 4x15 example:  Col 0: LEDs 0-14, Col 1: LEDs 29-15, ...
            // 4x60 example:  Col 0: LEDs 0-59, Col 1: LEDs 119-60, ...

            for (int x = 0; x < width; x++) {
                for (int y = 0; y < height; y++) {
                    int ledIndex;

                    if (x % 2 == 0) {
                        // Even columns: top to bottom
                        ledIndex = x * height + y;
                    } else {
                        // Odd columns: bottom to top (reversed)
                        ledIndex = x * height + (height - 1 - y);
                    }

                    positionToIndex[y * width + x] = ledIndex;
                    indexToX[ledIndex] = x;
                    indexToY[ledIndex] = y;
                }
            }
        } else {
            // Standard row-major mapping (horizontal layouts like bucket totem)
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
