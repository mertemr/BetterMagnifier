#pragma once

// Application orchestrator. Owns and sequences every component; does no real
// work itself.

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

    bool Initialize(HINSTANCE hInstance);
    int  Run();
    void Shutdown();

    // Valid for as long as App lives.
    StatusSnapshot* Status()   { return &m_status; }
    SettingsStore*  Settings() { return &m_settings; }

private:
    bool CreateMessageWindow();
    bool InitializeComponents();
    void SetupCallbacks();

    // Fill the snapshot's static monitor fields (init and display change).
    void PublishMonitorInfo();

    // Rate-limited: EVENT_OBJECT_SHOW fires constantly and calling
    // SetWindowPos on every one of them is z-order noise.
    void AssertOverlaysTopmost();

    void Update();
    void RenderMonitor(size_t monitorIndex);

    void OnToggleZoom();
    void OnFreeze();

    // Turns zoom ON when it is off and direction is positive; turns it OFF
    // when stepping down reaches minZoom.
    void OnZoomStep(int direction);

    void OnDisplayChange();
    void OnFocusChanged(HWND focused);
    void ApplySettings();

    // kFocusedMonitor resolves to whichever monitor holds the cursor.
    bool ResolveMonitorIndex(WPARAM wparam, size_t& outIndex) const;

    static LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HINSTANCE                   m_hInstance   = nullptr;
    HWND                        m_messageHwnd = nullptr;

    MonitorManager              m_monitorManager;
    D3DRenderer                 m_renderer;
    HotkeyManager               m_hotkeyManager;
    TrayIcon                    m_trayIcon;
    std::vector<DXGICapture>    m_captures;
    std::vector<OverlayWindow>  m_overlays;

    SettingsStore               m_settings;
    InputThread                 m_inputThread;
    StatusSnapshot              m_status;

    // Did the cursor actually move? Without this the per-frame mouse tracking
    // overwrites whatever focus tracking just set.
    POINT m_lastCursorPos{ -1, -1 };

    std::array<std::chrono::steady_clock::time_point, StatusSnapshot::kMaxMonitors> m_lastFrameTime{};

    // Last presented source region. If neither the screen nor the anchor
    // changed there is nothing new to show, and presenting anyway just waits
    // on vSync and burns GPU.
    std::array<RECT, StatusSnapshot::kMaxMonitors> m_lastSrcRect{};

    // Nothing presented this tick means vSync did not pace the loop, so it
    // needs an explicit sleep.
    bool m_presentedThisTick = false;

    std::chrono::steady_clock::time_point m_lastTopmostAssert{};

    bool m_running     = false;
    bool m_initialized = false;

    static App* s_instance;   // for the static WndProc

    static constexpr wchar_t kMsgWindowClass[] = L"BetterMagnifierMsg";
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_APP_H
