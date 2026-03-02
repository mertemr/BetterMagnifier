#pragma once

// =============================================================================
// MonitorManager.h — Multi-Monitor Enumeration & Per-Monitor State
// =============================================================================
//
// Python analojisi:
//   Python'da: screeninfo.get_monitors() veya pyautogui.size()
//   Burada:    MonitorManager::EnumerateMonitors() + per-monitor zoom state
//
// Bu sinif ne yapar:
//   1. Tum fiziksel monitorleri enumerate eder (EnumDisplayMonitors API)
//   2. Her monitor icin: HMONITOR, RECT, DPI, refresh rate, HDR bilgisi tutar
//   3. DXGI adapter-output eslestirmesi yapar (hangi GPU'nun hangi monitoru)
//   4. Monitor takilip cikarildiginda (WM_DISPLAYCHANGE) listeyi yeniler
//   5. Her monitor icin bagimsiz zoom state tutar (zoom level, focal point, aktif mi)
//
// Neden onemli:
//   Windows Magnifier'in en buyuk eksikligi: TUM monitorleri ayni zoom'da 
//   buyutmesi. Biz her monitorde BAGIMSIZ zoom seviyesi istiyoruz.
//   Bu sinif, her monitore ait state'i ayri ayri yonetir.
//
// =============================================================================

#ifndef BETTER_MAGNIFIER_MONITOR_MANAGER_H
#define BETTER_MAGNIFIER_MONITOR_MANAGER_H

#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <functional>
#include <windows.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

namespace BetterMagnifier {

// ─────────────────────────────────────────────────────────────────────────────
// Per-Monitor Zoom State
// ─────────────────────────────────────────────────────────────────────────────
// Python analojisi:
//   @dataclass
//   class ZoomState:
//       zoom_level: float = 1.0
//       is_active: bool = False
//       focal_x: int = 0
//       focal_y: int = 0
//
// Her monitor icin ayri bir ZoomState var.
// zoomLevel = 1.0 → normal gorunum (zoom yok)
// zoomLevel = 2.0 → 2x buyutme
// focalPoint = zoom'un merkez noktasi (genelde mouse pozisyonu)
// ─────────────────────────────────────────────────────────────────────────────
struct ZoomState
{
    float zoomLevel     = 1.0f;     // 1.0 = normal, 2.0 = 2x, max 10.0
    bool  isActive      = false;    // Bu monitorde zoom aktif mi?
    POINT focalPoint    = {0, 0};   // Zoom merkezi (ekran koordinatlari)
    float targetZoom    = 1.0f;     // Smooth zoom icin hedef deger
    bool  isFrozen      = false;    // Freeze/pin modu aktif mi?

    static constexpr float kMinZoom = 1.0f;
    static constexpr float kMaxZoom = 10.0f;
    static constexpr float kZoomStep = 0.25f;  // Scroll wheel basina zoom miktari
};

// ─────────────────────────────────────────────────────────────────────────────
// Monitor Info — Tek bir fiziksel monitorun tum bilgileri
// ─────────────────────────────────────────────────────────────────────────────
// Python analojisi:
//   @dataclass
//   class MonitorInfo:
//       handle: int
//       rect: tuple[int, int, int, int]  
//       name: str
//       dpi: int
//       refresh_rate: int
//       is_primary: bool
//       hdr_capable: bool
//       zoom: ZoomState
//
// HMONITOR = Windows'un monitore verdigi benzersiz kimlik (Python'daki id() gibi)
// RECT = {left, top, right, bottom} — monitorun ekran koordinatlari
// ─────────────────────────────────────────────────────────────────────────────
struct MonitorInfo
{
    // Windows Handle
    HMONITOR            hMonitor = nullptr;

    // Geometri
    RECT                bounds   = {};          // Monitor sinirlarinin ekran koordinatlari
    RECT                workArea = {};          // Taskbar haric kullanilabilir alan

