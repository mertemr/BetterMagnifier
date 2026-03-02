// =============================================================================
// MonitorManager.cpp — Multi-Monitor Enumeration & State Management
// =============================================================================

#include "pch.h"
#include "MonitorManager.h"
#include "Logger.h"

namespace BetterMagnifier {

// =============================================================================
// Initialize — Tum monitorleri bul ve DXGI ile esle
// =============================================================================
bool MonitorManager::Initialize()
{
    LOG_INFO("MonitorManager baslatiliyor...");

    m_monitors.clear();

    // ── 1. EnumDisplayMonitors ile tum monitorleri listele ──
    // Python analojisi: screeninfo.get_monitors()
    // Windows bu callback fonksiyonunu her monitor icin bir kez cagirir.
    // lParam ile "this" pointer'ini geciyoruz ki callback icinden
    // m_monitors vector'une erisim olsun.
    if (!EnumDisplayMonitors(nullptr, nullptr, EnumMonitorCallback, reinterpret_cast<LPARAM>(this)))
    {
        LOG_ERROR("EnumDisplayMonitors basarisiz!");
        return false;
    }

    if (m_monitors.empty())
    {
        LOG_ERROR("Hic monitor bulunamadi!");
        return false;
    }

    LOG_INFO("{} adet monitor bulundu", m_monitors.size());

    // ── 2. Her monitor icin detayli bilgileri doldur ──
    for (auto& mon : m_monitors)
    {
        PopulateMonitorDetails(mon);
    }

    // ── 3. DXGI adapter/output eslestirmesi ──
    if (!MatchDXGIOutputs())
    {
        LOG_WARN("DXGI output eslestirmesi kismen basarisiz — bazi monitorler capture edilemeyebilir");
        // Kritik degil, devam edebiliriz
    }

    LogAllMonitors();

    LOG_INFO("MonitorManager basariyla baslatildi");
    return true;
}

// =============================================================================
// Refresh — Monitor listesini yenile (WM_DISPLAYCHANGE)
// =============================================================================
void MonitorManager::Refresh()
{
    LOG_INFO("Monitor listesi yenileniyor (display change)...");

    std::lock_guard lock(m_mutex);

    // Mevcut zoom state'leri koru — yeni listeye aktarilacak
    // Python analojisi: 
    //   old_states = {mon.device_name: mon.zoom for mon in self.monitors}
    std::unordered_map<std::wstring, ZoomState> savedZoomStates;
    for (const auto& mon : m_monitors)
    {
        savedZoomStates[mon.deviceName] = mon.zoom;
    }

    // Yeniden enumerate et
    m_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorCallback, reinterpret_cast<LPARAM>(this));

    for (auto& mon : m_monitors)
    {
        PopulateMonitorDetails(mon);

        // Eski zoom state'i geri yukle (ayni device name ile eslesen monitor varsa)
        auto it = savedZoomStates.find(mon.deviceName);
        if (it != savedZoomStates.end())
        {
            mon.zoom = it->second;
            LOG_DEBUG("Zoom state korundu: {} -> level={:.2f}", 
                std::string(mon.deviceName.begin(), mon.deviceName.end()),
                mon.zoom.zoomLevel);
        }
    }

    MatchDXGIOutputs();
    LogAllMonitors();
}

// =============================================================================
// EnumMonitorCallback — Her monitor icin Windows tarafindan cagirilir
// =============================================================================
// Bu bir "static" fonksiyon — class instance'ina erisimi yok.
// Bu yuzden lParam ile "this" pointer'ini geciyoruz.
// Python'da: lambda self=self: self.monitors.append(...)
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

    return TRUE;  // TRUE = devam et (sonraki monitor), FALSE = dur
}

