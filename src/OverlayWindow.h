#pragma once

// =============================================================================
// OverlayWindow.h — Per-Monitor Transparent Overlay
// =============================================================================
// Her monitoru tam kaplayan, seffaf, click-through overlay penceresi.
// Zoom aktif oldugunda gorunur, pasif oldugunda gizlenir.
// =============================================================================

#ifndef BETTER_MAGNIFIER_OVERLAY_WINDOW_H
#define BETTER_MAGNIFIER_OVERLAY_WINDOW_H

#include <windows.h>
#include <string>

namespace BetterMagnifier {

struct MonitorInfo;

class OverlayWindow
{
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;
    OverlayWindow(OverlayWindow&& other) noexcept;
    OverlayWindow& operator=(OverlayWindow&& other) noexcept;

    // Monitor bilgileriyle overlay penceresi olustur
    bool Create(HINSTANCE hInstance, const MonitorInfo& monitorInfo, size_t monitorIndex);

    // Gorunurluk
    void Show();
    void Hide();
    bool IsVisible() const;

    // Boyut guncelleme (DPI/resolution degisimi)
    void Reposition(const RECT& bounds);

    HWND GetHwnd() const { return m_hwnd; }
    size_t GetMonitorIndex() const { return m_monitorIndex; }

    // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) basarili miydi?
    // false ise overlay kendini yakalar (feedback loop). Panelde uyari gosterilir.
    bool IsExcludedFromCapture() const { return m_excludedFromCapture; }

    // Window class kaydi (bir kez yapilir)
    static bool RegisterWindowClass(HINSTANCE hInstance);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND    m_hwnd                = nullptr;
    size_t  m_monitorIndex        = 0;
    bool    m_visible             = false;
    bool    m_excludedFromCapture = false;

    static constexpr wchar_t kClassName[] = L"BetterMagnifierOverlay";
    static bool s_classRegistered;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_OVERLAY_WINDOW_H
