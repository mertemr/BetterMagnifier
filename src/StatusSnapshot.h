#pragma once

// Live per-monitor state, written by the render thread every frame.
//
// Atomics rather than a mutex: taking a lock on the render thread's hot path
// makes frame time unpredictable, and low latency is the whole point of the
// project. std::atomic<float> and <bool> are lock-free on x64.
//
// Fields are individually atomic, the struct as a whole is not. A reader can
// see a fresh zoomLevel next to a one-frame-old isActive. Fine for a display,
// nothing decides anything on it.
//
// ponytail: nothing reads this yet. It is groundwork for the control panel
// (see docs/superpowers/plans). Delete it if that plan is dropped.

#ifndef BETTER_MAGNIFIER_STATUS_SNAPSHOT_H
#define BETTER_MAGNIFIER_STATUS_SNAPSHOT_H

#include <atomic>
#include <array>
#include <cstddef>

namespace BetterMagnifier {

struct MonitorStatus
{
    // Updated every frame
    std::atomic<float> zoomLevel{1.0f};
    std::atomic<bool>  isActive{false};
    std::atomic<bool>  isFrozen{false};
    std::atomic<bool>  captureOk{false};        // initialised && !needs reinit
    std::atomic<bool>  captureExcluded{false};  // WDA_EXCLUDEFROMCAPTURE took
    std::atomic<float> fps{0.0f};

    // Updated only on init and WM_DISPLAYCHANGE
    static constexpr size_t kNameCapacity = 64;

    // Not atomic: written only during WM_DISPLAYCHANGE. Worst case a reader
    // sees a half-written name and the label looks wrong for one tick. No
    // crash risk: fixed-size array, always null-terminated (wcsncpy_s +
    // _TRUNCATE).
    wchar_t             deviceName[kNameCapacity]{};

    std::atomic<int>    width{0};
    std::atomic<int>    height{0};
    std::atomic<int>    refreshRate{0};
    std::atomic<int>    dpiPercent{100};
    std::atomic<bool>   isPrimary{false};
};

class StatusSnapshot
{
public:
    static constexpr size_t kMaxMonitors = 8;

    StatusSnapshot() = default;
    StatusSnapshot(const StatusSnapshot&) = delete;
    StatusSnapshot& operator=(const StatusSnapshot&) = delete;

    // Out-of-range index clamps to the last slot rather than being undefined:
    // monitor count changes on WM_DISPLAYCHANGE and a reader can be a tick
    // behind.
    MonitorStatus& Monitor(size_t i)
    {
        return m_monitors[i < kMaxMonitors ? i : kMaxMonitors - 1];
    }

    const MonitorStatus& Monitor(size_t i) const
    {
        return m_monitors[i < kMaxMonitors ? i : kMaxMonitors - 1];
    }

    std::atomic<size_t>   monitorCount{0};

    // HotkeyManager::Reregister result. Bit 0 = toggle failed, bit 1 = freeze.
    std::atomic<unsigned> hotkeyFailedMask{0};

private:
    std::array<MonitorStatus, kMaxMonitors> m_monitors{};
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_STATUS_SNAPSHOT_H
