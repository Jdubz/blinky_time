/**
 * LEDMapper Unit Tests — native C++ (no Arduino required)
 *
 * Covers PANEL_GRID orientation: coordinate transpose, TL/BR panel index
 * swap, and serpentine-within-panel ordering.
 *
 * Compile and run:
 *   g++ -std=c++11 -I../../blinky-things test_led_mapper.cpp -o test_led_mapper
 *   ./test_led_mapper
 */

// ---------------------------------------------------------------------------
// Minimal stubs so LEDMapper compiles without Arduino or platform headers
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

// Stub <stdint.h> types are already provided by <cstdint>.
// PlatformConstants.h uses only stdint types — nothing else needed.

#include "../../blinky-things/render/LEDMapper.h"

// ---------------------------------------------------------------------------
// Lightweight test harness
// ---------------------------------------------------------------------------
static int g_total = 0;
static int g_failed = 0;

static void check(const char* expr, bool ok, const char* file, int line) {
    ++g_total;
    if (!ok) {
        ++g_failed;
        printf("  FAIL  %s:%d  %s\n", file, line, expr);
    }
}

#define EXPECT(cond) check(#cond, (cond), __FILE__, __LINE__)
#define EXPECT_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        ++g_total; \
        if (_a != _b) { \
            ++g_failed; \
            printf("  FAIL  %s:%d  expected %d, got %d  [" #a " == " #b "]\n", \
                   __FILE__, __LINE__, (int)_b, (int)_a); \
        } \
    } while (0)

// ---------------------------------------------------------------------------
// Helper: build a DeviceConfig for LEDMapper::begin()
// ---------------------------------------------------------------------------
static DeviceConfig makeConfig(int w, int h, MatrixOrientation orient) {
    DeviceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.deviceName          = "test";
    cfg.matrix.width        = (uint8_t)w;
    cfg.matrix.height       = (uint8_t)h;
    cfg.matrix.orientation  = orient;
    cfg.matrix.layoutType   = MATRIX_LAYOUT;
    cfg.matrix.ledPin       = 1;
    cfg.matrix.brightness   = 128;
    // charging: minVoltage / maxVoltage must pass DeviceConfigLoader::validate
    // but LEDMapper only reads matrix fields — leave others at 0
    return cfg;
}

// ---------------------------------------------------------------------------
// HORIZONTAL (row-major) baseline
// ---------------------------------------------------------------------------
static void testHorizontal() {
    printf("[HORIZONTAL]\n");

    LEDMapper m;
    DeviceConfig cfg = makeConfig(4, 3, HORIZONTAL);
    EXPECT(m.begin(cfg));

    // Row-major: LED(x,y) = y*width + x
    EXPECT_EQ(m.getIndex(0, 0), 0);
    EXPECT_EQ(m.getIndex(3, 0), 3);
    EXPECT_EQ(m.getIndex(0, 1), 4);
    EXPECT_EQ(m.getIndex(3, 2), 11);

    // Inverse
    EXPECT_EQ(m.getX(0), 0);  EXPECT_EQ(m.getY(0), 0);
    EXPECT_EQ(m.getX(7), 3);  EXPECT_EQ(m.getY(7), 1);
    EXPECT_EQ(m.getX(11), 3); EXPECT_EQ(m.getY(11), 2);

    // Out-of-bounds
    EXPECT_EQ(m.getIndex(-1, 0), -1);
    EXPECT_EQ(m.getIndex(4, 0),  -1);
    EXPECT_EQ(m.getIndex(0, 3),  -1);
}

