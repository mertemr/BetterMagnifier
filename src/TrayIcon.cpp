// =============================================================================
// TrayIcon.cpp — System Tray Icon Implementation
// =============================================================================

#include "pch.h"
#include "TrayIcon.h"
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
// Create — System tray'e icon ekle
// =============================================================================
bool TrayIcon::Create(HWND hwnd, HINSTANCE /*hInstance*/)
{
    m_hwnd = hwnd;

    ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = hwnd;
    m_nid.uID              = kTrayIconId;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = kTrayCallbackMsg;

    // Sistem varsayilan icon'unu kullan (ileride ozel icon eklenecek)
    m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    wcscpy_s(m_nid.szTip, L"BetterMagnifier - Zoom: Pasif");

    if (!Shell_NotifyIconW(NIM_ADD, &m_nid))
    {
        LOG_ERROR("Shell_NotifyIcon(NIM_ADD) basarisiz: {}", GetLastError());
        return false;
    }

    // NOTIFYICON_VERSION_4: Modern bildirim davranisi (Win Vista+)
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);

    m_created = true;
    LOG_INFO("System tray icon olusturuldu");
    return true;
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
        LOG_DEBUG("System tray icon kaldirildi");
    }
}

// =============================================================================
// UpdateTooltip
// =============================================================================
void TrayIcon::UpdateTooltip(const wchar_t* text)
{
    if (!m_created) return;

    wcscpy_s(m_nid.szTip, text);
    m_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

// =============================================================================
// HandleMessage — Tray icon'dan gelen mesajlari isle
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
        // Cift tiklama = toggle zoom
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

    // Only when something is wired up: the control panel is behind BM_PANEL=1.
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
