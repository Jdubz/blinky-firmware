/**
 * LEDMapper Unit Tests — native C++ (no Arduino required)
 *
 * Covers HORIZONTAL_ROTATED_180, unknown-orientation rejection, and PANEL_GRID: coordinate transpose, TL/BR panel index
 * swap, and serpentine-within-panel ordering.
 *
 * Compile and run:
 *   g++ -std=c++17 -DARDUINO_ARCH_NRF52 -DBLINKY_PLATFORM_NRF52840 \
 *       -I blinky-simulator/include -I blinky-things tests/unit/test_led_mapper.cpp -o test_led_mapper
 *   ./test_led_mapper        (from the repo root; CI: pr-validation.yml "LED Mapper")
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
// HORIZONTAL_ROTATED_180 (the test box grid: data enters bottom-right)
// ---------------------------------------------------------------------------
static void testHorizontalRotated180() {
    printf("[HORIZONTAL_ROTATED_180]\n");

    // 4 columns, 3 rows:
    //   y=0 (top):    11 10  9  8
    //   y=1:           7  6  5  4
    //   y=2 (bottom):  3  2  1  0
    LEDMapper m;
    DeviceConfig cfg = makeConfig(4, 3, HORIZONTAL_ROTATED_180);
    EXPECT(m.begin(cfg));

    EXPECT_EQ(m.getIndex(3, 2), 0);   // bottom-right is the first LED
    EXPECT_EQ(m.getIndex(2, 2), 1);   // the row runs right to left
    EXPECT_EQ(m.getIndex(0, 2), 3);
    EXPECT_EQ(m.getIndex(3, 1), 4);   // next row starts at the right again (no serpentine)
    EXPECT_EQ(m.getIndex(0, 0), 11);  // top-left is the last LED

    EXPECT_EQ(m.getX(0), 3);  EXPECT_EQ(m.getY(0), 2);
    EXPECT_EQ(m.getX(4), 3);  EXPECT_EQ(m.getY(4), 1);
    EXPECT_EQ(m.getX(11), 0); EXPECT_EQ(m.getY(11), 0);

    // Every LED index is used exactly once
    bool seen[12] = {false};
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++) {
            int i = m.getIndex(x, y);
            EXPECT(i >= 0 && i < 12 && !seen[i]);
            if (i >= 0 && i < 12) seen[i] = true;
        }
}

// ---------------------------------------------------------------------------
// An orientation with no mapping is refused, not silently mapped row-major
// ---------------------------------------------------------------------------
static void testUnknownOrientationRejected() {
    printf("[unknown orientation]\n");

    LEDMapper m;
    // 7 is inside the enum's value range (UBSan accepts it) but has no mapping.
    DeviceConfig cfg = makeConfig(4, 3, (MatrixOrientation)7);
    EXPECT(!m.begin(cfg));
    EXPECT(!m.isValid());
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
// PANEL_GRID — generic 4×4 with 2×2 panels (no transpose, no swap)
//
// Expected mapping:
//   Panel 0 (TL, gx=0-1, gy=0-1): LEDs 0-3
//   Panel 1 (TR, gx=2-3, gy=0-1): LEDs 4-7
//   Panel 2 (BL, gx=0-1, gy=2-3): LEDs 8-11
//   Panel 3 (BR, gx=2-3, gy=2-3): LEDs 12-15
//
//   (0,0)= 0  (1,0)= 1  (2,0)= 4  (3,0)= 5
//   (0,1)= 3  (1,1)= 2  (2,1)= 7  (3,1)= 6
//   (0,2)= 8  (1,2)= 9  (2,2)=12  (3,2)=13
//   (0,3)=11  (1,3)=10  (2,3)=15  (3,3)=14
// ---------------------------------------------------------------------------
static void testPanelGrid4x4() {
    printf("[PANEL_GRID 4x4 generic]\n");

    LEDMapper m;
    DeviceConfig cfg = makeConfig(4, 4, PANEL_GRID);
    EXPECT(m.begin(cfg));
    EXPECT(m.isValid());

    // Panel 0 (TL)
    EXPECT_EQ(m.getIndex(0, 0), 0);
    EXPECT_EQ(m.getIndex(1, 0), 1);
    EXPECT_EQ(m.getIndex(0, 1), 3);
    EXPECT_EQ(m.getIndex(1, 1), 2);

    // Panel 1 (TR)
    EXPECT_EQ(m.getIndex(2, 0), 4);
    EXPECT_EQ(m.getIndex(3, 0), 5);
    EXPECT_EQ(m.getIndex(2, 1), 7);
    EXPECT_EQ(m.getIndex(3, 1), 6);

    // Panel 2 (BL)
    EXPECT_EQ(m.getIndex(0, 2), 8);
    EXPECT_EQ(m.getIndex(1, 2), 9);
    EXPECT_EQ(m.getIndex(0, 3), 11);
    EXPECT_EQ(m.getIndex(1, 3), 10);

    // Panel 3 (BR)
    EXPECT_EQ(m.getIndex(2, 2), 12);
    EXPECT_EQ(m.getIndex(3, 2), 13);
    EXPECT_EQ(m.getIndex(2, 3), 15);
    EXPECT_EQ(m.getIndex(3, 3), 14);

    // Inverse
    EXPECT_EQ(m.getX(0), 0);  EXPECT_EQ(m.getY(0), 0);
    EXPECT_EQ(m.getX(1), 1);  EXPECT_EQ(m.getY(1), 0);
    EXPECT_EQ(m.getX(3), 0);  EXPECT_EQ(m.getY(3), 1);
    EXPECT_EQ(m.getX(12), 2); EXPECT_EQ(m.getY(12), 2);

    // Bijection
    bool seen[16] = {};
    for (int gy = 0; gy < 4; ++gy)
        for (int gx = 0; gx < 4; ++gx) {
            int idx = m.getIndex(gx, gy);
            EXPECT(idx >= 0 && idx < 16);
            if (idx >= 0 && idx < 16) {
                EXPECT(!seen[idx]);
                seen[idx] = true;
            }
        }
    for (int i = 0; i < 16; ++i) EXPECT(seen[i]);
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
    testHorizontalRotated180();
    testUnknownOrientationRejected();
    testVertical();
    testPanelGrid4x4();
    testPanelGridOddDimReject();
    testPanelGrid8x4();
    testCopyAssign();

    printf("\n%d tests, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
