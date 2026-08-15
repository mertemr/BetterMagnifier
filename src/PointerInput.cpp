#include "pch.h"
#include "PointerInput.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>

namespace BetterMagnifier {

namespace {

// Feel is not something to guess at from a desk. These are a startup override
// for experimenting without a rebuild; the real controls are in settings.ini
// and the panel, and ApplySettings overwrites these the moment settings load.
// Use them to find a number, then put it in the settings.
//
//   BM_POINTER_SPEED  overall multiplier          (default 1.0)
//   BM_POINTER_COMP   zoom compensation, 0..1     (default 0.2, measured)
//                     1 = hand maps 1:1 on the magnified screen (slowest)
//                     0 = native, pointer flies at zoom times speed
//   BM_POINTER_LOCK   0 to let the pointer leave the magnified monitor
//   BM_POINTER_BREAKOUT_PX  how much hand travel a shove at a spent edge has to
//                     spend before the lock opens (default 150)
float ReadEnvFloat(const wchar_t* name, float fallback)
{
    wchar_t buf[32]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, 32);
    if (n == 0 || n >= 32)
        return fallback;

    const double v = _wtof(buf);
    return (v > 0.0) ? static_cast<float>(v) : fallback;
}

} // namespace

bool EdgeBreakout::Update(int edge, double overshootPx, bool saturated,
                          unsigned long timeMs)
{
    // The hand stopped pushing: let the pressure go. This is the only thing
    // that ends a shove, deliberately. An event that simply did not press on
    // this edge — motion along the other axis, a jitter that rounded to
    // nothing — must not, because while the clamp holds the pointer still those
    // events are indistinguishable from the push itself.
    if (m_edge >= 0 && (timeMs - m_last) > kIdleMs)
        Reset();

    // Not pressing past an edge, or the view can still pan there: the shove is
    // being spent on edge-push, which is the more useful thing to do with it.
    if (edge < 0 || !saturated)
        return m_open;

    // A different edge is a different intention, so the travel starts over.
    if (edge != m_edge)
    {
        m_edge   = edge;
        m_travel = 0.0;
        m_open   = false;
    }

    m_travel += overshootPx;
    m_last    = timeMs;

    if (m_travel >= m_thresholdPx)
        m_open = true;

    return m_open;
}

void PointerInput::Attach(ViewportController* controller, ViewportSnapshot* snapshot)
{
    m_viewport = controller;
    m_snapshot = snapshot;

    SetSpeed(ReadEnvFloat(L"BM_POINTER_SPEED", 1.0f));

    // Read separately: 0 is a legitimate value here, and ReadEnvFloat treats
    // non-positive as "unset" so that a malformed number does not silently
    // become a valid setting.
    {
        wchar_t buf[32]{};
        if (GetEnvironmentVariableW(L"BM_POINTER_COMP", buf, 32) > 0)
            SetCompensation(static_cast<float>(_wtof(buf)));
    }
    {
        wchar_t buf[8]{};
        if (GetEnvironmentVariableW(L"BM_POINTER_LOCK", buf, 8) > 0)
            SetLockToMonitor(buf[0] != L'0');
    }

    double breakoutPx = EdgeBreakout::kDefaultThresholdPx;
    {
        wchar_t buf[16]{};
        if (GetEnvironmentVariableW(L"BM_POINTER_BREAKOUT_PX", buf, 16) > 0)
        {
            const double v = _wtof(buf);
            if (v >= 0.0)
                breakoutPx = v;
        }
    }
    m_breakout.SetThresholdPx(breakoutPx);

    LOG_INFO("Pointer tuning: speed={:.2f} compensation={:.2f} lockToMonitor={} "
             "breakoutPx={:.0f}",
             m_speed.load(std::memory_order_relaxed),
             m_compensation.load(std::memory_order_relaxed),
             m_lockToMonitor.load(std::memory_order_relaxed) ? "on" : "off",
             breakoutPx);

    Resync();
}

void PointerInput::SetSpeed(float speed)
{
    m_speed.store((speed > 0.01f) ? speed : 1.0f, std::memory_order_relaxed);
}

void PointerInput::SetCompensation(float compensation)
{
    m_compensation.store(std::clamp(compensation, 0.0f, 1.0f), std::memory_order_relaxed);
}

