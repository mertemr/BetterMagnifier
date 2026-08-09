#include "pch.h"
#include "PointerInput.h"
#include "Logger.h"

#include <cmath>

namespace BetterMagnifier {

void PointerInput::Attach(ViewportController* controller, ViewportSnapshot* snapshot)
{
    m_viewport = controller;
    m_snapshot = snapshot;
    Resync();
}

void PointerInput::SetSpeed(float speed)
{
    m_speed.store((speed > 0.01f) ? speed : 1.0f, std::memory_order_relaxed);
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

    const double scale =
        static_cast<double>(m_speed.load(std::memory_order_relaxed)) / zoom;

    m_x += (static_cast<double>(p.x) - static_cast<double>(m_lastSet.x)) * scale;
    m_y += (static_cast<double>(p.y) - static_cast<double>(m_lastSet.y)) * scale;

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