// ---------------------------------------------------------------------------
// VERTICAL (column-major zigzag)
// ---------------------------------------------------------------------------
static void testVertical() {
    printf("[VERTICAL]\n");

    // 4 columns, 3 rows
    // Col 0 (even): LEDs 0-2  top→bottom
    // Col 1 (odd):  LEDs 5-3  bottom→top
    // Col 2 (even): LEDs 6-8  top→bottom
    // Col 3 (odd):  LEDs 11-9 bottom→top
    LEDMapper m;
    DeviceConfig cfg = makeConfig(4, 3, VERTICAL);
    EXPECT(m.begin(cfg));

    EXPECT_EQ(m.getIndex(0, 0), 0);  // col0 top
    EXPECT_EQ(m.getIndex(0, 2), 2);  // col0 bottom
    EXPECT_EQ(m.getIndex(1, 0), 5);  // col1 top (reversed: 3+2=5)
    EXPECT_EQ(m.getIndex(1, 2), 3);  // col1 bottom
    EXPECT_EQ(m.getIndex(2, 0), 6);  // col2 top
    EXPECT_EQ(m.getIndex(3, 2), 9);  // col3 bottom (3*3 + (3-1-2) = 9)
}

// ---------------------------------------------------------------------------
// PANEL_GRID — 4×4 with 2×2 panels
//
// Expected mapping (hand-computed from LEDMapper algorithm):
//
//   Logical grid     → LED index
//   (gx, gy)
//   (0,0)=12  (1,0)=15  (2,0)= 8  (3,0)=11
//   (0,1)=13  (1,1)=14  (2,1)= 9  (3,1)=10
//   (0,2)= 4  (1,2)= 7  (2,2)= 0  (3,2)= 3
//   (0,3)= 5  (1,3)= 6  (2,3)= 1  (3,3)= 2
//
// Panel layout (chain order after TL↔BR swap):
//   Logical TL (gx=0-1, gy=0-1) → chain 3, LEDs 12-15
//   Logical TR (gx=2-3, gy=0-1) → chain 2, LEDs  8-11
//   Logical BL (gx=0-1, gy=2-3) → chain 1, LEDs  4- 7
//   Logical BR (gx=2-3, gy=2-3) → chain 0, LEDs  0- 3
// ---------------------------------------------------------------------------
static void testPanelGrid4x4() {
    printf("[PANEL_GRID 4x4]\n");

    LEDMapper m;
    DeviceConfig cfg = makeConfig(4, 4, PANEL_GRID);
    EXPECT(m.begin(cfg));
    EXPECT(m.isValid());

    // --- Forward mapping (gx, gy) → LED ---
    // Top-left logical panel (chain 3, LEDs 12-15)
    EXPECT_EQ(m.getIndex(0, 0), 12);
    EXPECT_EQ(m.getIndex(1, 0), 15);
    EXPECT_EQ(m.getIndex(0, 1), 13);
    EXPECT_EQ(m.getIndex(1, 1), 14);

    // Top-right logical panel (chain 2, LEDs 8-11)
    EXPECT_EQ(m.getIndex(2, 0),  8);
    EXPECT_EQ(m.getIndex(3, 0), 11);
    EXPECT_EQ(m.getIndex(2, 1),  9);
    EXPECT_EQ(m.getIndex(3, 1), 10);

    // Bottom-left logical panel (chain 1, LEDs 4-7)
    EXPECT_EQ(m.getIndex(0, 2), 4);
    EXPECT_EQ(m.getIndex(1, 2), 7);
    EXPECT_EQ(m.getIndex(0, 3), 5);
    EXPECT_EQ(m.getIndex(1, 3), 6);

    // Bottom-right logical panel (chain 0, LEDs 0-3)
    EXPECT_EQ(m.getIndex(2, 2), 0);
    EXPECT_EQ(m.getIndex(3, 2), 3);
    EXPECT_EQ(m.getIndex(2, 3), 1);
    EXPECT_EQ(m.getIndex(3, 3), 2);

    // --- Inverse mapping: LED → (gx, gy) ---
    EXPECT_EQ(m.getX(12), 0);  EXPECT_EQ(m.getY(12), 0);
    EXPECT_EQ(m.getX(15), 1);  EXPECT_EQ(m.getY(15), 0);
    EXPECT_EQ(m.getX(13), 0);  EXPECT_EQ(m.getY(13), 1);
    EXPECT_EQ(m.getX(14), 1);  EXPECT_EQ(m.getY(14), 1);

    EXPECT_EQ(m.getX(0), 2);   EXPECT_EQ(m.getY(0), 2);
    EXPECT_EQ(m.getX(3), 3);   EXPECT_EQ(m.getY(3), 2);
    EXPECT_EQ(m.getX(1), 2);   EXPECT_EQ(m.getY(1), 3);
    EXPECT_EQ(m.getX(2), 3);   EXPECT_EQ(m.getY(2), 3);

    // --- Every index is unique (bijection check) ---
    bool seen[16] = {};
    for (int gy = 0; gy < 4; ++gy) {
        for (int gx = 0; gx < 4; ++gx) {
            int idx = m.getIndex(gx, gy);
            EXPECT(idx >= 0 && idx < 16);
            if (idx >= 0 && idx < 16) {
                EXPECT(!seen[idx]);   // no duplicate
                seen[idx] = true;
            }
        }
    }
    for (int i = 0; i < 16; ++i) EXPECT(seen[i]);  // all 16 covered
}

