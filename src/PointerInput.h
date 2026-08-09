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
    // measured as too slow in practice: the content is zoom times further apart
    // visually, so crossing it takes zoom times the hand travel. compensation
    // below 1 gives some of that back, and it does so where the problem
    // actually is — at high zoom — instead of uniformly. speed then trims the
    // result to taste.
    void SetSpeed(float speed);
    void SetCompensation(float compensation);

    // Keep the pointer on the monitor it is magnifying instead of letting it
    // walk onto the next one. Wanted deliberately: with a zoomed edge the
    // pointer used to slip onto the neighbouring display exactly when the user
    // was trying to reach the edge of the magnified content.
    void SetLockToMonitor(bool lock);

    // True when the event was consumed: the caller must return 1 from the hook
    // and must NOT chain.
    bool OnMouseMove(const MSLLHOOKSTRUCT& data);

    // Snap V to the OS cursor. Call on enable, display change and unlock.
    void Resync();

private:
    ViewportController* m_viewport = nullptr;
    ViewportSnapshot*   m_snapshot = nullptr;

    std::atomic<bool>  m_enabled{false};
    std::atomic<float> m_speed{1.0f};
    std::atomic<float> m_compensation{0.5f};
    std::atomic<bool>  m_lockToMonitor{true};

    // Touched only from the hook, which runs on one thread. Not atomic.
    double m_x = 0.0;
    double m_y = 0.0;
    POINT  m_lastSet{0, 0};
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_POINTER_INPUT_H
