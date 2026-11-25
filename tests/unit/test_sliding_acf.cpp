/**
 * SlidingAcf unit tests (blinky-things/audio/SlidingAcf.h) — native C++.
 *
 * The incremental ACF must give what the tempo ACF used to recompute from
 * scratch every decision (AudioTracker::runAcf before the incremental
 * tables): mean-subtracted lag products over the window, at every lag the
 * tracker reads (0 .. OSS_BUFFER_SIZE/2 + 8). Checked here:
 *
 *   1. Exactness: after any number of pushes, filling and wrapping, held and
 *      caught up, the running int64 table equals a from-scratch int64 sum of
 *      the same quantized window, bit for bit (no drift to correct).
 *   2. Accuracy against the old float batch code (float mean, mean-subtracted
 *      in place, one float accumulator: the reference copy below) and against
 *      a double-precision batch on the same float inputs, for flux-like
 *      sparse signals, DC-heavy level-like signals and white noise. Errors are
 *      relative to the zero-lag energy (the ACF's own scale: pass 1 divides by
 *      it), over long runs.
 *
 * Compile and run (CI: pr-validation.yml, "Tracker Harness"):
 *   g++ -std=c++17 -O2 -I blinky-things tests/unit/test_sliding_acf.cpp -o build/test_sliding_acf
 *   build/test_sliding_acf
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "audio/SlidingAcf.h"

static constexpr int WINDOW = 792;           // AudioTracker::OSS_BUFFER_SIZE
static constexpr int MAX_LAG = WINDOW / 2 + 8;
static constexpr int SLACK = 16;
using Acf = SlidingAcf<WINDOW, MAX_LAG, SLACK>;

static int g_failed = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { g_failed++; std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// The old batch code (AudioTracker::runAcf before this change), for one lag:
// linearize, float mean, subtract in place, one float accumulator.
static void oldBatch(const std::vector<float>& win, std::vector<float>& out, int maxLag) {
    const int count = static_cast<int>(win.size());
    std::vector<float> buf(win);
    float mean = 0.0f;
    for (int i = 0; i < count; i++) mean += buf[i];
    mean /= count;
    for (int i = 0; i < count; i++) buf[i] -= mean;
    out.assign(maxLag + 1, 0.0f);
    for (int lag = 0; lag <= maxLag && lag < count; lag++) {
        float sum = 0.0f;
        for (int i = 0; i < count - lag; i++) sum += buf[i] * buf[i + lag];
        out[lag] = sum;
    }
}

// Double-precision batch on the same float inputs (the value both estimate).
static void doubleBatch(const std::vector<float>& win, std::vector<double>& out, int maxLag) {
    const int count = static_cast<int>(win.size());
    double mean = 0.0;
    for (float v : win) mean += v;
    mean /= count;
    out.assign(maxLag + 1, 0.0);
    for (int lag = 0; lag <= maxLag && lag < count; lag++) {
        double sum = 0.0;
        for (int i = 0; i < count - lag; i++) sum += (win[i] - mean) * (win[i + lag] - mean);
        out[lag] = sum;
    }
}

struct Stats {
    double maxNew = 0, maxOld = 0;   // max |C - Cdouble| / Cdouble[0]
    double sumNew = 0, sumOld = 0;
    long n = 0;
};

enum Kind { FLUX, LEVEL, NOISE };

static float gen(Kind k, std::mt19937& rng, long t) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    switch (k) {
        case FLUX: {  // half-wave rectified kicks every ~29.3 frames plus sparse hats, clamped [0,1]
            const float ph = std::fmod(t / 29.3f, 1.0f);
            float v = (ph < 0.04f ? 0.6f : 0.0f) + (u(rng) < 0.08f ? 0.3f * u(rng) : 0.0f) + 0.01f * u(rng);
            return v > 1.0f ? 1.0f : v;
        }
        case LEVEL:   // DC-heavy: 0.3 +- 0.03, slow wobble (mic level / bass energy)
            return 0.3f + 0.02f * std::sin(t * 0.21f) + 0.01f * u(rng);
        case NOISE:
            return u(rng);
    }
    return 0.0f;
}

static void runKind(Kind kind, const char* name, long frames, unsigned seed) {
    std::mt19937 rng(seed);
    Acf acf;
    std::vector<float> hist;  // every pushed float, for the batch references
    std::vector<int64_t> head(MAX_LAG + 1), tail(MAX_LAG + 1);
    std::vector<float> oldC;
    std::vector<double> dblC;
    Stats st;
    bool exactAll = true;
    long checks = 0;
    for (long t = 0; t < frames; t++) {
        const float x = gen(kind, rng, t);
        hist.push_back(x);
        EXPECT(acf.push(Acf::quantize(x)), "push refused at t=%ld", t);
        // Hold the table for up to 5 pushes now and then (as AudioTracker does
        // while an analysis is spread over frames), else apply each push.
        const bool hold = (t % 97) < 5;
        if (!hold) acf.applyPending(t % 3 == 0 ? 2 : -1);
        // Check at the fill edge, early wraps, and every 211 frames.
        const bool fillEdge = acf.count() >= WINDOW - 1 && t < WINDOW + 20;  // filling, then the first wraps
        const bool check = (acf.pending() == 0) && (fillEdge || t % 211 == 0);
        if (!check || acf.count() < 2) continue;
        checks++;
        const int n = acf.count();
        std::vector<float> win(hist.end() - n, hist.end());
        const int maxLag = std::min(MAX_LAG, n - 1);

        // 1. Exactness of the running table vs a from-scratch int64 sum.
        for (int l = 0; l <= maxLag; l++) {
            int64_t ref = 0;
            for (int i = 0; i + l < n; i++)
                ref += static_cast<int64_t>(Acf::quantize(win[i])) * Acf::quantize(win[i + l]);
            if (ref != acf.rawLag(l)) {
                exactAll = false;
                EXPECT(false, "%s t=%ld lag %d: running %lld != batch %lld", name, t, l,
                       static_cast<long long>(acf.rawLag(l)), static_cast<long long>(ref));
                break;
            }
        }

        // 2. Accuracy vs the old float batch and a double batch.
        const double sumOverN2 = acf.prefixSums(head.data(), tail.data(), maxLag);
        oldBatch(win, oldC, maxLag);
        doubleBatch(win, dblC, maxLag);
        const double scale = dblC[0];
        if (scale <= 0) continue;
        for (int l = 0; l <= maxLag; l++) {
            const double eNew = std::fabs(acf.centered(l, head.data(), tail.data(), sumOverN2) - dblC[l]) / scale;
            const double eOld = std::fabs(oldC[l] - dblC[l]) / scale;
            st.maxNew = std::max(st.maxNew, eNew);
            st.maxOld = std::max(st.maxOld, eOld);
            st.sumNew += eNew;
            st.sumOld += eOld;
            st.n++;
        }
    }
    std::printf("%-6s %7ld frames %4ld checks  exact int64 table: %s  |C-Cdbl|/C0: new mean %.2e max %.2e"
                "   old float batch mean %.2e max %.2e\n",
                name, frames, checks, exactAll ? "yes" : "NO", st.sumNew / st.n, st.maxNew,
                st.sumOld / st.n, st.maxOld);
    // The incremental table must be at least as accurate as the code it
    // replaces, and far below anything a threshold (0.05 peaks, 1e-10 floors)
    // could notice.
    EXPECT(st.maxNew < 1e-5, "%s: max error %.3g", name, st.maxNew);
    EXPECT(st.maxNew <= st.maxOld * 1.01 + 1e-7, "%s: new %.3g worse than old float batch %.3g",
           name, st.maxNew, st.maxOld);
}

int main() {
    std::printf("SlidingAcf<%d, %d, %d>: %zu bytes per source\n", WINDOW, MAX_LAG, SLACK, sizeof(Acf));
    runKind(FLUX, "flux", 20000, 1);
    runKind(LEVEL, "level", 20000, 2);
    runKind(NOISE, "noise", 20000, 3);

    // Refusal: SLACK pushes held, the next one is refused (caller bug, loud).
    {
        Acf a;
        for (int i = 0; i < SLACK; i++) EXPECT(a.push(Acf::quantize(0.5f)), "push %d", i);
        EXPECT(!a.push(Acf::quantize(0.5f)), "push past SLACK must be refused");
        a.applyPending(1);
        EXPECT(a.push(Acf::quantize(0.5f)), "push after applying one");
        EXPECT(a.pending() == SLACK, "pending %d", a.pending());
    }
    if (g_failed) {
        std::printf("%d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("all SlidingAcf checks passed\n");
    return 0;
}
