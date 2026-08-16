// =============================================================================
// MonitorManager.cpp — Multi-Monitor Enumeration & State Management
// =============================================================================

#include "pch.h"
#include "MonitorManager.h"
#include "Logger.h"

namespace BetterMagnifier {

// =============================================================================
// Initialize — enumerate the monitors and match them to DXGI outputs
// =============================================================================
bool MonitorManager::Initialize()
{
    LOG_INFO("MonitorManager starting");

    m_monitors.clear();

    // lParam carries "this" so the static callback can reach m_monitors.
    if (!EnumDisplayMonitors(nullptr, nullptr, EnumMonitorCallback, reinterpret_cast<LPARAM>(this)))
    {
        LOG_ERROR("EnumDisplayMonitors failed");
        return false;
    }

    if (m_monitors.empty())
    {
        LOG_ERROR("No monitors found");
        return false;
    }

    LOG_INFO("Found {} monitor(s)", m_monitors.size());

    for (auto& mon : m_monitors)
    {
        PopulateMonitorDetails(mon);
    }

    if (!MatchDXGIOutputs())
    {
        // Not fatal: an unmatched output only means that monitor cannot be
        // captured, and the rest still work.
        LOG_WARN("DXGI output matching partially failed, some monitors may not capture");
    }

    LogAllMonitors();

    LOG_INFO("MonitorManager initialised");
    return true;
}

// =============================================================================
// Refresh — rebuild the monitor list (WM_DISPLAYCHANGE)
// =============================================================================
void MonitorManager::Refresh()
{
    LOG_INFO("Rebuilding the monitor list (display change)");

    std::lock_guard lock(m_mutex);

    // Keep the current zoom states so they can be carried into the new list.
    std::unordered_map<std::wstring, ZoomState> savedZoomStates;
    for (const auto& mon : m_monitors)
    {
        savedZoomStates[mon.deviceName] = mon.zoom;
    }

    // Enumerate again
    m_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorCallback, reinterpret_cast<LPARAM>(this));

    for (auto& mon : m_monitors)
    {
        PopulateMonitorDetails(mon);

        // Carry zoom state across a topology change, keyed by device name.
        auto it = savedZoomStates.find(mon.deviceName);
        if (it != savedZoomStates.end())
        {
            mon.zoom = it->second;
            LOG_DEBUG("Zoom state preserved: {} -> level={:.2f}",
                ToUtf8(mon.deviceName),
                mon.zoom.zoomLevel);
        }
    }

    MatchDXGIOutputs();
    LogAllMonitors();
}

// =============================================================================
// EnumMonitorCallback — invoked by Windows once per monitor
// =============================================================================
// Static, as the Win32 signature requires, so the instance arrives via lParam.
// =============================================================================
BOOL CALLBACK MonitorManager::EnumMonitorCallback(
    HMONITOR hMon, HDC /*hDC*/, LPRECT lpRect, LPARAM lParam)
{
    auto* self = reinterpret_cast<MonitorManager*>(lParam);

    MonitorInfo info{};
    info.hMonitor = hMon;

    if (lpRect)
    {
        info.bounds = *lpRect;
    }

    self->m_monitors.push_back(std::move(info));

    return TRUE;   // FALSE would stop the enumeration early
}

