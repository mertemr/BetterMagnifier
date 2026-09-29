#pragma once

// Frame pacing for the render loop.
//
// Two halves that belong together but are deliberately separable:
//
//   FramePacer   pure math — given a rate ceiling and when the last present
//                happened, is the next one due yet? No Windows calls, no
//                clock of its own; `now` is always passed in, which is what
//                makes it assertable from FramePacerSelfCheck.
//
//   FrameWaiter  the Win32 half — wait until the pacer's deadline without
//                spinning and without going deaf to messages.
//
// Why this exists at all: the overlay is a layered window, and a layered
// window's swap chain is blt, so Present cannot be given SyncInterval 1 — the
// DWM surface update plus a vblank wait blocked the render thread for long
// enough that WM_HOTKEY went unprocessed and the app looked hung (see the
// comment at the Present call in App::RenderMonitor). With no vSync there is
// nothing pacing the loop, so it presented as fast as DWM would accept, which
// on a screen that changes continuously — a game, a video — is a pile of
// layered-surface updates the display can never show, taken out of the GPU
// budget of the very application being magnified.
//
// So the brake vSync used to be is now explicit, and being explicit it can
// target the monitor's actual refresh rate rather than 60.

#ifndef BETTER_MAGNIFIER_FRAME_PACER_H
#define BETTER_MAGNIFIER_FRAME_PACER_H

#include <chrono>
#include <windows.h>

namespace BetterMagnifier {

// Resolve the effective ceiling for one monitor.
//
//   maxFps == 0   follow the display: its refresh rate is the fastest rate at
//                 which a present can ever be seen.
//   maxFps  > 0   an explicit override, clamped to something sane. Not capped
//                 to the refresh rate: a display that misreports its mode is
//                 exactly the case an override is for.
//
// A refresh rate of zero, or one outside what a display plausibly runs at,
// falls back to 60 — the safe assumption, not the desirable one.
unsigned ResolveFrameRateCap(unsigned maxFps, unsigned refreshRate);

// Bounds for an explicit MaxFps. The floor is low enough to be a deliberate
// power-saving choice and high enough not to feel broken.
inline constexpr unsigned kMinFrameRateCap = 10;
inline constexpr unsigned kMaxFrameRateCap = 1000;

class FramePacer
{
public:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;
    using duration   = clock::duration;

    // 0 means uncapped, and uncapped means Due() always says yes. That is the
    // right answer for the flip-model overlay, where Present(1) blocks on
    // vblank and is already the brake.
    void SetCap(unsigned fps);
    unsigned Cap() const { return m_fps; }

    // Has enough of the interval elapsed to present again?
    bool Due(time_point now) const;

    // How long until Due() turns true. Zero when it already is.
    duration TimeUntilDue(time_point now) const;

    // Record that this monitor's slot has been used and schedule the next
    // deadline.
    //
    // "Serviced" rather than "presented" on purpose: the pacer gates the whole
    // per-monitor pass, capture and copy included, not just the Present. If it
    // only advanced when something was actually drawn, a static screen would
    // leave the deadline permanently in the past, TimeUntilDue would answer
    // zero forever, and the loop would spin a core doing nothing.
    void Serviced(time_point now);

    // Forget the cadence; the next Due() is immediate. Used when a monitor
    // stops being magnified, so turning it back on does not inherit a
    // deadline from minutes ago.
    void Reset();

private:
    // Jitter tolerance, as a fraction of the interval. Without it a wait that
    // lands a hundred microseconds early costs a whole frame — at 144 Hz that
    // is 144 fps turning into 72 for no reason other than arithmetic.
    static constexpr int kToleranceDivisor = 8;

    duration Tolerance() const { return m_interval / kToleranceDivisor; }

    unsigned   m_fps = 0;
    duration   m_interval{};
    time_point m_next{};      // default-constructed: due immediately
};

// =============================================================================
// FrameWaiter — wait out the pacer's deadline without going deaf
// =============================================================================
//
// Sleep() is the obvious tool and the wrong one, twice over.
//
// First, resolution. The default system timer granularity is 15.6 ms, so
// Sleep(4) sleeps 15.6 ms unless some other process on the machine happens to
// have called timeBeginPeriod — which is why the loop's real tick rate has
// been a function of whether a browser was open. That is a 64 Hz ceiling
// arriving by accident, and it is most of the answer to "why does it never go
// above 60". A CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer gets sub-millisecond
// waits without raising the resolution for the whole system, which
// timeBeginPeriod would.
//
// Second, deafness. Sleep cannot be woken by a message, so a hotkey or a zoom
// step posted during the sleep waits it out. MsgWaitForMultipleObjectsEx
// returns the moment anything lands in the queue, which is what keeps the
// input path responsive while the loop is idling between frames.
//
// Falls back to a plain timeout when the high-resolution timer is unavailable
// (before Windows 10 1803). Pacing is then as coarse as the system timer, and
// the application still works.
// =============================================================================
class FrameWaiter
{
public:
    FrameWaiter();
    ~FrameWaiter();

    FrameWaiter(const FrameWaiter&) = delete;
    FrameWaiter& operator=(const FrameWaiter&) = delete;

    // Waits at most `d`, returning early if a message arrives on this thread.
    // A non-positive duration returns immediately.
    void WaitOrMessage(std::chrono::steady_clock::duration d);

    // False when the fallback path is in use, which is worth one log line at
    // startup and nothing else.
    bool IsHighResolution() const { return m_highResolution; }

private:
    HANDLE m_timer         = nullptr;
    bool   m_highResolution = false;
};

#ifdef _DEBUG
// Assert-based self-check, run from main. Mirrors ViewportControllerSelfCheck.
void FramePacerSelfCheck();
#endif

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_FRAME_PACER_H
