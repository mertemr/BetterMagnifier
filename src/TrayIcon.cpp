// =============================================================================
// TrayIcon.cpp — System Tray Icon Implementation
// =============================================================================

#include "pch.h"
#include "TrayIcon.h"
#include "resource.h"
#include "Logger.h"

namespace BetterMagnifier {

// =============================================================================
// Destructor
// =============================================================================
TrayIcon::~TrayIcon()
{
    Destroy();
}

// =============================================================================
// Create — add the icon to the system tray
// =============================================================================
bool TrayIcon::Create(HWND hwnd, HINSTANCE hInstance)
{
    m_hwnd      = hwnd;
    m_hInstance = hInstance;

    // Loaded at the tray's own metric rather than through LoadIcon, which hands
    // back the 32 px entry and leaves the shell to shrink it. A downscaled 32
    // is visibly softer than the 16 that was drawn to be read at that size.
    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);

    auto load = [&](int id) -> HICON {
        return static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(id),
                                             IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    };

    m_iconOn  = load(IDI_TRAY_ON);
    m_iconOff = load(IDI_TRAY_OFF);

    // A missing resource must not cost the user their tray entry — without it
    // there is no menu and no way to quit but the panic shortcut.
    if (!m_iconOn || !m_iconOff)
    {
        LOG_WARN("Tray icon resources could not be loaded ({}), falling back to "
                 "the system icon", GetLastError());
        HICON fallback = LoadIconW(nullptr, IDI_APPLICATION);
        if (!m_iconOn)  m_iconOn  = fallback;
        if (!m_iconOff) m_iconOff = fallback;
    }

    ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = hwnd;
    m_nid.uID              = kTrayIconId;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = kTrayCallbackMsg;
    m_nid.hIcon            = m_iconOff;

    wcscpy_s(m_nid.szTip, L"BetterMagnifier - off");

    if (!Shell_NotifyIconW(NIM_ADD, &m_nid))
    {
        LOG_ERROR("Shell_NotifyIcon(NIM_ADD) failed: {}", GetLastError());
        return false;
    }

    // NOTIFYICON_VERSION_4: the modern callback and notification behaviour.
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);

    m_created = true;
    LOG_INFO("System tray icon created");
    return true;
}

// =============================================================================
// SetState — icon and tooltip follow the engine
// =============================================================================
//
// One number cannot describe per-monitor zoom honestly, so it does not try: the
// level is shown only when exactly one monitor is magnified, and otherwise the
// count is. Reporting, say, the highest level would read as a single global
// zoom, which is precisely the thing this application is not.
// =============================================================================
void TrayIcon::SetState(size_t activeCount, float zoom)
{
    if (!m_created)
        return;

    // Quantised before comparing, so a zoom drifting in the third decimal does
    // not push a shell call every frame.
    const float quantised = std::round(zoom * 100.0f) / 100.0f;

    if (activeCount == m_lastActiveCount && quantised == m_lastZoom)
        return;

    const bool wasActive = (m_lastActiveCount != static_cast<size_t>(-1))
                        && (m_lastActiveCount > 0);

    m_lastActiveCount = activeCount;
    m_lastZoom        = quantised;

    std::wstring tip;
    if (activeCount == 0)
        tip = L"BetterMagnifier - off";
    else if (activeCount == 1)
        tip = std::format(L"BetterMagnifier - {:.2f}x", quantised);
    else
        tip = std::format(L"BetterMagnifier - {} displays magnified", activeCount);

    // szTip is 128 wchars. None of the strings above reach that, but truncating
    // beats the silent overrun wcscpy_s would abort on.
    wcsncpy_s(m_nid.szTip, tip.c_str(), _TRUNCATE);

    const bool nowActive = (activeCount > 0);

    m_nid.uFlags = NIF_TIP;
    if (nowActive != wasActive)
    {
        m_nid.uFlags |= NIF_ICON;
        m_nid.hIcon = nowActive ? m_iconOn : m_iconOff;
    }

    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

// =============================================================================
// Destroy
// =============================================================================
void TrayIcon::Destroy()
{
    if (m_created)
    {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_created = false;
        LOG_DEBUG("System tray icon removed");
    }
}

// =============================================================================
// HandleMessage — dispatch what the shell sends to kTrayCallbackMsg
// =============================================================================
void TrayIcon::HandleMessage(WPARAM /*wParam*/, LPARAM lParam)
{
    UINT msg = LOWORD(lParam);

    switch (msg)
    {
    case WM_RBUTTONDOWN:
    case WM_CONTEXTMENU:
        ShowContextMenu();
        break;

    case WM_LBUTTONDBLCLK:
        // Double click toggles zoom
        if (m_onToggle) m_onToggle();
        break;

    default:
        break;
    }
}

// =============================================================================
// ShowContextMenu
// =============================================================================
void TrayIcon::ShowContextMenu()
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, kMenuToggle, L"Toggle Zoom (Ctrl+Alt+Z)");

    // Guarded because the callback is optional, not because the panel is: App
    // wires it up unconditionally now.
    if (m_onSettings)
        AppendMenuW(hMenu, MF_STRING, kMenuSettings, L"Settings...");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, kMenuExit, L"Exit");

    POINT pt;
    GetCursorPos(&pt);

    // Required before TrackPopupMenu: without a foreground owner the menu does
    // not dismiss when you click away from it.
    SetForegroundWindow(m_hwnd);

    UINT cmd = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        pt.x, pt.y, 0, m_hwnd, nullptr);

    DestroyMenu(hMenu);

    switch (cmd)
    {
    case kMenuToggle:
        if (m_onToggle) m_onToggle();
        break;
    case kMenuSettings:
        if (m_onSettings) m_onSettings();
        break;
    case kMenuExit:
        if (m_onExit) m_onExit();
        break;
    }
}

} // namespace BetterMagnifier
