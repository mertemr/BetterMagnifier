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

namespace {

// Shared by both axes. Returns how far srcOrigin should move on this axis.
//
//   screen        pointer position on the axis, 0..length
//   length        monitor extent on the axis, in screen pixels
//   band          band width, in screen pixels
//   zoom          current magnification
//   roomNegative  how far srcOrigin can still travel toward 0
//   roomPositive  how far it can still travel toward the maximum
//
// Only one side fires per call. When the band is wide enough that both halves
// overlap, the near edge wins, which stops a degenerate configuration from
// oscillating between the two.
//
// The caller does NOT adjust the pointer afterwards. That is the whole design:
// while there is room, the push cancels the overshoot and the pointer parks on
// the band's inner edge; once the source runs out, applied < want and the
// leftover is exactly what walks the pointer to the physical edge and onto the
// next monitor. No cursor clipping needed — the crossing falls out of the math.
double PushAmount(double screen, double length, double band, double zoom,
                  double roomNegative, double roomPositive)
{
    if (zoom <= kNoZoom || band <= 0.0)
        return 0.0;

    if (screen < band)
    {
        const double want = (band - screen) / zoom;
        return -(std::min)(want, roomNegative);
    }

    if (screen > length - band)
    {
        const double want = (screen - (length - band)) / zoom;
        return (std::min)(want, roomPositive);
    }

    return 0.0;
}

} // namespace

void ViewportController::OnPointerMoved(std::size_t index, double pointerX, double pointerY)
{
    if (!m_cfg.enabled)
        return;

    MonitorViewport& v = At(index);
    if (v.zoom <= kNoZoom)
        return;

    const double dx = PushAmount(ScreenX(index, pointerX),
                                 static_cast<double>(v.width),
                                 BandPx(v.width), v.zoom,
                                 v.srcOriginX,
                                 MaxSrcOriginX(index) - v.srcOriginX);

    const double dy = PushAmount(ScreenY(index, pointerY),
                                 static_cast<double>(v.height),
                                 BandPx(v.height), v.zoom,
                                 v.srcOriginY,
                                 MaxSrcOriginY(index) - v.srcOriginY);

    v.srcOriginX = std::clamp(v.srcOriginX + dx, 0.0, MaxSrcOriginX(index));
    v.srcOriginY = std::clamp(v.srcOriginY + dy, 0.0, MaxSrcOriginY(index));
}

