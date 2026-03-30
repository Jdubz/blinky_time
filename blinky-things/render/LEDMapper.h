#pragma once
#include "../devices/DeviceConfig.h"
#include <new>
#include <string.h>

class LEDMapper {
private:
    static constexpr uint16_t INVALID_INDEX = 0xFFFF;
    static constexpr uint8_t INVALID_COORD = 0xFF;

    int width, height, totalPixels;
    MatrixOrientation orientation;
    uint16_t* positionToIndex;  // [y][x] -> LED index (0xFFFF = invalid)
    uint8_t* indexToX;          // LED index -> x coordinate (0xFF = invalid)
    uint8_t* indexToY;          // LED index -> y coordinate (0xFF = invalid)

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
            positionToIndex = new(std::nothrow) uint16_t[totalPixels];
            indexToX = new(std::nothrow) uint8_t[totalPixels];
            indexToY = new(std::nothrow) uint8_t[totalPixels];
            if (positionToIndex && indexToX && indexToY) {
                memcpy(positionToIndex, other.positionToIndex, totalPixels * sizeof(uint16_t));
                memcpy(indexToX, other.indexToX, totalPixels * sizeof(uint8_t));
                memcpy(indexToY, other.indexToY, totalPixels * sizeof(uint8_t));
            } else {
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
                positionToIndex = new(std::nothrow) uint16_t[totalPixels];
                indexToX = new(std::nothrow) uint8_t[totalPixels];
                indexToY = new(std::nothrow) uint8_t[totalPixels];
                if (positionToIndex && indexToX && indexToY) {
                    memcpy(positionToIndex, other.positionToIndex, totalPixels * sizeof(uint16_t));
                    memcpy(indexToX, other.indexToX, totalPixels * sizeof(uint8_t));
                    memcpy(indexToY, other.indexToY, totalPixels * sizeof(uint8_t));
                } else {
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
        positionToIndex = new(std::nothrow) uint16_t[totalPixels];
        if (!positionToIndex) return false;

        indexToX = new(std::nothrow) uint8_t[totalPixels];
        if (!indexToX) {
            delete[] positionToIndex;
            positionToIndex = nullptr;
            return false;
        }

        indexToY = new(std::nothrow) uint8_t[totalPixels];
        if (!indexToY) {
            delete[] positionToIndex;
            delete[] indexToX;
            positionToIndex = nullptr;
            indexToX = nullptr;
            return false;
        }

        // Fill with sentinel values so unwritten entries are safely invalid
        memset(positionToIndex, 0xFF, totalPixels * sizeof(uint16_t));
        memset(indexToX, 0xFF, totalPixels * sizeof(uint8_t));
        memset(indexToY, 0xFF, totalPixels * sizeof(uint8_t));

        // PANEL_GRID requires a 2×2 arrangement of equal panels.
        // Reject odd dimensions (fractional panel sizes).
        if (orientation == PANEL_GRID && (width % 2 != 0 || height % 2 != 0)) {
            cleanup();
            return false;
        }

        // Generate the mapping based on orientation and wiring pattern
        generateMapping();
        return true;
    }

    // Get LED index from matrix coordinates (x, y). Returns -1 if invalid.
    int getIndex(int x, int y) const {
        if (x < 0 || x >= width || y < 0 || y >= height) return -1;
        uint16_t idx = positionToIndex[y * width + x];
        return (idx == INVALID_INDEX) ? -1 : (int)idx;
    }

    // Get matrix coordinates from LED index. Returns -1 if invalid.
    int getX(int index) const {
        if (index < 0 || index >= totalPixels) return -1;
        uint8_t v = indexToX[index];
        return (v == INVALID_COORD) ? -1 : (int)v;
    }

    int getY(int index) const {
        if (index < 0 || index >= totalPixels) return -1;
        uint8_t v = indexToY[index];
        return (v == INVALID_COORD) ? -1 : (int)v;
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
    // Helper: set forward + inverse mapping with bounds check on ledIndex.
    // Prevents buffer overflow when PANEL_GRID produces out-of-range indices
    // on unsupported non-square grids.
    inline void setMapping(int x, int y, int ledIndex) {
        positionToIndex[y * width + x] = (ledIndex >= 0 && ledIndex < totalPixels)
            ? (uint16_t)ledIndex : INVALID_INDEX;
        if (ledIndex >= 0 && ledIndex < totalPixels) {
            indexToX[ledIndex] = (uint8_t)x;
            indexToY[ledIndex] = (uint8_t)y;
        }
    }

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

                    setMapping(x, y, ledIndex);
                }
            }
        } else if (orientation == PANEL_GRID) {
            // 2×2 grid of equal panels, chained TL→TR→BL→BR.
            // Within each panel: serpentine rows (even rows L→R, odd rows R→L).
            // Works for any even-dimension grid (square or rectangular).
            int panelW = width  / 2;
            int panelH = height / 2;
            int panelPixels = panelW * panelH;

            for (int gy = 0; gy < height; gy++) {
                for (int gx = 0; gx < width; gx++) {
                    int px = gx / panelW;   // Panel column (0=left, 1=right)
                    int lx = gx % panelW;   // Local x within panel
                    int py = gy / panelH;   // Panel row (0=top, 1=bottom)
                    int ly = gy % panelH;   // Local y within panel

                    int panelIdx = py * 2 + px;
                    int panelStart = panelIdx * panelPixels;

                    // Serpentine within panel: even rows L→R, odd rows R→L
                    int localIdx = (ly % 2 == 0)
                        ? (ly * panelW + lx)
                        : (ly * panelW + (panelW - 1 - lx));

                    int ledIndex = panelStart + localIdx;
                    setMapping(gx, gy, ledIndex);
                }
            }
        } else {
            // Standard row-major mapping (horizontal layouts like bucket totem)
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int ledIndex = y * width + x;
                    setMapping(x, y, ledIndex);
                }
            }
        }
    }
};
