/**
 * LedBrightness unit tests (render/LedBrightness.h) - native C++, no Arduino.
 *
 * The test box's runtime brightness override (the `leds` command): the argument parser
 * refuses anything it doesn't name, the override reaches the strip, `config` returns to the
 * device config's brightness, and nothing changes without a command (the fleet's path).
 */
#include <cstdint>
#include <cstdio>

#include "../../blinky-things/hal/mock/MockLedStrip.h"
#include "../../blinky-things/render/LedBrightness.h"

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

using A = LedBrightness::Action;

static bool parsesTo(const char* args, A action, uint8_t value = 0) {
    LedBrightness::Parsed p = LedBrightness::parse(args);
    return p.action == action && (action != A::SET || p.value == value);
}

static void testParse() {
    CHECK(parsesTo("", A::STATUS));
    CHECK(parsesTo(" status", A::STATUS));
    CHECK(parsesTo("   ", A::STATUS));
    CHECK(parsesTo(" off", A::SET, 0));
    CHECK(parsesTo(" config", A::CONFIG));
    CHECK(parsesTo(" 0", A::SET, 0));
    CHECK(parsesTo(" 40", A::SET, 40));
    CHECK(parsesTo(" 255", A::SET, 255));
    CHECK(parsesTo(" 040 ", A::SET, 40));
    // Refused, never coerced.
    CHECK(parsesTo(" 256", A::INVALID));
    CHECK(parsesTo(" 1000000", A::INVALID));
    CHECK(parsesTo(" -1", A::INVALID));
    CHECK(parsesTo(" 4x", A::INVALID));
    CHECK(parsesTo(" on", A::INVALID));
    CHECK(parsesTo(" off 40", A::INVALID));
    CHECK(parsesTo(" 40 40", A::INVALID));
    CHECK(parsesTo(" statusx", A::INVALID));
    CHECK(parsesTo(" 99999999999999999999", A::INVALID));
    CHECK(parsesTo(nullptr, A::INVALID));
}

static void testOverrideAndConfig() {
    MockLedStrip strip(64);
    strip.setBrightness(0);  // boot: the test chip's device config (brightness 0)
    LedBrightness lb;
    CHECK(!lb.attached());
    lb.begin(&strip);
    CHECK(lb.attached());
    // No command: the boot brightness stands, no override (the fleet's path).
    CHECK(!lb.overridden());
    CHECK(lb.current() == 0);

    lb.set(40);
    CHECK(lb.overridden());
    CHECK(strip.getBrightness() == 40 && lb.current() == 40);

    lb.set(0);  // `leds off`: still an override, dark
    CHECK(lb.overridden());
    CHECK(lb.current() == 0);

    lb.config(0);
    CHECK(!lb.overridden());
    CHECK(lb.current() == 0);
}

static void testConfigRestoresAFleetBrightness() {
    MockLedStrip strip(89);
    strip.setBrightness(100);
    LedBrightness lb;
    lb.begin(&strip);
    lb.set(0);
    CHECK(lb.current() == 0);
    lb.config(100);
    CHECK(!lb.overridden());
    CHECK(strip.getBrightness() == 100);
}

int main() {
    testParse();
    testOverrideAndConfig();
    testConfigRestoresAFleetBrightness();
    std::printf("%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
