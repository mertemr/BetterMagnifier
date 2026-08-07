#include "pch.h"
#include "ViewportController.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>

namespace BetterMagnifier {

namespace {
// At or below this the source equals the monitor and nothing can pan.
constexpr double kNoZoom = 1.0;
} // namespace

MonitorViewport& ViewportController::At(std::size_t index)
{
    return m_v[index < kMaxMonitors ? index : kMaxMonitors - 1];
}

const MonitorViewport& ViewportController::At(std::size_t index) const
{
    return m_v[index < kMaxMonitors ? index : kMaxMonitors - 1];
}

const MonitorViewport& ViewportController::Viewport(std::size_t index) const
{
    return At(index);
}

void ViewportController::SetMonitorCount(std::size_t count)
{
    m_count = (std::min)(count, kMaxMonitors);
}

void ViewportController::SetMonitorRect(std::size_t index, long originX, long originY,
                                        long width, long height)
{
    MonitorViewport& v = At(index);
    v.originX = originX;
    v.originY = originY;
    v.width   = (std::max)(1L, width);
    v.height  = (std::max)(1L, height);

    // A resolution change can leave the old origin past the new bound.
    v.srcOriginX = std::clamp(v.srcOriginX, 0.0, MaxSrcOriginX(index));
    v.srcOriginY = std::clamp(v.srcOriginY, 0.0, MaxSrcOriginY(index));
}

double ViewportController::Zoom(std::size_t index) const { return At(index).zoom; }

double ViewportController::MaxSrcOriginX(std::size_t index) const
{
    const MonitorViewport& v = At(index);
    if (v.zoom <= kNoZoom) return 0.0;
    return static_cast<double>(v.width) - static_cast<double>(v.width) / v.zoom;
}

double ViewportController::MaxSrcOriginY(std::size_t index) const
{
    const MonitorViewport& v = At(index);
    if (v.zoom <= kNoZoom) return 0.0;
    return static_cast<double>(v.height) - static_cast<double>(v.height) / v.zoom;
}

double ViewportController::BandPx(long axisLength) const
{
    const double raw = static_cast<double>(axisLength) * m_cfg.bandFraction;
    return std::clamp(raw, static_cast<double>(m_cfg.bandMinPx),
                           static_cast<double>(m_cfg.bandMaxPx));
}

double ViewportController::ScreenX(std::size_t index, double pointerX) const
{
    const MonitorViewport& v = At(index);
    return (pointerX - static_cast<double>(v.originX) - v.srcOriginX) * v.zoom;
}

double ViewportController::ScreenY(std::size_t index, double pointerY) const
{
    const MonitorViewport& v = At(index);
    return (pointerY - static_cast<double>(v.originY) - v.srcOriginY) * v.zoom;
}

void ViewportController::SetZoom(std::size_t index, double zoom,
                                 double pointerX, double pointerY)
{
    MonitorViewport& v = At(index);

    // The pointer's screen position BEFORE the change; the whole point is to
    // keep it, so the content under the pointer does not slide out from under
    // it while zooming.
    const double screenX = ScreenX(index, pointerX);
    const double screenY = ScreenY(index, pointerY);

    v.zoom = (std::max)(kNoZoom, zoom);

    if (v.zoom <= kNoZoom)
    {
        v.srcOriginX = 0.0;
        v.srcOriginY = 0.0;
        return;
    }

    v.srcOriginX = pointerX - static_cast<double>(v.originX) - screenX / v.zoom;
    v.srcOriginY = pointerY - static_cast<double>(v.originY) - screenY / v.zoom;

    v.srcOriginX = std::clamp(v.srcOriginX, 0.0, MaxSrcOriginX(index));
    v.srcOriginY = std::clamp(v.srcOriginY, 0.0, MaxSrcOriginY(index));
}

int ViewportController::MonitorIndexAt(double x, double y) const
{
    for (std::size_t i = 0; i < m_count; ++i)
    {
        const MonitorViewport& v = m_v[i];
        if (x >= static_cast<double>(v.originX) &&
            x <  static_cast<double>(v.originX + v.width) &&
            y >= static_cast<double>(v.originY) &&
            y <  static_cast<double>(v.originY + v.height))
            return static_cast<int>(i);
    }
    return -1;
}

void ViewportController::ClampPointerToDesktop(double& x, double& y) const
{
    if (m_count == 0 || MonitorIndexAt(x, y) >= 0)
        return;

    std::size_t best = 0;
    double bestDist = -1.0;
    for (std::size_t i = 0; i < m_count; ++i)
    {
        const MonitorViewport& v = m_v[i];
        const double cx = std::clamp(x, static_cast<double>(v.originX),
                                        static_cast<double>(v.originX + v.width)  - 1.0);
        const double cy = std::clamp(y, static_cast<double>(v.originY),
                                        static_cast<double>(v.originY + v.height) - 1.0);
        const double d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        if (bestDist < 0.0 || d < bestDist) { bestDist = d; best = i; }
    }

    const MonitorViewport& v = m_v[best];
    x = std::clamp(x, static_cast<double>(v.originX),
                      static_cast<double>(v.originX + v.width)  - 1.0);
    y = std::clamp(y, static_cast<double>(v.originY),
                      static_cast<double>(v.originY + v.height) - 1.0);
}

void ViewportController::ReclampAll()
{
    for (std::size_t i = 0; i < m_count; ++i)
    {
        m_v[i].srcOriginX = std::clamp(m_v[i].srcOriginX, 0.0, MaxSrcOriginX(i));
        m_v[i].srcOriginY = std::clamp(m_v[i].srcOriginY, 0.0, MaxSrcOriginY(i));
    }
}

// OnPointerMoved and PlaceOnEntry land in the next task.
void ViewportController::OnPointerMoved(std::size_t, double, double) {}
void ViewportController::PlaceOnEntry(std::size_t, Edge, double&, double&) {}

#ifdef _DEBUG
void ViewportControllerSelfCheck()
{
    // 1x zoom: the source covers the whole monitor and cannot move.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 1.0, 960.0, 540.0);
        BM_SELFCHECK(vc.MaxSrcOriginX(0) == 0.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX == 0.0);
        BM_SELFCHECK(vc.ScreenX(0, 960.0) == 960.0);
    }

