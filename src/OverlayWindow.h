#pragma once

// Full-screen click-through overlay, one per monitor. Shown while that
// monitor's zoom is active, hidden otherwise.

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

    bool Create(HINSTANCE hInstance, const MonitorInfo& monitorInfo, size_t monitorIndex);

    void Show();
    void Hide();
    bool IsVisible() const;

    void Reposition(const RECT& bounds);

    // Menus and popups are also HWND_TOPMOST and are created after us, so they
    // end up above the overlay and the user sees them twice: once inside our
    // magnified capture, once as their own unmagnified window. Re-asserting
    // topmost keeps us above them.
    void EnsureTopmost();

    HWND GetHwnd() const { return m_hwnd; }
    size_t GetMonitorIndex() const { return m_monitorIndex; }

    // False means the overlay captures itself, i.e. a feedback loop.
    bool IsExcludedFromCapture() const { return m_excludedFromCapture; }

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
