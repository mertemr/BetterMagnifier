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

    LOG_INFO("Pointer tuning: speed={:.2f} compensation={:.2f} lockToMonitor={}",
             m_speed.load(std::memory_order_relaxed),
             m_compensation.load(std::memory_order_relaxed),
             m_lockToMonitor.load(std::memory_order_relaxed) ? "on" : "off");

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
    // exactly the position we set. Anything else injected is a foreign
    // SetCursorPos — a game, RDP, returning from Ctrl+Alt+Del — and means our V
    // is stale, so resync rather than treating it as motion.
    if (data.flags & LLMHF_INJECTED)
    {
        if (p.x == m_lastSet.x && p.y == m_lastSet.y)
            return true;            // our echo: consume, change nothing

        Resync();
        return false;
    }

    if (!m_enabled.load(std::memory_order_relaxed))
    {
        trackRaw();
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
    if (m_lockToMonitor.load(std::memory_order_relaxed))
    {
        const MonitorViewport& v = m_viewport->Viewport(static_cast<std::size_t>(monitor));
        m_x = std::clamp(m_x, static_cast<double>(v.originX),
                              static_cast<double>(v.originX + v.width)  - 1.0);
        m_y = std::clamp(m_y, static_cast<double>(v.originY),
                              static_cast<double>(v.originY + v.height) - 1.0);
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
    SetCursorPos(target.x, target.y);
    m_lastSet = target;

    m_snapshot->pointerX.store(m_x, std::memory_order_relaxed);
    m_snapshot->pointerY.store(m_y, std::memory_order_relaxed);

    return true;   // swallow: the OS must not also move the cursor
}

} // namespace BetterMagnifier
