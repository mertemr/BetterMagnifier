#pragma once

// =============================================================================
// TrayIcon.h — System Tray Icon + Context Menu
// =============================================================================

#ifndef BETTER_MAGNIFIER_TRAY_ICON_H
#define BETTER_MAGNIFIER_TRAY_ICON_H

#include <windows.h>
#include <shellapi.h>
#include <functional>

namespace BetterMagnifier {

class TrayIcon
{
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // ── Initialization ──
    bool Create(HWND hwnd, HINSTANCE hInstance);
    void Destroy();

    // ── Tooltip guncelle ──
    void UpdateTooltip(const wchar_t* text);

    // ── WM_APP+1 mesajini isle ──
    void HandleMessage(WPARAM wParam, LPARAM lParam);

    // ── Callbacks ──
    void SetToggleCallback(std::function<void()> cb)  { m_onToggle = std::move(cb); }
    void SetExitCallback(std::function<void()> cb)    { m_onExit = std::move(cb); }

    static constexpr UINT kTrayCallbackMsg = WM_APP + 1;
    static constexpr UINT kTrayIconId      = 1;

    // Menu item ID'leri
    static constexpr UINT kMenuToggle  = 1001;
    static constexpr UINT kMenuExit    = 1002;

private:
    void ShowContextMenu();

    HWND              m_hwnd = nullptr;
    NOTIFYICONDATAW   m_nid{};
    bool              m_created = false;

    std::function<void()> m_onToggle;
    std::function<void()> m_onExit;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_TRAY_ICON_H