void PointerInput::SetLockToMonitor(bool lock)
{
    m_lockToMonitor.store(lock, std::memory_order_relaxed);
}

void PointerInput::SetEnabled(bool enabled)
{
    if (m_enabled.exchange(enabled, std::memory_order_relaxed) == enabled)
        return;

    // Resync on both edges. Turning on, V has to start from wherever the OS
    // cursor actually is; turning off, the OS resumes control from there.
    Resync();

    if (m_snapshot)
        m_snapshot->pointerScaled.store(enabled, std::memory_order_relaxed);

    LOG_INFO("Pointer scaling {}", enabled ? "ON" : "OFF");

    // Reported on the way out, where logging is free, because the hook must not
    // log. These three numbers are the only evidence of how the injected path
    // actually behaves on a given machine, and every guess about it so far has
    // had to be paid for later: echoLive near zero would mean SetCursorPos is
    // not echoing here at all, and a large foreign count means something else
    // is driving the cursor.
    if (!enabled)
    {
        LOG_INFO("Injected moves this session: echo={} staleEcho={} foreign={}",
                 m_echoLive.exchange(0, std::memory_order_relaxed),
                 m_echoStale.exchange(0, std::memory_order_relaxed),
                 m_foreignInjected.exchange(0, std::memory_order_relaxed));

        LOG_INFO("Monitor lock this session: clamped={} atSpentEdge={} released={}",
                 m_clampHits.exchange(0, std::memory_order_relaxed),
                 m_clampSaturated.exchange(0, std::memory_order_relaxed),
                 m_breakouts.exchange(0, std::memory_order_relaxed));
    }
}

void PointerInput::Resync()
{
    POINT p{};
    if (!GetCursorPos(&p))
        return;

    m_x = static_cast<double>(p.x);
    m_y = static_cast<double>(p.y);
    m_lastSet = p;

    if (m_snapshot)
    {
        m_snapshot->pointerX.store(m_x, std::memory_order_relaxed);
        m_snapshot->pointerY.store(m_y, std::memory_order_relaxed);
    }
}

void PointerInput::RememberTarget(POINT target)
{
    m_recent[m_recentNext] = target;
    m_recentNext = (m_recentNext + 1) % kRecentTargets;
}

bool PointerInput::WasRecentTarget(POINT p) const
{
    for (const POINT& t : m_recent)
        if (t.x == p.x && t.y == p.y)
            return true;
    return false;
}

bool PointerInput::EdgeIsSaturated(std::size_t index, int edge) const
{
    if (edge < 0 || !m_viewport)
        return false;

    // Only edge push has anything left in reserve. Fixed never moves, and
    // Anchored has already moved as far as this pointer position allows — it is
    // a closed-form function of the pointer, so a pointer pinned at the edge is
    // showing everything that edge can show. Either way there is nothing to
    // wait for and pressure alone decides.
    //
    // Stated as a mode test rather than reused geometry on purpose: under
    // Anchored the arithmetic below misses by (1 - 1/zoom) of a source pixel,
    // which at high zoom is enough to keep the lock shut for good.
    if (m_viewport->Config().mode != PanMode::EdgePush)
        return true;

    // Half a source pixel: below that the view cannot move again anyway, and an
    // exact comparison would never fire against a double that edge-push has
    // been nudging in fractions.
    constexpr double kEps = 0.5;

    const MonitorViewport& v = m_viewport->Viewport(index);

    switch (static_cast<Edge>(edge))
    {
    case Edge::Left:   return v.srcOriginX <= kEps;
    case Edge::Right:  return v.srcOriginX >= m_viewport->MaxSrcOriginX(index) - kEps;
    case Edge::Top:    return v.srcOriginY <= kEps;
    case Edge::Bottom: return v.srcOriginY >= m_viewport->MaxSrcOriginY(index) - kEps;
    }

    return false;
}

// Advance the view for a pointer we are not steering. Everything the scaled
// path does to the pointer itself is deliberately absent.
void PointerInput::DriveViewport()
{
    const int monitor = m_viewport->MonitorIndexAt(m_x, m_y);
    if (monitor < 0)
        return;

    const std::size_t mi = static_cast<std::size_t>(monitor);

    if (m_viewport->Zoom(mi) <= 1.0)
        return;

    if (m_snapshot->Monitor(mi).frozen.load(std::memory_order_relaxed))
        return;

    m_viewport->OnPointerMoved(mi, m_x, m_y);
}

