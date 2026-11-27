#pragma once

#include <Arduino.h>

/**
 * LoopMetrics — main-loop fps + frame-time observability.
 *
 * Always-on counters (cheap: 3 comparisons + 1 increment per loop tick).
 * The previous serial-print fps tracker was log-level-gated which made
 * production fleets invisible. v36-fmax (#136) needs measured on-device fps
 * under typical music + LED load as a hard merge gate (≥30 fps), so the
 * data has to be queryable from any operator station, not just from devices
 * that happen to have INFO logging on.
 *
 * Window: rolling 5 s. Each closed window's fps and frame-ms range are
 * what the getters return until the next window closes. The window in
 * progress is invisible to readers — that keeps results stable from one
 * `json info` call to the next regardless of operator timing.
 *
 * `json info` also reports when that window closed (frameWindowEndMs, uptime
 * millis()) and how long it ran (frameWindowMs: at least WINDOW_MS, longer by
 * any stall in it). With the reply's own `millis` the host places the window
 * exactly, e.g. to tell whether one of its settings saves (a known flash-erase
 * stall, #49) fell inside it.
 */
namespace LoopMetrics {

constexpr uint32_t WINDOW_MS = 5000;

struct State {
    // Window in progress
    uint32_t windowStart  = 0;
    uint32_t frameCount   = 0;
    uint32_t minFrameMs   = UINT32_MAX;
    uint32_t maxFrameMs   = 0;
    // Last closed window (the public reading)
    float    lastFps      = 0.0f;
    uint32_t lastMinMs    = 0;
    uint32_t lastMaxMs    = 0;
    uint32_t lastWindowMs = 0;  // monotonic timestamp of the last closed window
    uint32_t lastWindowLenMs = 0;  // how long the last closed window ran (>= WINDOW_MS)
    uint32_t prevTickMs   = 0;
    // LED hand-off cost (#14): loop time inside leds->show(), this window and the last closed one
    uint32_t showSumUs    = 0;
    uint32_t showCount    = 0;
    uint32_t showMaxUs    = 0;
    uint32_t lastShowUs   = 0;  // mean per show() call
    uint32_t lastShowMaxUs = 0;
};

inline State& state() {
    static State s;
    return s;
}

/// Call once per main-loop iteration with the current millis().
/// Closes a window every WINDOW_MS and updates the public readings.
inline void tick(uint32_t now) {
    State& s = state();
    if (s.prevTickMs != 0) {
        uint32_t dt = now - s.prevTickMs;
        if (dt < s.minFrameMs) s.minFrameMs = dt;
        if (dt > s.maxFrameMs) s.maxFrameMs = dt;
    }
    s.prevTickMs = now;
    s.frameCount++;

    // windowStart==0 is the "not-started" sentinel. millis()==0 is
    // theoretically possible at boot — if both are 0 on the very first
    // tick, this branch fires again on tick 2 with the same effect (the
    // window starts from tick 2's timestamp instead of tick 1's, error
    // ≈18 ms at 55 fps). Harmless; no special-case needed.
    if (s.windowStart == 0) {
        s.windowStart = now;
    } else if (now - s.windowStart >= WINDOW_MS) {
        // elapsed is unsigned modular arithmetic. millis() wraps at
        // 2^32 ms ≈ 49.7 days; when now wraps past zero mid-window,
        // (now - s.windowStart) underflows to a large value (close to
        // 4.29 GB ms), the branch fires immediately, and we'd report
        // a spuriously low fps for that one window. Detect that case
        // by an elapsed bound far above any plausible window
        // (16×WINDOW_MS = 80 s; real window closes in ~5 s under
        // normal load and at most ~10 s under heavy stalls per the
        // drain-loop ceiling) and reset cleanly without publishing
        // the bad reading.
        //
        // Stall vs wrap ambiguity: if the loop is genuinely stuck for
        // >80 s (deadlock, power glitch, hard fault), this branch also
        // fires and discards. That's the right call either way — a
        // stalled-then-recovered window's fps would be more misleading
        // than the prior good reading. The operator sees lastFps stay
        // pinned at the last good value, which on its own signals
        // something's wrong (no fps movement = main loop not running).
        // Per PR 138 rounds 9 + 12 review.
        uint32_t elapsed = now - s.windowStart;
        if (elapsed > 16U * WINDOW_MS) {
            // Wrap or extreme stall — discard this window's data,
            // start fresh. Public readings remain at last good window.
            //
            // Reset prevTickMs alongside the rest so the first dt sample
            // of the new window is *not* `now - <pre-wrap stale value>`,
            // which would inject a multi-day or multi-second garbage
            // value into minFrameMs/maxFrameMs and stay sticky for the
            // whole new window. Per PR 138 round-14 review (MEDIUM #6).
            s.windowStart = now;
            s.frameCount  = 0;
            s.minFrameMs  = UINT32_MAX;
            s.maxFrameMs  = 0;
            s.prevTickMs  = 0;
            s.showSumUs   = 0;
            s.showCount   = 0;
            s.showMaxUs   = 0;
            return;
        }
        s.lastFps      = s.frameCount * 1000.0f / elapsed;
        // minFrameMs == UINT32_MAX means no dt was sampled this window
        // (only the very first window's first tick, before prevTickMs was
        // set, can produce this). 0 is the correct reading there — it's
        // not a masked failure, just a "no data yet" sentinel. Documented
        // per CLAUDE.md No-Silent-Fallbacks principled-exception rule.
        s.lastMinMs    = (s.minFrameMs == UINT32_MAX) ? 0 : s.minFrameMs;
        s.lastMaxMs    = s.maxFrameMs;
        s.lastWindowMs = now;
        s.lastWindowLenMs = elapsed;
        s.lastShowUs   = s.showCount ? s.showSumUs / s.showCount : 0;  // 0 = no show() (safe mode)
        s.lastShowMaxUs = s.showMaxUs;
        s.windowStart  = now;
        s.frameCount   = 0;
        s.minFrameMs   = UINT32_MAX;
        s.maxFrameMs   = 0;
        s.showSumUs    = 0;
        s.showCount    = 0;
        s.showMaxUs    = 0;
    }
}

/// Loop task: time one leds->show() took, in micros(). On the nRF52840 core micros() is the
/// FreeRTOS tick (1024 Hz, ~977 us steps) unless the DWT is enabled, so the mean is a dithered
/// estimate and the max is quantized to ~1 ms, like the acf timers.
inline void noteShowUs(uint32_t us) {
    State& s = state();
    s.showSumUs += us;
    s.showCount++;
    if (us > s.showMaxUs) s.showMaxUs = us;
}

inline float    getFps()           { return state().lastFps; }
inline uint32_t getMinFrameMs()    { return state().lastMinMs; }
inline uint32_t getMaxFrameMs()    { return state().lastMaxMs; }
// 0 = no window has closed yet (boot edge case). Callers using this value
// as a "new window" sentinel (e.g., once-per-window logging in blinky-
// things.ino) must explicitly handle the 0 case to avoid a spurious
// initial fire. The 49.7-day wrap-discard branch in tick() does NOT reset
// this to 0 — only the boot-init path sees 0.
inline uint32_t getLastWindowMs()  { return state().lastWindowMs; }
// Length of the last closed window in ms (0 until one has closed).
inline uint32_t getLastWindowLenMs() { return state().lastWindowLenMs; }
// Loop time inside leds->show() over the last closed window: mean per call and max, in us.
inline uint32_t getShowUs()        { return state().lastShowUs; }
inline uint32_t getShowMaxUs()     { return state().lastShowMaxUs; }

}  // namespace LoopMetrics
