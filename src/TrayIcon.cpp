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
    if (!m_created || !text) return;

    wcscpy_s(m_nid.szTip, text);

    // uFlags'i KALICI olarak degistirmiyoruz. Eskiden `m_nid.uFlags = NIF_TIP;`
    // yaziyordu — ilk tooltip guncellemesinden sonra NIF_ICON ve NIF_MESSAGE
    // sonsuza dek kayboluyordu, sonraki her NIM_MODIFY eksik bayrakla gidiyordu.
    NOTIFYICONDATAW update = m_nid;
    update.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &update);
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
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, kMenuExit, L"Exit");

    // Menunun dogru pozisyonda acilmasi icin
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);  // Menu focus icin gerekli (Win32 quirk)

    UINT cmd = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        pt.x, pt.y, 0, m_hwnd, nullptr);

    // Belgelenmis Win32 tuhafligi (MS KB135788): SetForegroundWindow ile
    // menuyu one aldiktan sonra pencereye bos bir mesaj postalanmazsa,
    // kullanici menunun disina tikladiginda menu ekranda asili kaliyor.
    PostMessageW(m_hwnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);

    switch (cmd)
    {
    case kMenuToggle:
        if (m_onToggle) m_onToggle();
        break;
    case kMenuExit:
        if (m_onExit) m_onExit();
        break;
    }
}

} // namespace BetterMagnifier