void ViewportController::PlaceOnEntry(std::size_t index, Edge entry,
                                      double& pointerX, double& pointerY)
{
    const MonitorViewport& v = At(index);

    // srcOrigin is deliberately left alone: the view stays where the user left
    // it and the pointer is moved to match. The real pointer teleports, but it
    // is hidden, so what the user sees is a sprite entering from the edge.
    const auto sourceAt = [&](double screen, long origin, double srcOrigin) {
        return static_cast<double>(origin) + srcOrigin + screen / v.zoom;
    };

    switch (entry)
    {
    case Edge::Left:
        pointerX = sourceAt(0.0, v.originX, v.srcOriginX);
        break;
    case Edge::Right:
        pointerX = sourceAt(static_cast<double>(v.width), v.originX, v.srcOriginX);
        break;
    case Edge::Top:
        pointerY = sourceAt(0.0, v.originY, v.srcOriginY);
        break;
    case Edge::Bottom:
        pointerY = sourceAt(static_cast<double>(v.height), v.originY, v.srcOriginY);
        break;
    }
}

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

    // Inside the band the view does not move at all.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 960.0, 540.0);
        const double before = vc.Viewport(0).srcOriginX;
        vc.OnPointerMoved(0, 960.0, 540.0);          // dead centre
        BM_SELFCHECK(vc.Viewport(0).srcOriginX == before);
    }

    // Past the band the view moves by exactly the overshoot, in source pixels.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        // Zooming at the origin is the only way to land srcOrigin on 0, since
        // SetZoom preserves the pointer's screen position.
        vc.SetZoom(0, 2.0, 0.0, 0.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX == 0.0);

        const double band = vc.BandPx(1920);         // 230.4
        // Put the pointer 100 screen px past the band's inner edge.
        // screen = (p - srcOrigin) * zoom  =>  p = screen / zoom
        const double targetScreen = 1920.0 - band + 100.0;
        const double p = targetScreen / 2.0;
        vc.OnPointerMoved(0, p, 270.0);
        BM_SELFCHECK(std::abs(vc.Viewport(0).srcOriginX - 50.0) < 1e-6);  // 100 / zoom

        // And the pointer now sits exactly at the band's inner edge.
        BM_SELFCHECK(std::abs(vc.ScreenX(0, p) - (1920.0 - band)) < 1e-6);
    }

    // The push saturates at the source edge and never overruns it.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 960.0, 540.0);
        for (int i = 0; i < 200; ++i)
            vc.OnPointerMoved(0, 1900.0, 540.0);     // keep shoving right
        BM_SELFCHECK(std::abs(vc.Viewport(0).srcOriginX - vc.MaxSrcOriginX(0)) < 1e-6);

        // Saturated: the pointer is now free to run past the band toward the
        // physical edge, which is what lets it cross to the next monitor.
        BM_SELFCHECK(vc.ScreenX(0, 1900.0) > 1920.0 - vc.BandPx(1920));
    }

    // The left edge is the mirror image.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 1440.0, 540.0);           // srcOrigin lands on 720
        const double scrolled = vc.Viewport(0).srcOriginX;
        BM_SELFCHECK(scrolled > 0.0);
        // Screen position must be inside the left band for this to fire:
        // (750 - 720) * 2 = 60, well under the 230.4 band.
        for (int i = 0; i < 200; ++i)
            vc.OnPointerMoved(0, 750.0, 540.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX < scrolled);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX >= 0.0);
    }

    // Y pushes independently of X.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 480.0, 270.0);
        const double xBefore = vc.Viewport(0).srcOriginX;
        const double yBefore = vc.Viewport(0).srcOriginY;
        vc.OnPointerMoved(0, 480.0, 1000.0);         // deep in the bottom band only
        BM_SELFCHECK(vc.Viewport(0).srcOriginX == xBefore);
        BM_SELFCHECK(vc.Viewport(0).srcOriginY > yBefore);
    }

    // Disabled config means the view never moves.
    {
        ViewportController vc;
        EdgePushConfig cfg; cfg.enabled = false;
        vc.SetConfig(cfg);
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 480.0, 270.0);
        const double before = vc.Viewport(0).srcOriginX;
        vc.OnPointerMoved(0, 1900.0, 270.0);         // far into the right band
        BM_SELFCHECK(vc.Viewport(0).srcOriginX == before);
    }

    // Degenerate: a band wider than half the viewport must not fight itself.
    // Both edges firing at once would oscillate forever.
    {
        ViewportController vc;
        EdgePushConfig cfg;
        cfg.bandFraction = 0.9f;
        cfg.bandMinPx    = 1.0f;
        cfg.bandMaxPx    = 100000.0f;
        vc.SetConfig(cfg);
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 480.0, 270.0);
        vc.OnPointerMoved(0, 480.0, 270.0);
        const double settled = vc.Viewport(0).srcOriginX;
        vc.OnPointerMoved(0, 480.0, 270.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX == settled);   // settles, no oscillation
    }

    // zoom == 1 is fully transparent: no push, no state.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 1.0, 960.0, 540.0);
        vc.OnPointerMoved(0, 1919.0, 1079.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginX == 0.0);
        BM_SELFCHECK(vc.Viewport(0).srcOriginY == 0.0);
    }

    // Entering a monitor preserves its srcOrigin and puts the pointer on the
    // crossed edge, so the view stays where the user left it.
    {
        ViewportController vc;
        vc.SetMonitorCount(2);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetMonitorRect(1, 1920, 0, 1920, 1080);
        vc.SetZoom(1, 2.0, 1920.0 + 1440.0, 540.0);
        const double kept = vc.Viewport(1).srcOriginX;
        BM_SELFCHECK(kept > 0.0);

        double px = 1921.0, py = 540.0;
        vc.PlaceOnEntry(1, Edge::Left, px, py);
        BM_SELFCHECK(vc.Viewport(1).srcOriginX == kept);          // view preserved
        BM_SELFCHECK(std::abs(vc.ScreenX(1, px) - 0.0) < 1e-6);   // enters at x=0
    }

    // Entry from the right lands on the far edge.
    {
        ViewportController vc;
        vc.SetMonitorCount(2);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetMonitorRect(1, 1920, 0, 1920, 1080);
        vc.SetZoom(0, 4.0, 960.0, 540.0);
        double px = 1919.0, py = 540.0;
        vc.PlaceOnEntry(0, Edge::Right, px, py);
        BM_SELFCHECK(std::abs(vc.ScreenX(0, px) - 1920.0) < 1e-6);
    }

    LOG_INFO("ViewportControllerSelfCheck passed");
}
#endif

} // namespace BetterMagnifier