    // Identification
    std::wstring        deviceName;             // "\\\\.\\DISPLAY1" gibi
    std::wstring        friendlyName;           // "DELL U2723QE" gibi (varsa)
    bool                isPrimary = false;

    // Display Properties
    UINT                dpiX = 96;              // Yatay DPI (96 = %100 scaling)
    UINT                dpiY = 96;              // Dikey DPI
    UINT                refreshRate = 60;       // Hz cinsinden
    bool                isHDRCapable = false;   // HDR destegi var mi?

    // DXGI Eslestirmesi
    UINT                adapterIndex = 0;       // Hangi GPU (multi-GPU icin)
    UINT                outputIndex  = 0;       // GPU'nun hangi cikisi
    Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;  // DXGI output referansi

    // Per-Monitor Zoom
    ZoomState           zoom;

    // Yardimci fonksiyonlar
    int Width() const  { return bounds.right - bounds.left; }
    int Height() const { return bounds.bottom - bounds.top; }
    float AspectRatio() const { return static_cast<float>(Width()) / static_cast<float>(Height()); }
    float ScaleFactor() const { return static_cast<float>(dpiX) / 96.0f; }
};

// ─────────────────────────────────────────────────────────────────────────────
// MonitorManager Class
// ─────────────────────────────────────────────────────────────────────────────
class MonitorManager
{
public:
    MonitorManager() = default;
    ~MonitorManager() = default;

    // Kopyalama/tasima yasak (COM pointer'lar icerdiginden)
    MonitorManager(const MonitorManager&) = delete;
    MonitorManager& operator=(const MonitorManager&) = delete;

    // ── Initialization ──
    // Tum monitorleri enumerate eder ve DXGI eslestirmesini yapar.
    // Basarisiz olursa false doner.
    bool Initialize();

    // ── Refresh ──
    // WM_DISPLAYCHANGE geldiginde cagirilir (monitor takildi/cikarildi/cozunurluk degisti).
    // Mevcut zoom state'leri koruyarak listeyi yeniler.
    void Refresh();

    // ── Monitor Access ──
    const std::vector<MonitorInfo>& GetMonitors() const { return m_monitors; }
    size_t GetMonitorCount() const { return m_monitors.size(); }

    // Index ile monitor al (bounds check'li)
    MonitorInfo* GetMonitor(size_t index);
    const MonitorInfo* GetMonitor(size_t index) const;

    // HMONITOR handle'i ile monitor bul
    MonitorInfo* FindByHandle(HMONITOR hMon);

    // Bir noktanin hangi monitorde oldugunu bul (mouse pozisyonu icin)
    MonitorInfo* FindByPoint(POINT pt);

    // Primary monitoru al
    MonitorInfo* GetPrimaryMonitor();

    // ── Zoom Control ──
    // Belirtilen monitorde zoom'u degistir.
    // delta: pozitif = zoom in, negatif = zoom out
    void AdjustZoom(size_t monitorIndex, float delta);
    void SetZoom(size_t monitorIndex, float level);
    void ToggleZoom(size_t monitorIndex);
    void ToggleFreezeOnMonitor(size_t monitorIndex);

    // ── Debug ──
    // Tum monitorlerin bilgilerini log'a yaz
    void LogAllMonitors() const;

private:
    // ── Internal Methods ──
    // EnumDisplayMonitors callback'i icin static fonksiyon
    static BOOL CALLBACK EnumMonitorCallback(HMONITOR hMon, HDC hDC, LPRECT lpRect, LPARAM lParam);

    // Tek bir monitor icin detayli bilgileri doldur
    void PopulateMonitorDetails(MonitorInfo& info);

    // DXGI adapter/output eslestirmesi
    bool MatchDXGIOutputs();

    // ── Data ──
    std::vector<MonitorInfo> m_monitors;
    mutable std::mutex       m_mutex;   // Thread-safe erisim icin
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_MONITOR_MANAGER_H
