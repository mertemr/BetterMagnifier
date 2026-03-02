#pragma once

// =============================================================================
// App.h — Ana Uygulama Sinifi (Orkestrator)
// =============================================================================
// Tum component'lari (MonitorManager, DXGICapture, D3DRenderer, Overlay,
// HotkeyManager, TrayIcon) init eden ve yoneten RAII sinifi.
// =============================================================================

#ifndef BETTER_MAGNIFIER_APP_H
#define BETTER_MAGNIFIER_APP_H

#include "MonitorManager.h"
#include "DXGICapture.h"
#include "D3DRenderer.h"
#include "OverlayWindow.h"
#include "HotkeyManager.h"
#include "TrayIcon.h"

#include <windows.h>
#include <vector>
#include <memory>

namespace BetterMagnifier {

class App
{
public:
    App() = default;
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ── Lifecycle ──
    bool Initialize(HINSTANCE hInstance);
    int  Run();
    void Shutdown();

private:
    // ── Internal Setup ──
    bool CreateMessageWindow();
    bool InitializeComponents();
    void SetupCallbacks();

    // ── Per-Frame ──
    void Update();
    void RenderMonitor(size_t monitorIndex);

    // ── Event Handlers ──
    void OnToggleZoom();
    void OnFreeze();
    void OnScroll(int delta, POINT mousePos);
    void OnDisplayChange();

    // ── Message Window WndProc ──
    static LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ── Components ──
    HINSTANCE                   m_hInstance = nullptr;
    HWND                        m_messageHwnd = nullptr;

    MonitorManager              m_monitorManager;
    D3DRenderer                 m_renderer;
    HotkeyManager               m_hotkeyManager;
    TrayIcon                    m_trayIcon;
    std::vector<DXGICapture>    m_captures;
    std::vector<OverlayWindow>  m_overlays;

    bool m_running     = false;
    bool m_initialized = false;

    // Static instance for WndProc
    static App* s_instance;

    static constexpr wchar_t kMsgWindowClass[] = L"BetterMagnifierMsg";
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_APP_H
