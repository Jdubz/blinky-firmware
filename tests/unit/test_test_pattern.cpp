/**
 * TestPattern unit tests (render/TestPattern.h) - native C++, no Arduino.
 *
 * The camera calibration patterns: solid and pixel fills, and the FLASH train's
 * schedule and edge stamps (the host's timing reference for the box camera).
 */
#include <cstdint>
#include <cstdio>

#include "../../blinky-things/render/TestPattern.h"

static int g_total = 0;
static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        g_total++;                                                      \
        if (!(cond)) {                                                  \
            g_failed++;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static int litCount(const PixelMatrix& m) {
    int n = 0;
    for (int y = 0; y < m.getHeight(); y++)
        for (int x = 0; x < m.getWidth(); x++) n += m.getPixel(x, y) != RGB();
    return n;
}

static void testOffSolidPixel() {
    PixelMatrix m(8, 8);
    TestPattern tp;
    CHECK(!tp.active());
    tp.fill(m);
    CHECK(litCount(m) == 0);

    tp.solid(10, 20, 30);
    CHECK(tp.active() && tp.mode() == TestPattern::Mode::SOLID);
    tp.fill(m);
    CHECK(litCount(m) == 64);
    CHECK(m.getPixel(7, 7) == RGB(10, 20, 30));

    m.clear();
    tp.pixel(3, 5, 0, 255, 0);
    tp.fill(m);
    CHECK(litCount(m) == 1);
    CHECK(m.getPixel(3, 5) == RGB(0, 255, 0));

    tp.off();
    CHECK(!tp.active());
}

static void testFlashRefusesBadParameters() {
    TestPattern tp;
    CHECK(!tp.flash(255, 255, 255, 0, 100, 10, 1000));    // no on time
    CHECK(!tp.flash(255, 255, 255, 100, 100, 10, 1000));  // on time fills the period
    CHECK(!tp.flash(255, 255, 255, 50, 100, 0, 1000));    // no flashes
    CHECK(!tp.flash(255, 255, 255, 50, 100, TestPattern::MAX_FLASHES + 1, 1000));
    CHECK(!tp.active());  // a refused train changes nothing
}

// Drive a frame loop at `frameMs` and return the matrix lit state per frame.
static void runFlashes(TestPattern& tp, uint32_t start, uint32_t frameMs, uint32_t untilMs) {
    PixelMatrix m(4, 4);
    for (uint32_t t = start; t < untilMs; t += frameMs) {
        tp.update(t);
        m.clear();
        tp.fill(m);
        tp.shown(t);  // the frame went to the LEDs at t
    }
}

static void testFlashScheduleAndStamps() {
    TestPattern tp;
    const uint32_t start = 5000;
    CHECK(tp.flash(255, 0, 0, 30, 200, 20, start));
    runFlashes(tp, start, 7, start + 20 * 300 + 1000);  // 7 ms frames, enough time for all
    CHECK(tp.done());
    CHECK(tp.recorded() == 20);
    uint32_t prevOn = start;
    for (int i = 0; i < tp.recorded(); i++) {
        const TestPattern::Edge& e = tp.edge(i);
        // On for ~30 ms, quantized to the frame loop.
        CHECK(e.offMs > e.onMs);
        CHECK(e.offMs - e.onMs >= 30 && e.offMs - e.onMs < 30 + 7 + 1);
        // Interval: one period plus up to half of it, then the frame quantization.
        uint32_t gap = e.onMs - prevOn;
        if (i == 0) {
            CHECK(gap >= 200 && gap < 200 + 7);
        } else {
            CHECK(gap >= 200 - 7 && gap <= 300 + 7);
        }
        prevOn = e.onMs;
    }
}

static void testFlashIntervalsAreIrregular() {
    TestPattern tp;
    CHECK(tp.flash(255, 255, 255, 20, 100, 50, 123456));
    runFlashes(tp, 123456, 1, 123456 + 50 * 160 + 500);
    CHECK(tp.recorded() == 50);
    int distinct = 0;
    for (int i = 2; i < tp.recorded(); i++) {
        uint32_t a = tp.edge(i).onMs - tp.edge(i - 1).onMs;
        uint32_t b = tp.edge(i - 1).onMs - tp.edge(i - 2).onMs;
        distinct += a != b;
    }
    CHECK(distinct > 30);  // not a fixed period that could phase-lock to the camera
}

static void testFlashSurvivesMillisWrap() {
    TestPattern tp;
    const uint32_t start = 0xFFFFFF00u;  // wraps during the train
    CHECK(tp.flash(255, 255, 255, 30, 100, 5, start));
    PixelMatrix m(2, 2);
    uint32_t t = start;
    for (int i = 0; i < 2000; i++, t += 1) {
        tp.update(t);
        tp.shown(t);
    }
    CHECK(tp.done());
    CHECK(tp.recorded() == 5);
    CHECK((uint32_t)(tp.edge(4).offMs - tp.edge(4).onMs) >= 30);
}

static void testOffClearsAFlashTrain() {
    TestPattern tp;
    CHECK(tp.flash(1, 2, 3, 10, 50, 5, 0));
    runFlashes(tp, 0, 1, 120);
    CHECK(tp.recorded() >= 1);
    tp.off();
    CHECK(!tp.active() && tp.recorded() == 0 && !tp.done());
}

int main() {
    testOffSolidPixel();
    testFlashRefusesBadParameters();
    testFlashScheduleAndStamps();
    testFlashIntervalsAreIrregular();
    testFlashSurvivesMillisWrap();
    testOffClearsAFlashTrain();
    std::printf("%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
