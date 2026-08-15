#pragma once

// Monitor enumeration plus per-monitor zoom state.
//
// The per-monitor part is the reason this project exists: Windows Magnifier
// magnifies every display at the same factor. Here each monitor owns its own
// zoom level, anchor and freeze flag.

#ifndef BETTER_MAGNIFIER_MONITOR_MANAGER_H
#define BETTER_MAGNIFIER_MONITOR_MANAGER_H

#include <string>
#include <vector>
#include <mutex>
#include <windows.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

namespace BetterMagnifier {

struct ZoomState
{
    float zoomLevel  = 1.0f;      // 1.0 = no magnification
    bool  isActive   = false;
    POINT focalPoint = {0, 0};    // anchor, in screen coordinates
    float targetZoom = 1.0f;      // for future smooth interpolation
    bool  isFrozen   = false;     // anchor pinned, view stops following

    static constexpr float kMinZoom  = 1.0f;
    static constexpr float kMaxZoom  = 10.0f;
    static constexpr float kZoomStep = 0.25f;
};

struct MonitorInfo
{
    HMONITOR     hMonitor = nullptr;
    RECT         bounds   = {};       // screen coordinates, physical pixels
    RECT         workArea = {};       // minus the taskbar

    std::wstring deviceName;          // "\\\\.\\DISPLAY1"
    std::wstring friendlyName;        // "DELL U2723QE" when available
    bool         isPrimary = false;

    UINT         dpiX = 96;           // 96 = 100% scaling
    UINT         dpiY = 96;
    UINT         refreshRate = 60;
    bool         isHDRCapable = false;

    // Which GPU and which of its outputs, for multi-GPU setups.
    UINT         adapterIndex = 0;
    UINT         outputIndex  = 0;
    Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;

    ZoomState    zoom;

    int   Width() const  { return bounds.right - bounds.left; }
    int   Height() const { return bounds.bottom - bounds.top; }
    float ScaleFactor() const { return static_cast<float>(dpiX) / 96.0f; }
};

class MonitorManager
{
public:
    MonitorManager() = default;
    ~MonitorManager() = default;

    // Not copyable: holds COM pointers.
    MonitorManager(const MonitorManager&) = delete;
    MonitorManager& operator=(const MonitorManager&) = delete;

    bool Initialize();

    // Called on WM_DISPLAYCHANGE. Rebuilds the list from scratch, carrying
    // zoom state across by device name. Existing dxgiOutput pointers become
    // invalid, so callers must rebuild anything derived from them.
    void Refresh();

    const std::vector<MonitorInfo>& GetMonitors() const { return m_monitors; }
    size_t GetMonitorCount() const { return m_monitors.size(); }

    MonitorInfo* GetMonitor(size_t index);
    const MonitorInfo* GetMonitor(size_t index) const;

    MonitorInfo* FindByHandle(HMONITOR hMon);
    MonitorInfo* FindByPoint(POINT pt);
    MonitorInfo* GetPrimaryMonitor();

    void AdjustZoom(size_t monitorIndex, float delta);   // signed delta
    void SetZoom(size_t monitorIndex, float level);
    void ToggleZoom(size_t monitorIndex);
    void ToggleFreezeOnMonitor(size_t monitorIndex);

    void LogAllMonitors() const;

private:
    static BOOL CALLBACK EnumMonitorCallback(HMONITOR hMon, HDC hDC, LPRECT lpRect, LPARAM lParam);

    void PopulateMonitorDetails(MonitorInfo& info);
    bool MatchDXGIOutputs();

    std::vector<MonitorInfo> m_monitors;
    mutable std::mutex       m_mutex;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_MONITOR_MANAGER_H
