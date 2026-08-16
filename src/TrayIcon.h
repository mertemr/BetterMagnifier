#pragma once

// System tray icon and its context menu.

#ifndef BETTER_MAGNIFIER_TRAY_ICON_H
#define BETTER_MAGNIFIER_TRAY_ICON_H

#include "AppMessages.h"

#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <string>

namespace BetterMagnifier {

class TrayIcon
{
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Create(HWND hwnd, HINSTANCE hInstance);
    void Destroy();

    // The tray is the only place the application is visible while it is idle,
    // and until this existed it showed the generic Windows icon and a tooltip
    // that never changed. Called every frame from App::Update with the settled
    // state; it compares and returns when nothing moved, so the cost of that is
    // two integer compares and no shell call.
    //
    //   activeCount  how many monitors are magnified right now
    //   zoom         that monitor's level, meaningful only when activeCount==1
    void SetState(size_t activeCount, float zoom);

    // Handles kTrayCallbackMsg
    void HandleMessage(WPARAM wParam, LPARAM lParam);

    // A balloon rather than a dialog: there is no window to interrupt, and a
    // modal over a magnified screen is the last thing a low-vision user wants.
    void ShowUpdateBalloon(const std::wstring& version);

    void SetToggleCallback(std::function<void()> cb)   { m_onToggle = std::move(cb); }
    void SetSettingsCallback(std::function<void()> cb) { m_onSettings = std::move(cb); }
    void SetExitCallback(std::function<void()> cb)     { m_onExit = std::move(cb); }

    static constexpr UINT kTrayCallbackMsg = WM_APP_TRAY;
    static constexpr UINT kTrayIconId      = 1;

    static constexpr UINT kMenuToggle   = 1001;
    static constexpr UINT kMenuExit     = 1002;
    static constexpr UINT kMenuSettings = 1003;

private:
    void ShowContextMenu();

    HWND            m_hwnd = nullptr;
    HINSTANCE       m_hInstance = nullptr;
    NOTIFYICONDATAW m_nid{};
    bool            m_created = false;

    // Loaded once. LoadIcon returns a shared handle from the module's resources
    // and must not be destroyed, which is why there is no cleanup for these.
    HICON m_iconOn  = nullptr;
    HICON m_iconOff = nullptr;

    // Last published state, so SetState can be called at frame rate. A
    // Shell_NotifyIcon per frame is a round trip to explorer.exe and shows up
    // as tray flicker.
    size_t m_lastActiveCount = static_cast<size_t>(-1);
    float  m_lastZoom        = -1.0f;

    std::function<void()> m_onToggle;
    std::function<void()> m_onSettings;
    std::function<void()> m_onExit;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_TRAY_ICON_H
