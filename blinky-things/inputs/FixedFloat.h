#pragma once

#include <string.h>
#include <stddef.h>
#include <stdint.h>

/**
 * FixedFloat - format a float exactly as printf("%.*f", digits, (double)x)
 * does, without printf (blinky-firmware#23).
 *
 * Why: Arduino's Print::print(float, digits) on this core is
 * snprintf("%.*f"), and newlib's %f converts through _dtoa_r: software
 * double arithmetic and multi-precision (Bigint) steps, per value. A 50-band
 * NN stream line prints ~60 floats, ~60 times a second, on the loop task.
 *
 * How, and why it is byte-identical: a float has a 24-bit significand, and
 * 10^d = 2^d * 5^d with 5^d < 2^14 for d <= 6, so (double)x * 10^d needs at
 * most 38 significant bits and is computed EXACTLY in double (53 bits). Its
 * integer and fractional parts are then exact too, so rounding to the
 * nearest integer, ties to even, is the correctly rounded result that a
 * correctly rounding printf prints (C11 7.21.6.1: the value is rounded to
 * the precision; newlib's and glibc's conversions round exactly, ties to
 * even, under the default rounding mode). The sign is printed whenever the
 * sign bit is set, as printf does ("-0.00" for -0.0 or -0.001).
 *
 * Covers finite |x| < 1e9 with digits <= 6; anything else (NaN and Inf
 * included, which printf spells "nan"/"inf") returns 0 and the
 * caller falls back to Print's own formatting. tests/unit/test_fixed_float.cpp
 * compares it with the host's printf (at -O1 and -Ofast). On the test chip a
 * temporary self-test compared it with the firmware's own newlib printf:
 * 8056 values, 0 mismatches; 13.7 us per value against printf's 84 us.
 */
namespace FixedFloat {

static constexpr uint8_t MAX_DIGITS = 6;
static constexpr size_t BUF_SIZE = 20;  // '-' + 10 integer digits + '.' + 6 + NUL

// Writes the text into buf (BUF_SIZE bytes, NUL-terminated) and returns its
// length, or 0 when x or digits is outside the fast path.
inline size_t format(char* buf, float x, uint8_t digits) {
    static const double P10[MAX_DIGITS + 1] = {1.0, 10.0, 100.0, 1e3, 1e4, 1e5, 1e6};
    // Sign, NaN and Inf from the bits: the core builds with -Ofast, whose
    // -ffinite-math-only lets the compiler fold isfinite() to true and
    // -fno-signed-zeros lets it drop the sign of -0.0.
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    if (digits > MAX_DIGITS || (bits & 0x7F800000u) == 0x7F800000u) return 0;  // NaN/Inf: printf's path
    const bool neg = (bits & 0x80000000u) != 0;
    bits &= 0x7FFFFFFFu;
    float ax;
    memcpy(&ax, &bits, sizeof(ax));
    if (bits >= 0x4E6E6B28u) return 0;  // |x| >= 1e9 (0x4E6E6B28 is 1e9f)
    const double a = static_cast<double>(ax);
    const double p = a * P10[digits];         // exact (see above)
    uint64_t q = static_cast<uint64_t>(p);     // floor: p >= 0
    const double frac = p - static_cast<double>(q);  // exact
    if (frac > 0.5 || (frac == 0.5 && (q & 1u))) q++;

    char rev[BUF_SIZE];
    size_t n = 0;
    if (q <= 0xFFFFFFFFu) {  // 32-bit division: no __aeabi_uldivmod
        uint32_t v = static_cast<uint32_t>(q);
        do { rev[n++] = static_cast<char>('0' + v % 10u); v /= 10u; } while (v);
    } else {
        do { rev[n++] = static_cast<char>('0' + q % 10u); q /= 10u; } while (q);
    }
    while (n < static_cast<size_t>(digits) + 1) rev[n++] = '0';

    size_t len = 0;
    if (neg) buf[len++] = '-';
    for (size_t i = n; i > digits; i--) buf[len++] = rev[i - 1];
    if (digits > 0) {
        buf[len++] = '.';
        for (size_t i = digits; i > 0; i--) buf[len++] = rev[i - 1];
    }
    buf[len] = '\0';
    return len;
}

}  // namespace FixedFloat
