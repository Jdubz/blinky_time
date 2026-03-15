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

        // PANEL_GRID requires even dimensions so the 2×2 grid splits cleanly.
        // Odd dimensions produce truncated panel sizes and out-of-bounds indices.
        if (orientation == PANEL_GRID && (width % 2 != 0 || height % 2 != 0)) {
            cleanup();
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
        } else if (orientation == PANEL_GRID) {
            // 2×2 grid of equal panels, chained TL→TR→BL→BR.
            // Within each panel: serpentine rows (even rows left→right,
            // odd rows right→left) — standard WS2812B matrix wiring.
            //
            // Physical chain order (data-in perspective):
            //   Panel 0 (TL) → Panel 1 (TR) → Panel 2 (BL) → Panel 3 (BR)
            //   BUT physical panels 0 and 3 are swapped relative to logical
            //   coordinates (panelIdx swap below), so logical TL = chain BR.
            //
            // Physical→logical transpose:
            //   Logical (gx, gy) → physical (phx=gy, phy=gx)
            //   This corrects a 90° CCW rotation in the physical panel
            //   mounting (the panels are installed rotated — top-right
            //   of the physical chain becomes logical top-left).
            //
            //   Logical grid:         Physical chain order:
            //   (0,0) (1,0) ...       Panel3 Panel1
            //   (0,1) (1,1) ...       Panel2 Panel0
            //   ↑ logical origin      (after swap: TL=3, TR=1, BL=2, BR=0)
            int panelW = width  / 2;
            int panelH = height / 2;
            int panelPixels = panelW * panelH;

            for (int gy = 0; gy < height; gy++) {
                for (int gx = 0; gx < width; gx++) {
                    // Transpose: swap logical x/y before panel lookup
                    int phx = gy;  // physical x = logical y
                    int phy = gx;  // physical y = logical x

                    int px = phx / panelW;  // Panel column (0=left, 1=right)
                    int lx = phx % panelW;  // Local x within panel
                    int py = phy / panelH;  // Panel row (0=top, 1=bottom)
                    int ly = phy % panelH;  // Local y within panel

                    int panelIdx  = py * 2 + px;         // Chain order: 0=TL,1=TR,2=BL,3=BR
                    // Swap TL (0) and BR (3) panel positions
                    if (panelIdx == 0 || panelIdx == 3) panelIdx = 3 - panelIdx;
                    int panelStart = panelIdx * panelPixels;

                    // Serpentine within panel: even rows L→R, odd rows R→L
                    int localIdx = (ly % 2 == 0)
                        ? (ly * panelW + lx)
                        : (ly * panelW + (panelW - 1 - lx));

                    int ledIndex = panelStart + localIdx;
                    positionToIndex[gy * width + gx] = ledIndex;
                    indexToX[ledIndex] = gx;
                    indexToY[ledIndex] = gy;
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
