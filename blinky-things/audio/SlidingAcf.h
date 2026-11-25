#pragma once

#include <stdint.h>
#include <string.h>

/**
 * SlidingAcf - exact running autocorrelation over a sliding window.
 *
 * The tempo ACF (AudioTracker) reads the mean-subtracted lag products of the
 * last WINDOW frames of a rhythm source,
 *
 *     C[l] = sum_{i=0}^{N-l-1} (x_i - mu) (x_{i+l} - mu),   mu = window mean,
 *
 * at a few dozen lags per decision. Recomputing them from scratch every
 * decision cost ~88k multiply-adds (17-19 ms on the nRF52840). This keeps the
 * raw lag products P[l] = sum x_i x_{i+l} up to date instead, one sample at a
 * time: the online sliding-window ACF of madmom (Boeck et al.,
 * madmom/features/tempo.py, online mode) and BTrack (Stark, Davies &
 * Plumbley, DAFx 2009). Each new sample adds its products with the MAX_LAG
 * samples before it and, once the window is full, the sample leaving the
 * window takes its products back out: O(MAX_LAG) per sample. C[l] then
 * follows exactly from P[l] and two range sums:
 *
 *     C[l] = P[l] - mu (A_l + B_l) + (N - l) mu^2,
 *     A_l = sum_{i<N-l} x_i = S - tail(l),  B_l = sum_{i>=l} x_i = S - head(l).
 *
 * Exact, not drift-corrected: samples are quantized to Q24 integers and the
 * sums are int64, so adding and removing a product is exact and the running
 * table equals a from-scratch sum bit for bit forever (a float table would
 * drift and need periodic recomputation). Q24 resolves 6e-8, a float's
 * resolution at 1.0. Bound: WINDOW * (2^24)^2 < 2^63 for WINDOW < 2^15.
 * The final formula runs in double (C[l] is a small difference of large
 * terms when the source has a DC level, e.g. mic level).
 *
 * Deferred updates: push() only appends to the ring; applyPending() advances
 * the table. A caller can hold the table at one window (AudioTracker holds it
 * at the analysis snapshot while the analysis is spread over frames) for up
 * to SLACK pushes, then catch up.
 *
 * RAM: (WINDOW + SLACK) * 4 + (MAX_LAG + 1) * 8 bytes.
 */
template <int WINDOW, int MAX_LAG, int SLACK>
class SlidingAcf {
public:
    static_assert(MAX_LAG >= 0 && MAX_LAG < WINDOW, "lags must fit in the window");
    static_assert(WINDOW <= 32768, "int64 lag sums: WINDOW * 2^48 must stay below 2^63");
    static_assert(SLACK >= 1, "need room for at least one pending sample");

    static constexpr int RING = WINDOW + SLACK;
    static constexpr float Q_SCALE = 16777216.0f;              // 2^24
    static constexpr float INV_Q2F = 1.0f / 281474976710656.0f;  // 2^-48

    // Quantize a source value. The rhythm sources are all in [0, 1] (bass
    // flux contrast and mic level are clamped there, whitened mel energy is
    // a ratio to its running max); the caller asserts that before pushing.
    static int32_t quantize(float x) { return static_cast<int32_t>(x * Q_SCALE + 0.5f); }

    // Append one quantized sample. The table does not move until
    // applyPending(). Returns false (sample dropped) if SLACK samples are
    // already pending: the caller held the table too long, a bug.
    bool push(int32_t q) {
        if (pending_ >= SLACK) return false;
        int slot = applySlot_ + pending_;
        if (slot >= RING) slot -= RING;
        ring_[slot] = q;
        pending_++;
        return true;
    }

    int pending() const { return pending_; }

    // Advance the table by up to maxSamples pending samples (all if < 0).
    void applyPending(int maxSamples = -1) {
        while (pending_ > 0 && maxSamples != 0) {
            applyOne();
            if (maxSamples > 0) maxSamples--;
        }
    }

    // Samples in the table's window.
    int count() const { return count_; }

    // Sum of the window, Q24 units.
    int64_t sum() const { return sum_; }

    // Raw lag product sum P[l], Q24^2 units (tests).
    int64_t rawLag(int l) const { return lag_[l]; }

    // head[l] = sum of the first l samples of the window, tail[l] = sum of the
    // last l, for l = 0..maxLag (Q24 units). maxLag <= min(MAX_LAG, count()).
    // Returns sum / count^2, centered()'s other per-window input.
    double prefixSums(int64_t* head, int64_t* tail, int maxLag) const {
        int h = applySlot_ - count_;          // oldest sample of the window
        if (h < 0) h += RING;
        int t = applySlot_;                   // one past the newest
        head[0] = 0;
        tail[0] = 0;
        for (int l = 1; l <= maxLag; l++) {
            head[l] = head[l - 1] + ring_[h];
            if (++h == RING) h = 0;
            if (--t < 0) t = RING - 1;
            tail[l] = tail[l - 1] + ring_[t];
        }
        const double n = static_cast<double>(count_);
        return static_cast<double>(sum_) / (n * n);
    }

