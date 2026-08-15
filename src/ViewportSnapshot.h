#pragma once

// Lock-free exchange of viewport state between the render thread and the input
// thread. Same reasoning as StatusSnapshot: a lock on the render thread's hot
// path makes frame time unpredictable, and a lock taken inside a low-level hook
// risks priority inversion against LowLevelHooksTimeout.
//
// Traffic runs both ways, and which side owns which field matters:
//
//   render -> input   requestedZoom, monitor rects, monitorCount, layoutEpoch
//   input  -> render  srcOriginX/Y, zoom, pointerX/Y, pointerScaled
//
// Why zoom travels as a *request* rather than a call: zoom is mutated from
// eight places in App (hotkeys, the tray, the panel, auto-off at minZoom, and
// so on). Threading a notification through every one of them means every future
// call site has to remember, and the failure mode when one forgets is a silent
// desync between the zoom being rendered and the zoom the source rect was
// computed for. Publishing the settled value once per frame cannot be forgotten
// by construction, and the input thread applies it idempotently.
//
// Fields are individually atomic, the struct is not. A reader can see a fresh
// srcOriginX beside a one-event-old zoom; the visible cost is one frame of a
// slightly stale view, which is below the noise floor at 60 Hz.
//
// std::atomic<double> and <long> are lock-free on x64.

#ifndef BETTER_MAGNIFIER_VIEWPORT_SNAPSHOT_H
#define BETTER_MAGNIFIER_VIEWPORT_SNAPSHOT_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace BetterMagnifier {

struct MonitorViewportAtomic
{
    // ── input thread writes, render thread reads ──
    std::atomic<double> srcOriginX{0.0};   // monitor-local source origin
    std::atomic<double> srcOriginY{0.0};
    std::atomic<double> zoom{1.0};         // the zoom the origin was computed for

    // ── render thread writes, input thread reads ──
    std::atomic<double> requestedZoom{1.0};
    std::atomic<long>   originX{0};        // monitor rect, virtual-desktop coords
    std::atomic<long>   originY{0};
    std::atomic<long>   width{0};
    std::atomic<long>   height{0};

    // Freeze pins the view: the pointer keeps moving, the source rect does not.
    // It has to be honoured here rather than by not moving the pointer, because
    // the pointer is also what the user is still steering.
    std::atomic<bool>   frozen{false};
};

class ViewportSnapshot
{
public:
    static constexpr std::size_t kMaxMonitors = 8;

    ViewportSnapshot() = default;
    ViewportSnapshot(const ViewportSnapshot&) = delete;
    ViewportSnapshot& operator=(const ViewportSnapshot&) = delete;

    // Out-of-range clamps to the last slot rather than being undefined: the
    // monitor count changes on WM_DISPLAYCHANGE and a reader can be a tick
    // behind. Same contract as StatusSnapshot::Monitor.
    MonitorViewportAtomic& Monitor(std::size_t i)
    {
        return m_monitors[i < kMaxMonitors ? i : kMaxMonitors - 1];
    }

    const MonitorViewportAtomic& Monitor(std::size_t i) const
    {
        return m_monitors[i < kMaxMonitors ? i : kMaxMonitors - 1];
    }

    std::atomic<std::size_t> monitorCount{0};

    // Bumped by the render thread on WM_DISPLAYCHANGE. The input thread
    // re-reads every rect when it moves, rather than diffing eight of them on
    // every mouse event.
    std::atomic<std::uint64_t> layoutEpoch{0};

    // Virtual-desktop coordinates. The authoritative pointer position: the real
    // OS cursor is kept at round(pointerX, pointerY) once scaling is on.
    std::atomic<double> pointerX{0.0};
    std::atomic<double> pointerY{0.0};

    // True while PointerInput is driving the cursor. When false the renderer
    // must not draw a sprite — the real pointer is visible and doing the job.
    std::atomic<bool> pointerScaled{false};

    // ── Keyboard focus following: render -> input ──
    //
    // One request rather than one per monitor, because focus is singular; the
    // monitor it landed on travels with it.
    //
    // Epoch-gated rather than a bare position, and that is the whole mechanism.
    // A position alone cannot be distinguished from the same position arriving
    // again, so the input thread would re-centre on every sync tick and pin the
    // view to the last focused control for good — the pointer could never move
    // it again. A counter makes "there is a NEW request" a fact rather than an
    // inference, and the input thread consumes each one exactly once.
    //
    // Write order matters: coordinates first, then the epoch with release, so
    // an input thread that sees the new epoch also sees the position that goes
    // with it.
    std::atomic<std::uint64_t> focusEpoch{0};
    std::atomic<std::size_t>   focusMonitor{0};
    std::atomic<double>        focusX{0.0};
    std::atomic<double>        focusY{0.0};

private:
    std::array<MonitorViewportAtomic, kMaxMonitors> m_monitors{};
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_VIEWPORT_SNAPSHOT_H
