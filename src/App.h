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
#include "AppMessages.h"
#include "StatusSnapshot.h"
#include "SettingsStore.h"
#include "InputThread.h"

#include <windows.h>
#include <vector>
#include <memory>
#include <array>
#include <chrono>

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

    // GUI thread bu pointer'lari okur. App yasadigi surece gecerli.
    StatusSnapshot* Status()   { return &m_status; }
    SettingsStore*  Settings() { return &m_settings; }

private:
    // ── Internal Setup ──
    bool CreateMessageWindow();
    bool InitializeComponents();
    void SetupCallbacks();

    // Snapshot'in statik monitor bilgilerini doldur (init ve display change'de)
    void PublishMonitorInfo();

    // ── Per-Frame ──
    void Update();
    void RenderMonitor(size_t monitorIndex);

    // ── Event Handlers ──
    void OnToggleZoom();
    void OnFreeze();
    void OnScroll(int delta, POINT mousePos);
    void OnDisplayChange();
    void OnFocusChanged(HWND focused);

    // Ayarlar degistiginde uygula (WM_APP_SETTINGS_CHANGED)
    void ApplySettings();

    // wParam'i monitor indeksine cevir. kFocusedMonitor = farenin oldugu monitor.
    bool ResolveMonitorIndex(WPARAM wparam, size_t& outIndex) const;

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

    SettingsStore               m_settings;
    InputThread                 m_inputThread;

    // Fare gercekten hareket etti mi? Klavye odagi takibi ile cakismasin diye.
    POINT m_lastCursorPos{ -1, -1 };

    // GUI thread'in okudugu canli durum. Render thread her frame yazar.
    StatusSnapshot              m_status;

    // FPS olcumu — monitor basina son frame zamani
    std::array<std::chrono::steady_clock::time_point, StatusSnapshot::kMaxMonitors> m_lastFrameTime{};

    bool m_running     = false;
    bool m_initialized = false;

    // Static instance for WndProc
    static App* s_instance;

    static constexpr wchar_t kMsgWindowClass[] = L"BetterMagnifierMsg";
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_APP_H