    // 2x zoom: the source is half the monitor, so srcOrigin can travel half.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 960.0, 540.0);
        BM_SELFCHECK(std::abs(vc.MaxSrcOriginX(0) - 960.0) < 1e-9);
        BM_SELFCHECK(std::abs(vc.MaxSrcOriginY(0) - 540.0) < 1e-9);
    }

    // A zoom change keeps the pointer over the same content AND at the same
    // screen position. This is the invariant that replaces the old anchor
    // identity, so it gets an explicit case.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 960.0, 540.0);
        const double screenBefore = vc.ScreenX(0, 960.0);
        vc.SetZoom(0, 4.0, 960.0, 540.0);
        const double screenAfter = vc.ScreenX(0, 960.0);
        BM_SELFCHECK(std::abs(screenBefore - screenAfter) < 1e-6);
    }

    // srcOrigin never leaves [0, max], even when the pointer sits outside.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 4.0, -5000.0, -5000.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX >= 0.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX <= vc.MaxSrcOriginX(0));
        vc.SetZoom(0, 4.0, 99999.0, 99999.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX <= vc.MaxSrcOriginX(0));
    }

    // Monitors left of and above the primary have negative origins. This has
    // been a bug source in every codebase that assumes (0,0) is the corner.
    {
        ViewportController vc;
        vc.SetMonitorCount(2);
        vc.SetMonitorRect(0, -1920, -200, 1920, 1080);
        vc.SetMonitorRect(1, 0, 0, 2560, 1440);
        BM_SELFCHECK(vc.MonitorIndexAt(-1000.0, 100.0) == 0);
        BM_SELFCHECK(vc.MonitorIndexAt(1000.0, 100.0) == 1);
        BM_SELFCHECK(vc.MonitorIndexAt(9999.0, 9999.0) == -1);

        vc.SetZoom(0, 2.0, -960.0, 340.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX >= 0.0);
        BM_SELFCHECK(vc.ScreenX(0, -960.0) >= 0.0);
        BM_SELFCHECK(vc.ScreenX(0, -960.0) <= 1920.0);
    }

    // A pointer outside every monitor is pulled back into the nearest one.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        double x = 5000.0, y = -300.0;
        vc.ClampPointerToDesktop(x, y);
        BM_SELFCHECK(x >= 0.0 && x <= 1920.0);
        BM_SELFCHECK(y >= 0.0 && y <= 1080.0);
    }

    // Band width is clamped at both ends.
    {
        ViewportController vc;
        EdgePushConfig cfg;
        cfg.bandFraction = 0.12f;
        cfg.bandMinPx    = 80.0f;
        cfg.bandMaxPx    = 300.0f;
        vc.SetConfig(cfg);
        BM_SELFCHECK(vc.BandPx(400)  == 80.0);                       // 48 -> floored
        BM_SELFCHECK(std::abs(vc.BandPx(1920) - 230.4) < 1e-4);      // inside range
        BM_SELFCHECK(vc.BandPx(7680) == 300.0);                      // 921 -> capped
    }

    LOG_INFO("ViewportControllerSelfCheck passed");
}
#endif

} // namespace BetterMagnifier