    // C[l] in source units^2, from prefixSums() of the same window state
    // (head, tail and its return value). 0 <= l < count(), l <= MAX_LAG.
    float centered(int l, const int64_t* head, const int64_t* tail, double sumOverN2) const {
        const int64_t n = count_;
        const int64_t pairs = n - l;
        // C = P - (S / n^2) D with D = n (A + B) - pairs * S; D is exact in
        // int64 (|D| < 2^45), only the last product and difference are double
        // (soft-float on the M4: two int64->double, a multiply, a subtract).
        const int64_t ab = 2 * sum_ - head[l] - tail[l];
        const int64_t d = n * ab - pairs * sum_;
        const double c = static_cast<double>(lag_[l]) - sumOverN2 * static_cast<double>(d);
        return static_cast<float>(c) * INV_Q2F;  // exact power-of-two scaling
    }

    void reset() {
        memset(ring_, 0, sizeof(ring_));
        memset(lag_, 0, sizeof(lag_));
        sum_ = 0;
        applySlot_ = pending_ = count_ = 0;
    }

private:
    void applyOne() {
        const int s = applySlot_;                 // slot of the sample entering
        const int32_t q = ring_[s];
        const bool full = (count_ == WINDOW);
        // Slot of the sample leaving, WINDOW samples back. Still intact: at
        // most SLACK samples are pushed past it and RING = WINDOW + SLACK.
        int so = s - WINDOW;
        if (so < 0) so += RING;
        const int32_t old = full ? ring_[so] : 0;

        // Lag 0, then lags 1..maxL: the new sample with the ones before it
        // and the leaving sample with the ones after it (all inside the
        // window, MAX_LAG < WINDOW). Before the window fills, only new pairs.
        // Contiguous runs, no modulo per element (at most 3 runs).
        lag_[0] += static_cast<int64_t>(q) * q - static_cast<int64_t>(old) * old;
        const int maxL = full ? MAX_LAG : (count_ < MAX_LAG ? count_ : MAX_LAG);
        int pn = s - 1;                           // partner of the new sample at lag l, walking down
        if (pn < 0) pn += RING;
        int po = so + 1;                          // partner of the leaving sample at lag l, walking up
        if (po >= RING) po -= RING;
        for (int l = 1; l <= maxL;) {
            int run = maxL - l + 1;
            if (pn + 1 < run) run = pn + 1;
            if (full && RING - po < run) run = RING - po;
            if (full) {
                macRun(&lag_[l], &ring_[pn], &ring_[po], q, -old, run);
            } else {
                // Nothing leaves yet (nold = 0); xo must still be `run` valid
                // elements: ring_[pn + 1 - run .. pn] (run <= pn + 1 above).
                macRun(&lag_[l], &ring_[pn], &ring_[pn + 1 - run], q, 0, run);
            }
            l += run;
            pn -= run;
            if (pn < 0) pn += RING;
            po += run;
            if (po >= RING) po -= RING;
        }
        sum_ += static_cast<int64_t>(q) - old;
        if (!full) count_++;
        if (++applySlot_ == RING) applySlot_ = 0;
        pending_--;
    }

    // acc[k] += q * xn[-k] + nold * xo[k]. Written as two separate 32x32->64
    // multiply-adds so GCC 9 (-Ofast, Cortex-M4) emits two SMLALs per lag
    // (~14 cycles with the int64 load/store); `a * b - c * d` in one
    // expression compiled to a generic 64x64 multiply with stack spills, and so
    // did this loop once inlined into applyOne (q spilled as an int64): keep
    // it out of line (one call per contiguous run, at most 3 per sample).
    __attribute__((noinline))
    static void macRun(int64_t* acc, const int32_t* xn, const int32_t* xo, int32_t q, int32_t nold, int run) {
        for (int k = 0; k < run; k++) {
            const int32_t a = xn[-k];
            const int32_t b = xo[k];
            int64_t v = acc[k];
            v += static_cast<int64_t>(q) * a;
            v += static_cast<int64_t>(nold) * b;
            acc[k] = v;
        }
    }

    int32_t ring_[RING] = {};
    int64_t lag_[MAX_LAG + 1] = {};
    int64_t sum_ = 0;
    int applySlot_ = 0;   // slot of the next sample the table takes
    int pending_ = 0;     // samples pushed but not yet applied (<= SLACK)
    int count_ = 0;       // samples in the window (<= WINDOW)
};
