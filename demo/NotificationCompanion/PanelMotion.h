#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

namespace companion {
inline double motion_now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Fluent baseline curves: cubic-bezier(0,0,0,1) on entry and (1,0,1,1)
// on exit. Solve x(t) analytically instead of approximating with linear motion.
inline double fluent_ease(double progress, bool entering) {
    double x = std::clamp(progress, 0.0, 1.0);
    double t = entering ? std::cbrt(x) : 1.0 - std::cbrt(1.0 - x);
    return t * t * (3.0 - 2.0 * t);
}
enum class MotionPhase { Hidden, Entering, Visible, Exiting };
struct PanelMotion {
    MotionPhase phase = MotionPhase::Hidden;
    double from = 0, to = 0, started = 0, duration = 0;
    uint64_t generation = 0;
    static constexpr unsigned enter_ms = 250, exit_ms = 167;
    bool moving() const { return phase == MotionPhase::Entering || phase == MotionPhase::Exiting; }
    bool wants_visible() const { return phase == MotionPhase::Entering || phase == MotionPhase::Visible; }
    double value(double now) const {
        if (!moving() || duration <= 0) return to;
        return from + (to - from) * fluent_ease((now - started) / duration, to > from);
    }
    bool request(bool visible, bool animate, double now) {
        if (wants_visible() == visible && (animate || !moving())) return false;
        double current = value(now);
        ++generation; from = current; to = visible ? 1.0 : 0.0; started = now;
        duration = animate ? std::max(1.0, (visible ? enter_ms : exit_ms) * std::abs(to - from)) : 0;
        phase = animate && std::abs(to - from) > 0.0001 ?
            (visible ? MotionPhase::Entering : MotionPhase::Exiting) :
            (visible ? MotionPhase::Visible : MotionPhase::Hidden);
        return true;
    }
    bool advance(double now) {
        if (!moving() || now - started < duration) return false;
        phase = to > 0 ? MotionPhase::Visible : MotionPhase::Hidden; from = to; duration = 0;
        return true;
    }
};
inline RECT translated_panel(RECT target, double openness, bool vertical) {
    int offset = int(std::lround((vertical ? target.bottom - target.top : target.right - target.left) *
                                (1.0 - std::clamp(openness, 0.0, 1.0))));
    OffsetRect(&target, vertical ? 0 : offset, vertical ? offset : 0);
    return target;
}
inline RECT motion_clip(RECT target, RECT translated) {
    RECT visible{}; IntersectRect(&visible, &target, &translated);
    if (IsRectEmpty(&visible)) return {};
    OffsetRect(&visible, -translated.left, -translated.top); return visible;
}

// This worker only posts coalesced frame requests. HWND operations stay on the
// WinUI thread. DWM and a high-resolution waitable timer pace active animations;
// there is no permanent animation timer and no global timer-resolution change.
class MotionFrames {
    struct State {
        winrt::handle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        std::atomic_bool active = false, stop = false, pending = false;
        HWND target = nullptr;
        UINT message = 0;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
public:
    ~MotionFrames() { stop(); }
    void start(HWND target, UINT message) {
        state->target = target; state->message = message;
        auto shared = state;
        std::thread([shared] {
            winrt::handle timer{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
            if (!timer) timer.attach(CreateWaitableTimerW(nullptr, FALSE, nullptr));
            while (!shared->stop.load()) {
                if (!shared->active.load()) { WaitForSingleObject(shared->wake.get(), INFINITE); continue; }
                auto before = motion_now_ms();
                DWM_TIMING_INFO timing{}; timing.cbSize = sizeof(timing);
                double period = SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.rateRefresh.uiNumerator ?
                    1000.0 * timing.rateRefresh.uiDenominator / timing.rateRefresh.uiNumerator : 1000.0 / 60;
                period = std::clamp(period, 1000.0 / 360, 1000.0 / 30);
                DwmFlush();
                if (shared->stop.load()) break;
                if (shared->active.load() && !shared->pending.exchange(true)) {
                    if (!PostMessageW(shared->target, shared->message, 0, 0)) shared->pending = false;
                }
                double remaining = period - (motion_now_ms() - before);
                if (remaining > 0.1 && timer) {
                    LARGE_INTEGER due{}; due.QuadPart = -LONGLONG(std::max(1.0, remaining * 10000));
                    if (SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE)) {
                        HANDLE waits[]{shared->wake.get(), timer.get()};
                        WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                    }
                } else if (!timer) WaitForSingleObject(shared->wake.get(), DWORD(std::max(1.0, remaining)));
            }
        }).detach();
    }
    void active(bool value) {
        bool previous = state->active.exchange(value);
        if (previous != value) SetEvent(state->wake.get());
    }
    void consumed() { state->pending = false; }
    void stop() { state->stop = true; state->active = false; SetEvent(state->wake.get()); }
};
} // namespace companion