bool PointerInput::OnMouseMove(const MSLLHOOKSTRUCT& data)
{
    if (!m_viewport || !m_snapshot)
        return false;

    const POINT p = data.pt;

    auto trackRaw = [&]() {
        m_x = static_cast<double>(p.x);
        m_y = static_cast<double>(p.y);
        m_lastSet = p;
        m_snapshot->pointerX.store(m_x, std::memory_order_relaxed);
        m_snapshot->pointerY.store(m_y, std::memory_order_relaxed);
    };

    // Our own SetCursorPos comes back through the hook as an injected event at
    // exactly the position we set. Three cases, and they need three different
    // answers — treating them as two is what made clicks land wrong now and
    // then.
    if (data.flags & LLMHF_INJECTED)
    {
        if (p.x == m_lastSet.x && p.y == m_lastSet.y)
        {
            // Our latest move, and the cursor is already sitting here. Pass it
            // on without touching V: this event IS the WM_MOUSEMOVE the window
            // underneath gets. Swallowing it, as this used to, left
            // applications with no pointer motion at all while zoomed — hover,
            // drag and text selection went by whatever moves happened to leak
            // through, which is exactly the intermittent misbehaviour reported.
            // It cannot move the cursor anywhere new, so nothing is risked.
            m_echoLive.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (WasRecentTarget(p))
        {
            // A stale echo: one of our earlier targets, overtaken by a newer
            // SetCursorPos before it was delivered. Letting it through would
            // drag the cursor back to a position it has already left, and V
            // would go on measuring deltas from somewhere the cursor is not.
            m_echoStale.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Foreign SetCursorPos — a game, RDP, returning from Ctrl+Alt+Del. It
        // means our V is stale, so resync rather than treating it as motion.
        m_foreignInjected.fetch_add(1, std::memory_order_relaxed);
        Resync();
        return false;
    }

    if (!m_enabled.load(std::memory_order_relaxed))
    {
        trackRaw();

        // The view still has to follow, and this is not a nicety. Panning used
        // to live entirely inside the scaled path below, so turning "Magnified
        // pointer" off froze the magnified view outright — and so did landing
        // on a machine where MagShowSystemCursor is unavailable, because that
        // takes the same branch. The only thing that could move the view was a
        // zoom change. It read as the magnifier being broken, not as a setting.
        //
        // Nothing else from the scaled path applies here: no clamping, no
        // SetCursorPos, no swallowing. The OS moves the cursor at its native
        // speed and we only follow where it went.
        DriveViewport();
        return false;
    }

    const int monitor = m_viewport->MonitorIndexAt(m_x, m_y);
    if (monitor < 0)
    {
        Resync();
        return false;
    }

    const double zoom = m_viewport->Zoom(static_cast<std::size_t>(monitor));

    // Unmagnified monitor: leave the pointer completely alone. This early-out
    // is what keeps the hook free during ordinary use.
    if (zoom <= 1.0)
    {
        trackRaw();
        return false;
    }

    // Partial compensation, not full. compensation = 1 puts hand movement 1:1
    // on the magnified screen, which is correct in principle and too slow in
    // practice, because the content is zoom times further apart than it looks.
    const double comp = static_cast<double>(m_compensation.load(std::memory_order_relaxed));
    const double scale =
        static_cast<double>(m_speed.load(std::memory_order_relaxed)) / std::pow(zoom, comp);

    m_x += (static_cast<double>(p.x) - static_cast<double>(m_lastSet.x)) * scale;
    m_y += (static_cast<double>(p.y) - static_cast<double>(m_lastSet.y)) * scale;

    // Confine the pointer to the magnified display when asked. The source edge
    // is already reachable through edge-push, so there is nothing left on this
    // monitor that walking off it would get you.
    //
    // The clamp is not absolute, and that is the fix for what it used to be:
    // a hard lock made the next display unreachable without turning zoom off.
    // Keep shoving into the same edge once edge-push has run out of source and
    // EdgeBreakout opens the clamp, so a deliberate crossing still works while
    // an accidental brush past the edge does not.
    if (m_lockToMonitor.load(std::memory_order_relaxed))
    {
        const std::size_t mi = static_cast<std::size_t>(monitor);
        const MonitorViewport& v = m_viewport->Viewport(mi);

        const double minX = static_cast<double>(v.originX);
        const double maxX = static_cast<double>(v.originX + v.width)  - 1.0;
        const double minY = static_cast<double>(v.originY);
        const double maxY = static_cast<double>(v.originY + v.height) - 1.0;

        int    edge      = -1;
        double overshoot = 0.0;
        if      (m_x > maxX) { edge = static_cast<int>(Edge::Right);  overshoot = m_x - maxX; }
        else if (m_x < minX) { edge = static_cast<int>(Edge::Left);   overshoot = minX - m_x; }
        else if (m_y > maxY) { edge = static_cast<int>(Edge::Bottom); overshoot = m_y - maxY; }
        else if (m_y < minY) { edge = static_cast<int>(Edge::Top);    overshoot = minY - m_y; }

        const bool saturated = EdgeIsSaturated(mi, edge);

        if (edge >= 0)
        {
            m_clampHits.fetch_add(1, std::memory_order_relaxed);
            if (saturated)
                m_clampSaturated.fetch_add(1, std::memory_order_relaxed);
        }

        const bool wasOpen = m_breakout.Open();

        if (m_breakout.Update(edge, overshoot, saturated, data.time))
        {
            if (!wasOpen)
                m_breakouts.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            m_x = std::clamp(m_x, minX, maxX);
            m_y = std::clamp(m_y, minY, maxY);
        }
    }
    else
    {
        m_breakout.Reset();
    }

    m_viewport->ClampPointerToDesktop(m_x, m_y);

    int nowOn = m_viewport->MonitorIndexAt(m_x, m_y);
    if (nowOn >= 0 && nowOn != monitor)
    {
        // Crossed onto another monitor. Preserve that monitor's view and slide
        // the pointer onto the edge we came through, so the target display
        // stays where the user left it.
        const MonitorViewport& from = m_viewport->Viewport(static_cast<std::size_t>(monitor));

        const Edge entry =
            (m_x >= static_cast<double>(from.originX + from.width))  ? Edge::Left  :
            (m_x <  static_cast<double>(from.originX))               ? Edge::Right :
            (m_y >= static_cast<double>(from.originY + from.height)) ? Edge::Top
                                                                     : Edge::Bottom;

        m_viewport->PlaceOnEntry(static_cast<std::size_t>(nowOn), entry, m_x, m_y);
    }
    else
    {
        nowOn = monitor;
    }

    if (!m_snapshot->Monitor(static_cast<std::size_t>(nowOn))
             .frozen.load(std::memory_order_relaxed))
    {
        m_viewport->OnPointerMoved(static_cast<std::size_t>(nowOn), m_x, m_y);
    }

    const POINT target{ static_cast<LONG>(std::lround(m_x)),
                        static_cast<LONG>(std::lround(m_y)) };

    // Recorded BEFORE the call, not after. The echo can be delivered from
    // inside SetCursorPos itself, and a m_lastSet written afterwards would
    // arrive too late for the test above to recognise our own move — which
    // would send it down the foreign-injected path and resync mid-motion.
    m_lastSet = target;
    RememberTarget(target);

    if (!SetCursorPos(target.x, target.y))
    {
        // The OS cursor is now somewhere we did not put it, and every later
        // delta is measured from m_lastSet. Recording the target we failed to
        // reach would leave the two drifting apart for good: the sprite would
        // be drawn at V while clicks landed at a fixed offset from it, which is
        // exactly the intermittent misclick this guard exists to end.
        LOG_WARN("SetCursorPos({}, {}) failed: {} — resyncing to the OS cursor",
                 target.x, target.y, GetLastError());
        Resync();
        return true;
    }

    m_snapshot->pointerX.store(m_x, std::memory_order_relaxed);
    m_snapshot->pointerY.store(m_y, std::memory_order_relaxed);

    return true;   // swallow: the OS must not also move the cursor
}


#ifdef _DEBUG
void PointerInputSelfCheck()
{
    constexpr int kRight = static_cast<int>(Edge::Right);
    constexpr int kLeft  = static_cast<int>(Edge::Left);

    // Never pressing an edge: nothing ever opens, however many events go by.
    {
        EdgeBreakout b;
        b.SetThresholdPx(150.0);
        for (unsigned long t = 0; t < 5000; t += 100)
            BM_SELFCHECK(b.Update(-1, 0.0, true, t) == false);
    }

    // Pressing, but the view can still pan there. Edge-push gets the motion,
    // and the lock must not open while there is still content to reveal.
    {
        EdgeBreakout b;
        b.SetThresholdPx(150.0);
        for (unsigned long t = 0; t < 5000; t += 100)
            BM_SELFCHECK(b.Update(kRight, 20.0, false, t) == false);
    }

    // Travel accumulates across events and opens on reaching the threshold.
    {
        EdgeBreakout b;
        b.SetThresholdPx(150.0);
        BM_SELFCHECK(b.Update(kRight, 50.0, true, 1000) == false);   //  50
        BM_SELFCHECK(b.Update(kRight, 50.0, true, 1010) == false);   // 100
        BM_SELFCHECK(b.Update(kRight, 49.0, true, 1020) == false);   // 149
        BM_SELFCHECK(b.Update(kRight,  1.0, true, 1030) == true);    // 150
        BM_SELFCHECK(b.Update(kRight,  1.0, true, 1040) == true);    // stays open
    }

    // The regression that made a timer the wrong tool. While the clamp holds
    // the pointer still, events that press nothing — motion along the other
    // axis, jitter that rounded away — arrive constantly between the ones that
    // do. They must not undo the shove: the timer version reset on every one of
    // them, so the hold never accumulated and the lock never opened at all.
    {
        EdgeBreakout b;
        b.SetThresholdPx(150.0);
        BM_SELFCHECK(b.Update(kRight, 80.0, true, 1000) == false);
        BM_SELFCHECK(b.Update(-1,      0.0, true, 1010) == false);   // pure-Y event
        BM_SELFCHECK(b.Update(-1,      0.0, true, 1020) == false);
        BM_SELFCHECK(b.Update(kRight, 80.0, true, 1030) == true);    // 160 total
    }

    // Letting go bleeds the pressure off, so the next crossing is earned again.
    {
        EdgeBreakout b;
        b.SetThresholdPx(150.0);
        BM_SELFCHECK(b.Update(kRight, 100.0, true, 1000) == false);
        BM_SELFCHECK(b.Update(kRight, 100.0, true, 3000) == false);  // idle, then 100
        BM_SELFCHECK(b.Update(kRight,  49.0, true, 3010) == false);  // 149
        BM_SELFCHECK(b.Update(kRight,   1.0, true, 3020) == true);   // 150
    }

    // Turning into a different edge is a different intention: start over.
    {
        EdgeBreakout b;
        b.SetThresholdPx(150.0);
        BM_SELFCHECK(b.Update(kRight, 140.0, true, 1000) == false);
        BM_SELFCHECK(b.Update(kLeft,   40.0, true, 1010) == false);  // not 180
        BM_SELFCHECK(b.Update(kLeft,  110.0, true, 1020) == true);   // 150 of THIS edge
    }

    // The idle bleed-off reads GetTickCount, which wraps every 49 days. The
    // wrap must not read as a 49-day pause and drop a shove in progress.
    // 0xFFFFFF00 is 256 ticks short of the wrap, so tick 44 is 300 ms later.
    {
        EdgeBreakout b;
        b.SetThresholdPx(150.0);
        BM_SELFCHECK(b.Update(kRight, 100.0, true, 0xFFFFFF00ul) == false);
        BM_SELFCHECK(b.Update(kRight,  50.0, true, 44ul)         == true);
    }

    // A threshold of zero opens on first contact, which is what a user who set
    // BM_POINTER_BREAKOUT_PX=0 asked for: the lock off in all but name.
    {
        EdgeBreakout b;
        b.SetThresholdPx(0.0);
        BM_SELFCHECK(b.Update(kRight, 0.0, true, 12345) == true);
    }

    LOG_INFO("PointerInputSelfCheck passed");
}
#endif

} // namespace BetterMagnifier
