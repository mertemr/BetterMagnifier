#pragma once

// Scales mouse motion by 1/zoom and keeps the real OS cursor at round(V).
//
// Why this is not optional polish. Without it the real cursor roams the whole
// monitor while the visible source window is only width/zoom wide, so the
// pointer spends almost all its time outside the region actually on screen.
// Edge-push then saturates immediately, the usable middle shrinks to
// (width - 2*band)/zoom, and a zoom change swings srcOrigin between its clamps.
// The pan geometry needs the pointer to live inside the visible window, and
// scaling is what keeps it there.
//
// Why the real cursor still moves: clicks, hover, drag and every application's
// own hit-testing use the OS cursor position. Holding it at round(V) is what
// makes a click land on whatever the magnified sprite is pointing at, with no
// coordinate remapping anywhere in the system.
//
// Only WM_MOUSEMOVE is consumed. Buttons and the wheel pass through untouched.
//
// Thread ownership: the input thread, called from inside the low-level hook.
// Everything here is arithmetic plus one SetCursorPos.

#ifndef BETTER_MAGNIFIER_POINTER_INPUT_H
#define BETTER_MAGNIFIER_POINTER_INPUT_H

#include "ViewportController.h"
#include "ViewportSnapshot.h"

#include <windows.h>
#include <atomic>

namespace BetterMagnifier {

// Sustained-pressure release for the monitor lock.
//
// A lock with no way out is a trap: the only route to the next display becomes
// turning zoom off, which is worse than the drift the lock prevents. So the
// clamp yields to a deliberate shove — keep pushing into the same edge, once
// the view has nothing left to pan into, and the pointer is let through.
//
// Pure logic, no Windows calls, so the self-check can drive it. Time arrives as
// the hook event's own timestamp; unsigned arithmetic makes the GetTickCount
// wrap at 49 days a non-event.
class EdgeBreakout
{
public:
    // Long enough that brushing the edge on the way to something else never
    // releases, short enough that a deliberate shove does not feel stuck.
    static constexpr unsigned long kDefaultHoldMs = 300;

    // edge      : an Edge cast to int, or -1 when the motion stays on the monitor
    // saturated : the view has nothing left to reveal on that edge, so holding
    //             out any longer would not show the user anything new
    //
    // True when the clamp must be lifted for this event.
    bool Update(int edge, bool saturated, unsigned long timeMs);

    void Reset() { m_edge = -1; m_open = false; }
    void SetHoldMs(unsigned long ms) { m_holdMs = ms; }

private:
    unsigned long m_holdMs = kDefaultHoldMs;
    unsigned long m_since  = 0;
    int           m_edge   = -1;
    bool          m_open   = false;
};

class PointerInput
{
public:
    void Attach(ViewportController* controller, ViewportSnapshot* snapshot);

    // Off means native behaviour: no swallowing, no SetCursorPos, no sprite.
    // Gated by the caller on SystemCursor::MagPathAvailable, because a sprite
    // without hiding the real pointer shows two pointers in different places.
    void SetEnabled(bool enabled);
    bool Enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // scale = speed / pow(zoom, compensation)
    //
    // Two knobs because they answer different complaints. compensation = 1
    // maps hand movement 1:1 onto the magnified screen, which sounds ideal and
    // measured as far too slow in practice: the content is zoom times further
    // apart visually, so crossing it takes zoom times the hand travel.
    // compensation below 1 gives that back where the problem actually is — at
    // high zoom — instead of uniformly. speed then trims to taste.
    //
    // 0.2 is not a guess. 1.0 was unusable, 0.5 still slow, 0.2 tested right on
    // a 2560x1440 plus 1920x1080 pair. It is closer to native than to full
    // compensation, which says the theory oversold how much correction the
    // pointer actually wants.
    void SetSpeed(float speed);
    void SetCompensation(float compensation);

    // Keep the pointer on the monitor it is magnifying instead of letting it
    // walk onto the next one. Wanted deliberately: with a zoomed edge the
    // pointer used to slip onto the neighbouring display exactly when the user
    // was trying to reach the edge of the magnified content.
    //
    // Not absolute — see EdgeBreakout for the way out.
    void SetLockToMonitor(bool lock);

    // True when the event was consumed: the caller must return 1 from the hook
    // and must NOT chain.
    bool OnMouseMove(const MSLLHOOKSTRUCT& data);

    // Snap V to the OS cursor. Call on enable, display change and unlock.
    void Resync();

private:
    // True when srcOrigin has run out on that edge, so edge-push cannot reveal
    // anything more and the only thing left beyond it is the next monitor.
    bool EdgeIsSaturated(std::size_t index, int edge) const;

    ViewportController* m_viewport = nullptr;
    ViewportSnapshot*   m_snapshot = nullptr;

    std::atomic<bool>  m_enabled{false};
    std::atomic<float> m_speed{1.0f};
    std::atomic<float> m_compensation{0.2f};
    std::atomic<bool>  m_lockToMonitor{true};

    // Touched only from the hook, which runs on one thread. Not atomic.
    double       m_x = 0.0;
    double       m_y = 0.0;
    POINT        m_lastSet{0, 0};
    EdgeBreakout m_breakout;
};

#ifdef _DEBUG
// Assert-based self-check, run from main. Covers EdgeBreakout, which is the
// only part of this file that is pure logic.
void PointerInputSelfCheck();
#endif

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_POINTER_INPUT_H
