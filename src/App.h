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
#include "ViewportController.h"
#include "ViewportSnapshot.h"
#include "CursorRenderer.h"
#include "ControlPanel.h"

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

    // Act on whichever monitor holds the cursor.
    void OnToggleZoom();
    void OnFreeze();

    // Same, on a named monitor: the control panel drives one card at a time.
    void ToggleZoomOnMonitor(size_t monitorIndex);

    void OnShowPanel();

    // Turns zoom ON when it is off and direction is positive; turns it OFF
    // when stepping down reaches minZoom.
    void OnZoomStep(int direction);

    void OnDisplayChange();
    void OnFocusChanged(HWND focused);
    void ApplySettings();

    // The live-tunable subset, called at startup as well as on change.
    // ApplySettings is only reachable from WM_APP_SETTINGS_CHANGED, so anything
    // that lives only there never takes effect until the user changes something.
    void ApplyPointerSettings();

    // Locking the workstation switches to the secure desktop, which tears down
    // our low-level hooks; Windows does not put them back. RegisterHotKey
    // bindings survive, the hooks do not, so Win+Plus and Ctrl+Alt+wheel go
    // dead until they are reinstalled.
    void OnSessionUnlock();

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
    ControlPanel                m_controlPanel;

    // The source rectangle and the edge-push pan. Owned here for lifetime, but
    // mutated only on the input thread — see InputThread::Attach.
    ViewportController          m_viewport;
    ViewportSnapshot            m_viewportSnapshot;

    // Push monitor rects and the settled zoom to the input thread. Called every
    // frame; bumpLayout only on a topology change.
    void PublishViewportRequests(bool bumpLayout);

    // Turn pointer compositing on exactly while a monitor is magnified, and
    // only when the pointer can be hidden safely.
    // anyMonitorZoomed drives the one-shot Windows Magnifier clash check;
    // pointerOnMagnified is what the pointer feature itself follows.
    void UpdatePointerCompositing(bool anyMonitorZoomed, bool pointerOnMagnified);

    CursorCache m_cursorCache;
    bool        m_pointerCompositing = false;

    // Sprite size relative to the content scale. Cached rather than read from
    // SettingsStore in the draw call: the panel thread writes that struct and
    // only then posts WM_APP_SETTINGS_CHANGED, so a per-frame read would be
    // racing it. ApplyPointerSettings is the far side of that message.
    float m_cursorScale = 1.0f;

    // Latched once the sprite has proved it cannot be drawn. Without the latch
    // UpdatePointerCompositing would turn the feature straight back on next
    // frame and hide the pointer again, so the failure would loop instead of
    // stopping.
    bool m_pointerCompositingBroken = false;

    int m_spriteFailures = 0;
    static constexpr int kSpriteFailureLimit = 30;

    // Edge-triggers the Windows Magnifier clash check on the transition into
    // magnifying, rather than paying for a FindWindow every frame.
    bool m_wasZoomed = false;

    std::array<std::chrono::steady_clock::time_point, StatusSnapshot::kMaxMonitors> m_lastFrameTime{};

    // Last presented source region. If neither the screen nor the anchor
    // changed there is nothing new to show, and presenting anyway just waits
    // on vSync and burns GPU.
    std::array<RECT, StatusSnapshot::kMaxMonitors> m_lastSrcRect{};

    // The sprite is part of what is on screen, so it is part of "did anything
    // change". Without it, edge-push holding the source rect still — which is
    // the entire feature — froze the magnified pointer mid-move.
    std::array<POINT, StatusSnapshot::kMaxMonitors>       m_lastSpritePos{};
    std::array<const void*, StatusSnapshot::kMaxMonitors> m_lastSpriteShape{};

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