// =============================================================================
// PopulateMonitorDetails — Tek bir monitor icin detayli bilgileri doldur
// =============================================================================
void MonitorManager::PopulateMonitorDetails(MonitorInfo& info)
{
    // ── 1. MONITORINFOEX ile temel bilgiler ──
    // Python'da: win32api.GetMonitorInfo(hMonitor)
    MONITORINFOEXW monInfo{};
    monInfo.cbSize = sizeof(monInfo);

    if (GetMonitorInfoW(info.hMonitor, &monInfo))
    {
        info.bounds    = monInfo.rcMonitor;
        info.workArea  = monInfo.rcWork;
        info.deviceName = monInfo.szDevice;     // L"\\\\.\\DISPLAY1" gibi
        info.isPrimary = (monInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
    }
    else
    {
        LOG_WARN("GetMonitorInfo basarisiz, HMONITOR: 0x{:X}",
            reinterpret_cast<uintptr_t>(info.hMonitor));
    }

    // ── 2. Per-Monitor DPI ──
    // Windows 8.1+ API — her monitörun gerçek DPI değerini alır.
    // 96 DPI = %100 scaling, 144 DPI = %150, 192 DPI = %200
    // Bu değer zoom hesaplamasında ve overlay boyutlandırmada lazım.
    UINT dpiX = 96, dpiY = 96;
    HRESULT hr = GetDpiForMonitor(info.hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    if (SUCCEEDED(hr))
    {
        info.dpiX = dpiX;
        info.dpiY = dpiY;
    }
    else
    {
        LOG_WARN("GetDpiForMonitor basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
    }

    // ── 3. Refresh Rate ──
    // DEVMODE yapisinda monitorun refresh rate'i (Hz) var.
    // 60Hz, 144Hz, 240Hz gibi — v-sync icin lazim.
    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);

    if (EnumDisplaySettingsW(info.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &devMode))
    {
        info.refreshRate = devMode.dmDisplayFrequency;
    }
    else
    {
        LOG_WARN("EnumDisplaySettings basarisiz: {}", 
            std::string(info.deviceName.begin(), info.deviceName.end()));
        info.refreshRate = 60;  // Fallback
    }

    // ── 4. Focal Point baslangici — monitorun merkezi ──
    info.zoom.focalPoint.x = info.bounds.left + info.Width() / 2;
    info.zoom.focalPoint.y = info.bounds.top + info.Height() / 2;
}

// =============================================================================
// MatchDXGIOutputs — DXGI adapter/output'lari fiziksel monitorlerle esle
// =============================================================================
// Neden gerekli?
// Desktop Duplication API "IDXGIOutput" bazinda calisir.
// Hangi IDXGIOutput'un hangi fiziksel monitore denk geldigini bilmemiz lazim.
// Eslestirme: DXGI_OUTPUT_DESC.Monitor == MonitorInfo.hMonitor
//
// COM Release Sirasi (KRITIK!):
//   Bu fonksiyonda olusturulan COM objeleri (Factory, Adapter, Output):
//   - ComPtr<T> kullanıyoruz → scope bitince otomatik Release
//   - Manuel Release yapmaya GEREK YOK (Python GC gibi dusun)
//   - Ama Output pointer'ini MonitorInfo'ya kaydediyoruz → 
//     onun omru MonitorInfo'nun omru kadar uzar
// =============================================================================
bool MonitorManager::MatchDXGIOutputs()
{
    LOG_INFO("DXGI output eslestirmesi basliyor...");

    // DXGI Factory olustur — tum GPU adapter'larina erisim noktası
    // Python analojisi: factory = DXGIFactory.create()
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        LOG_ERROR("CreateDXGIFactory1 basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    bool anyMatched = false;

    // Her GPU adapter'ini iterate et
    // Python analojisi: for i, adapter in enumerate(factory.get_adapters()):
    for (UINT adapterIdx = 0; ; adapterIdx++)
    {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapterIdx, &adapter);

        if (hr == DXGI_ERROR_NOT_FOUND)
            break;  // Daha fazla adapter yok

        if (FAILED(hr))
        {
            LOG_WARN("EnumAdapters1({}) basarisiz: 0x{:08X}", adapterIdx, static_cast<unsigned long>(hr));
            continue;
        }

        // Adapter bilgilerini logla
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        LOG_INFO("  GPU {}: {} (VRAM: {} MB)",
            adapterIdx,
            std::string(adapterDesc.Description, adapterDesc.Description + wcslen(adapterDesc.Description)),
            adapterDesc.DedicatedVideoMemory / (1024 * 1024));

        // Her adapter'in output'larini iterate et
        // Python analojisi: for j, output in enumerate(adapter.get_outputs()):
        for (UINT outputIdx = 0; ; outputIdx++)
        {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(outputIdx, &output);

            if (hr == DXGI_ERROR_NOT_FOUND)
                break;  // Bu adapter'de daha fazla output yok

            if (FAILED(hr))
            {
                LOG_WARN("EnumOutputs({}) basarisiz: 0x{:08X}", outputIdx, static_cast<unsigned long>(hr));
                continue;
            }

            // Output'un hangi monitore bagli oldugunu bul
            DXGI_OUTPUT_DESC outputDesc{};
            output->GetDesc(&outputDesc);

            // MonitorInfo'larla esle: outputDesc.Monitor == info.hMonitor
            for (auto& mon : m_monitors)
            {
                if (mon.hMonitor == outputDesc.Monitor)
                {
                    mon.adapterIndex = adapterIdx;
                    mon.outputIndex  = outputIdx;
                    mon.dxgiOutput   = output;
                    anyMatched = true;

                    LOG_INFO("    Output {} -> {} (Adapter {})",
                        outputIdx,
                        std::string(mon.deviceName.begin(), mon.deviceName.end()),
                        adapterIdx);
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

// Bir noktanin hangi monitorde oldugunu bul
// Python analojisi: 
//   next((m for m in monitors if m.rect.contains(point)), None)
MonitorInfo* MonitorManager::FindByPoint(POINT pt)
{
    for (auto& mon : m_monitors)
    {
        if (PtInRect(&mon.bounds, pt))
            return &mon;
    }

    // Hicbir monitorde degilse, Windows'un MonitorFromPoint'ini kullan
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
    // Primary yoksa ilkini don
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
    LOG_INFO("Monitor {} zoom {}", monitorIndex, mon->zoom.isActive ? "AKTIF" : "PASIF");

    // Zoom pasif olursa level'i sifirla
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
    LOG_INFO("=== Monitor Listesi ({} adet) ===", m_monitors.size());

    for (size_t i = 0; i < m_monitors.size(); i++)
    {
        const auto& mon = m_monitors[i];
        std::string name(mon.deviceName.begin(), mon.deviceName.end());
        
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
