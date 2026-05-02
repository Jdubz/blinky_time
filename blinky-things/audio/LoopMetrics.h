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
    uint32_t prevTickMs   = 0;
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
        // the bad reading. Per PR 138 round-9 review (HIGH).
        uint32_t elapsed = now - s.windowStart;
        if (elapsed > 16U * WINDOW_MS) {
            // Wrap or extreme stall — discard this window's data,
            // start fresh. Public readings remain at last good window.
            s.windowStart = now;
            s.frameCount  = 0;
            s.minFrameMs  = UINT32_MAX;
            s.maxFrameMs  = 0;
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
        s.windowStart  = now;
        s.frameCount   = 0;
        s.minFrameMs   = UINT32_MAX;
        s.maxFrameMs   = 0;
    }
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

}  // namespace LoopMetrics
