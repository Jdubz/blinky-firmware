/**
 * FixedFloat unit tests (inputs/FixedFloat.h) - native C++, no Arduino.
 *
 * blinky-firmware#23: the stream's floats are formatted without printf, and
 * the bytes must not change (blinky-server and the tools parse them). Every
 * case is compared with the host's own snprintf("%.*f", d, (double)x):
 * exact ties at every precision (round half to even), signed zero and tiny
 * negatives, the edges of the fast path, and millions of random floats.
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../blinky-things/inputs/FixedFloat.h"

static long g_total = 0;
static long g_failed = 0;

static void same(float x, uint8_t d) {
    char want[64];
    std::snprintf(want, sizeof(want), "%.*f", d, static_cast<double>(x));
    char got[FixedFloat::BUF_SIZE];
    const size_t n = FixedFloat::format(got, x, d);
    g_total++;
    if (n == 0) return;  // outside the fast path: the caller falls back to printf
    if (std::strcmp(got, want) != 0 || n != std::strlen(want)) {
        if (g_failed++ < 20) std::printf("FAIL %.9g d=%u: got '%s' want '%s'\n", static_cast<double>(x), d, got, want);
    }
}

static bool fast(float x, uint8_t d) {
    char b[FixedFloat::BUF_SIZE];
    return FixedFloat::format(b, x, d) != 0;
}

int main() {
    // Exact ties (k + 0.5) * 10^-d where representable: k/2^m values.
    for (int d = 0; d <= 6; d++) {
        for (int k = -4096; k <= 4096; k++) {
            same(k / 8.0f, static_cast<uint8_t>(d));
            same(k / 64.0f, static_cast<uint8_t>(d));
            same(k / 1024.0f, static_cast<uint8_t>(d));
            same(k * 0.001f, static_cast<uint8_t>(d));
            same(k * 0.1f, static_cast<uint8_t>(d));
        }
    }
    const float specials[] = {0.0f, -0.0f, 1e-9f, -1e-9f, -0.0004f, -0.004f, 0.005f, 0.015f, 0.125f, 0.375f,
                              2.5f, 3.5f, -2.5f, 999999.94f, 1e8f, 999999872.0f, 123456.789f, 0.99999994f,
                              1.0f, -1.0f, 9.995f, 0.045f, 1.0e-30f, 1.17549435e-38f, 1.4e-45f};
    for (float x : specials)
        for (int d = 0; d <= 6; d++) same(x, static_cast<uint8_t>(d));
    // Random bit patterns, finite and inside the fast path, every precision.
    uint32_t s = 0x2026B23u;
    long fastCount = 0;
    for (long i = 0; i < 3000000; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        float x;
        std::memcpy(&x, &s, 4);
        const uint8_t d = static_cast<uint8_t>(i % 7);
        if (!std::isfinite(x)) continue;
        if (fast(x, d)) fastCount++;
        same(x, d);
    }
    // Values the stream actually prints: 0..1 levels, dB, BPM, centroid.
    for (long i = 0; i < 2000000; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const float x = (s & 0xFFFFFF) / 16777216.0f * ((i & 3) == 0 ? 1.0f : (i & 3) == 1 ? 200.0f : (i & 3) == 2 ? -90.0f : 0.01f);
        same(x, static_cast<uint8_t>(1 + i % 5));
    }
    // The fast path's edges: refused, never wrong.
    g_total++;
    if (fast(NAN, 2) || fast(INFINITY, 2) || fast(-INFINITY, 2) || fast(1e9f, 2) || fast(1.0f, 7)) {
        g_failed++;
        std::printf("FAIL: the fast path took a value it must refuse\n");
    }
    g_total++;
    if (!fast(999999872.0f, 6) || !fast(0.5f, 0)) {
        g_failed++;
        std::printf("FAIL: the fast path refused a value it covers\n");
    }
    std::printf("%ld/%ld checks passed (random fast-path hits %ld)\n", g_total - g_failed, g_total, fastCount);
    return g_failed == 0 ? 0 : 1;
}