// =============================================================================
// PopulateMonitorDetails — name, bounds, DPI and refresh rate for one monitor
// =============================================================================
void MonitorManager::PopulateMonitorDetails(MonitorInfo& info)
{
    // MONITORINFOEX rather than MONITORINFO: the device name comes with it.
    MONITORINFOEXW monInfo{};
    monInfo.cbSize = sizeof(monInfo);

    if (GetMonitorInfoW(info.hMonitor, &monInfo))
    {
        info.bounds    = monInfo.rcMonitor;
        info.workArea  = monInfo.rcWork;
        info.deviceName = monInfo.szDevice;     // e.g. L"\\\\.\\DISPLAY1"
        info.isPrimary = (monInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
    }
    else
    {
        LOG_WARN("GetMonitorInfo failed, HMONITOR: 0x{:X}",
            reinterpret_cast<uintptr_t>(info.hMonitor));
    }

    // ── 2. Per-Monitor DPI ──
    // Windows 8.1+, and the only way to get a per-monitor DPI rather than the
    // system one.
    // 96 DPI is 100% scaling, 144 is 150%, 192 is 200%. Needed for overlay
    // sizing and reported in the panel.
    UINT dpiX = 96, dpiY = 96;
    HRESULT hr = GetDpiForMonitor(info.hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    if (SUCCEEDED(hr))
    {
        info.dpiX = dpiX;
        info.dpiY = dpiY;
    }
    else
    {
        LOG_WARN("GetDpiForMonitor failed: 0x{:08X}", static_cast<unsigned long>(hr));
    }

    // Refresh rate, needed to pace Present against the right vblank.
    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);

    if (EnumDisplaySettingsW(info.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &devMode))
    {
        info.refreshRate = devMode.dmDisplayFrequency;
    }
    else
    {
        LOG_WARN("EnumDisplaySettings failed: {}",
            ToUtf8(info.deviceName));
        info.refreshRate = 60;  // Fallback
    }
}

// =============================================================================
// MatchDXGIOutputs — pair DXGI outputs with physical monitors
// =============================================================================
// Desktop Duplication works per IDXGIOutput, so we have to know which output
// corresponds to which physical monitor. The pairing key is
// DXGI_OUTPUT_DESC.Monitor == MonitorInfo.hMonitor.

//
// Lifetime: the factory, adapters and outputs created here are held in ComPtr
// and released when the scope ends. The exception is the output pointer stored
// into MonitorInfo, whose lifetime then follows that MonitorInfo — which is
// why a display change has to tear the whole chain down rather than patch it.
// =============================================================================
bool MonitorManager::MatchDXGIOutputs()
{
    LOG_INFO("Matching DXGI outputs");

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        LOG_ERROR("CreateDXGIFactory1 failed: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    bool anyMatched = false;

    // Walk every adapter; a machine can have more than one GPU.
    for (UINT adapterIdx = 0; ; adapterIdx++)
    {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapterIdx, &adapter);

        if (hr == DXGI_ERROR_NOT_FOUND)
            break;  // no more adapters

        if (FAILED(hr))
        {
            LOG_WARN("EnumAdapters1({}) failed: 0x{:08X}", adapterIdx, static_cast<unsigned long>(hr));
            continue;
        }

        // Log what this adapter is
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        LOG_INFO("  GPU {}: {} (VRAM: {} MB)",
            adapterIdx,
            ToUtf8(adapterDesc.Description),
            adapterDesc.DedicatedVideoMemory / (1024 * 1024));

        // Enumerate the outputs on this adapter.
        for (UINT outputIdx = 0; ; outputIdx++)
        {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(outputIdx, &output);

            if (hr == DXGI_ERROR_NOT_FOUND)
                break;  // no more outputs on this adapter

            if (FAILED(hr))
            {
                LOG_WARN("EnumOutputs({}) failed: 0x{:08X}", outputIdx, static_cast<unsigned long>(hr));
                continue;
            }

            // Which monitor is this output attached to?
            DXGI_OUTPUT_DESC outputDesc{};
            output->GetDesc(&outputDesc);

            // Match against our MonitorInfo list by HMONITOR
            for (auto& mon : m_monitors)
            {
                if (mon.hMonitor == outputDesc.Monitor)
                {
                    mon.adapterIndex = adapterIdx;
                    mon.outputIndex  = outputIdx;
                    mon.dxgiOutput   = output;
                    anyMatched = true;

                    // Rotation is logged rather than stored: only the renderer
                    // needs it and it takes it from DXGICapture, which re-reads
                    // it on recovery. Here it is the first thing worth seeing
                    // when a portrait monitor misbehaves.
                    const char* rot =
                        (outputDesc.Rotation == DXGI_MODE_ROTATION_ROTATE90)  ? "90"  :
                        (outputDesc.Rotation == DXGI_MODE_ROTATION_ROTATE180) ? "180" :
                        (outputDesc.Rotation == DXGI_MODE_ROTATION_ROTATE270) ? "270" : "none";

                    LOG_INFO("    Output {} -> {} (Adapter {}, rotation {})",
                        outputIdx,
                        ToUtf8(mon.deviceName),
                        adapterIdx,
                        rot);
                    break;
                }
            }
        }
    }

    return anyMatched;
}

// =============================================================================
// Monitor Access Methods
// =============================================================================

MonitorInfo* MonitorManager::GetMonitor(size_t index)
{
    if (index < m_monitors.size())
        return &m_monitors[index];
    return nullptr;
}

const MonitorInfo* MonitorManager::GetMonitor(size_t index) const
{
    if (index < m_monitors.size())
        return &m_monitors[index];
    return nullptr;
}

MonitorInfo* MonitorManager::FindByHandle(HMONITOR hMon)
{
    for (auto& mon : m_monitors)
    {
        if (mon.hMonitor == hMon)
            return &mon;
    }
    return nullptr;
}

// Which monitor contains a point.
MonitorInfo* MonitorManager::FindByPoint(POINT pt)
{
    for (auto& mon : m_monitors)
    {
        if (PtInRect(&mon.bounds, pt))
            return &mon;
    }

    // Outside every monitor rect: let Windows pick the nearest.
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    return FindByHandle(hMon);
}

MonitorInfo* MonitorManager::GetPrimaryMonitor()
{
    for (auto& mon : m_monitors)
    {
        if (mon.isPrimary)
            return &mon;
    }
    // No monitor flagged primary; the first will do.
    if (!m_monitors.empty())
        return &m_monitors[0];
    return nullptr;
}

// =============================================================================
// Zoom Control
// =============================================================================

void MonitorManager::AdjustZoom(size_t monitorIndex, float delta)
{
    auto* mon = GetMonitor(monitorIndex);
    if (!mon) return;

    float newZoom = mon->zoom.zoomLevel + delta;
    newZoom = std::clamp(newZoom, ZoomState::kMinZoom, ZoomState::kMaxZoom);
    mon->zoom.targetZoom = newZoom;
    mon->zoom.zoomLevel  = newZoom;  // Simdilik anlik — smooth interpolation ileride

    LOG_DEBUG("Monitor {} zoom: {:.2f}", monitorIndex, newZoom);
}

void MonitorManager::SetZoom(size_t monitorIndex, float level)
{
    auto* mon = GetMonitor(monitorIndex);
    if (!mon) return;

    mon->zoom.zoomLevel  = std::clamp(level, ZoomState::kMinZoom, ZoomState::kMaxZoom);
    mon->zoom.targetZoom = mon->zoom.zoomLevel;
}

void MonitorManager::ToggleZoom(size_t monitorIndex)
{
    auto* mon = GetMonitor(monitorIndex);
    if (!mon) return;

    mon->zoom.isActive = !mon->zoom.isActive;
    LOG_INFO("Monitor {} zoom {}", monitorIndex, mon->zoom.isActive ? "ON" : "OFF");

    // Reset the level when zoom goes off
    if (!mon->zoom.isActive)
    {
        mon->zoom.zoomLevel = ZoomState::kMinZoom;
        mon->zoom.targetZoom = ZoomState::kMinZoom;
    }
}

void MonitorManager::ToggleFreezeOnMonitor(size_t monitorIndex)
{
    auto* mon = GetMonitor(monitorIndex);
    if (!mon) return;

    mon->zoom.isFrozen = !mon->zoom.isFrozen;
    LOG_INFO("Monitor {} freeze {}", monitorIndex, mon->zoom.isFrozen ? "ON" : "OFF");
}

// =============================================================================
// Debug Logging
// =============================================================================
void MonitorManager::LogAllMonitors() const
{
    LOG_INFO("=== Monitors ({}) ===", m_monitors.size());

    for (size_t i = 0; i < m_monitors.size(); i++)
    {
        const auto& mon = m_monitors[i];
        std::string name = ToUtf8(mon.deviceName);
        
        LOG_INFO("  [{}] {} {}",
            i, name, mon.isPrimary ? "(PRIMARY)" : "");
        LOG_INFO("      Bounds: {}x{} @ ({},{})  DPI: {} ({}%)",
            mon.Width(), mon.Height(),
            mon.bounds.left, mon.bounds.top,
            mon.dpiX,
            static_cast<int>(mon.ScaleFactor() * 100.0f));
        LOG_INFO("      Refresh: {}Hz  HDR: {}  DXGI: Adapter{}/Output{}",
            mon.refreshRate,
            mon.isHDRCapable ? "Yes" : "No",
            mon.adapterIndex,
            mon.outputIndex);
        LOG_INFO("      Zoom: {:.2f}x  Active: {}  Frozen: {}",
            mon.zoom.zoomLevel,
            mon.zoom.isActive ? "Yes" : "No",
            mon.zoom.isFrozen ? "Yes" : "No");
    }
}

} // namespace BetterMagnifier
