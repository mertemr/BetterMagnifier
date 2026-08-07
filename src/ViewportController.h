#pragma once

// Per-monitor source-rectangle state and the edge-push pan model.
//
// Pure math: no Windows calls, no logging, no allocation. That is deliberate —
// it is the only part of the magnification transform that can be asserted from
// a script, and every geometry bug this project has had lived here.
//
// Coordinates: pointer positions are virtual-desktop pixels, and they can be
// negative when a monitor sits left of or above the primary. srcOrigin is
// monitor-local. Both are double so that sub-pixel hand motion survives being
// scaled by 1/zoom.
//
// Thread ownership: the input thread. It advances on mouse events rather than
// on frames, because "push by as much as the mouse pushed" has to be
// proportional to mouse motion, not to frame rate.

#ifndef BETTER_MAGNIFIER_VIEWPORT_CONTROLLER_H
#define BETTER_MAGNIFIER_VIEWPORT_CONTROLLER_H

#include <array>
#include <cstddef>

namespace BetterMagnifier {

enum class Edge { Left, Right, Top, Bottom };

struct EdgePushConfig
{
    bool  enabled      = true;
    float bandFraction = 0.12f;    // of the axis length
    float bandMinPx    = 80.0f;
    float bandMaxPx    = 300.0f;
};

struct MonitorViewport
{
    long   originX = 0;      // monitor rect in virtual-desktop coordinates
    long   originY = 0;
    long   width   = 0;
    long   height  = 0;

    double srcOriginX = 0.0; // monitor-local, always within [0, MaxSrcOrigin]
    double srcOriginY = 0.0;
    double zoom       = 1.0;
};

class ViewportController
{
public:
    static constexpr std::size_t kMaxMonitors = 8;

    void        SetMonitorCount(std::size_t count);
    std::size_t MonitorCount() const { return m_count; }

    void SetMonitorRect(std::size_t index, long originX, long originY,
                        long width, long height);

    void SetConfig(const EdgePushConfig& cfg) { m_cfg = cfg; }
    const EdgePushConfig& Config() const { return m_cfg; }

    // Keeps both the content under the pointer and the pointer's screen
    // position fixed across the change.
    void   SetZoom(std::size_t index, double zoom, double pointerX, double pointerY);
    double Zoom(std::size_t index) const;

    // The pointer has already been advanced by the caller. Pushes srcOrigin
    // when the pointer's screen position falls inside an edge band. Does not
    // modify the pointer: when the source runs out, the leftover motion is
    // exactly what carries the pointer to the physical edge and onto the next
    // monitor.
    void OnPointerMoved(std::size_t index, double pointerX, double pointerY);

    // Positions the pointer for a monitor it just entered, preserving that
    // monitor's srcOrigin so the view stays where the user left it.
    void PlaceOnEntry(std::size_t index, Edge entry, double& pointerX, double& pointerY);

    // Pulls a pointer that fell outside every monitor back into the nearest
    // one, so it cannot drift into empty space at the desktop's outer edge.
    void ClampPointerToDesktop(double& x, double& y) const;

    // -1 when no monitor contains the point.
    int MonitorIndexAt(double x, double y) const;

    // Call after a resolution or topology change.
    void ReclampAll();

    const MonitorViewport& Viewport(std::size_t index) const;

    double MaxSrcOriginX(std::size_t index) const;
    double MaxSrcOriginY(std::size_t index) const;

    // Band width in screen pixels for an axis of the given length.
    double BandPx(long axisLength) const;

    // Pointer's screen position on its monitor, 0..width / 0..height.
    double ScreenX(std::size_t index, double pointerX) const;
    double ScreenY(std::size_t index, double pointerY) const;

private:
    MonitorViewport&       At(std::size_t index);
    const MonitorViewport& At(std::size_t index) const;

    std::array<MonitorViewport, kMaxMonitors> m_v{};
    std::size_t    m_count = 0;
    EdgePushConfig m_cfg{};
};

#ifdef _DEBUG
// Assert-based self-check, run from main. Mirrors SettingsStoreSelfCheck.
void ViewportControllerSelfCheck();
#endif

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_VIEWPORT_CONTROLLER_H