// ---------------------------------------------------------------------------
// PANEL_GRID — odd dimension must fail begin()
// ---------------------------------------------------------------------------
static void testPanelGridOddDimReject() {
    printf("[PANEL_GRID odd dimensions rejected]\n");

    LEDMapper m;
    DeviceConfig cfg = makeConfig(3, 4, PANEL_GRID);  // width odd
    EXPECT(!m.begin(cfg));

    cfg = makeConfig(4, 3, PANEL_GRID);  // height odd
    EXPECT(!m.begin(cfg));

    cfg = makeConfig(3, 3, PANEL_GRID);  // both odd
    EXPECT(!m.begin(cfg));
}

// ---------------------------------------------------------------------------
// PANEL_GRID — 8×4 with 4×2 panels
//   panelW=4, panelH=2, panelPixels=8, 4 panels × 8 = 32 LEDs
//   Spot-check a few corners to confirm the algorithm generalises
// ---------------------------------------------------------------------------
static void testPanelGrid8x4() {
    printf("[PANEL_GRID 8x4]\n");

    LEDMapper m;
    DeviceConfig cfg = makeConfig(8, 4, PANEL_GRID);
    EXPECT(m.begin(cfg));

    // Bijection: all 32 LED indices used exactly once
    bool seen[32] = {};
    int dupes = 0, oob = 0;
    for (int gy = 0; gy < 4; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            int idx = m.getIndex(gx, gy);
            if (idx < 0 || idx >= 32) { ++oob; continue; }
            if (seen[idx]) ++dupes;
            seen[idx] = true;
        }
    }
    EXPECT(oob == 0);
    EXPECT(dupes == 0);
    for (int i = 0; i < 32; ++i) EXPECT(seen[i]);
}

// ---------------------------------------------------------------------------
// Copy / assignment
// ---------------------------------------------------------------------------
static void testCopyAssign() {
    printf("[Copy / Assignment]\n");

    LEDMapper src;
    DeviceConfig cfg = makeConfig(4, 4, PANEL_GRID);
    EXPECT(src.begin(cfg));

    // Copy constructor
    LEDMapper copy(src);
    EXPECT(copy.isValid());
    for (int gy = 0; gy < 4; ++gy)
        for (int gx = 0; gx < 4; ++gx)
            EXPECT_EQ(copy.getIndex(gx, gy), src.getIndex(gx, gy));

    // Assignment operator
    LEDMapper assigned;
    assigned = src;
    EXPECT(assigned.isValid());
    EXPECT_EQ(assigned.getIndex(2, 2), src.getIndex(2, 2));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("=== LEDMapper Unit Tests ===\n\n");

    testHorizontal();
    testVertical();
    testPanelGrid4x4();
    testPanelGridOddDimReject();
    testPanelGrid8x4();
    testCopyAssign();

    printf("\n%d tests, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
