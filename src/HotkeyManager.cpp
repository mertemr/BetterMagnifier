// =============================================================================
// HotkeyManager.cpp — Global Hotkeys + Mouse Hook Implementation
// =============================================================================

#include "pch.h"
#include "HotkeyManager.h"
#include "Logger.h"

namespace BetterMagnifier {

HotkeyManager* HotkeyManager::s_instance = nullptr;

// =============================================================================
// Destructor
// =============================================================================
HotkeyManager::~HotkeyManager()
{
    Shutdown();
}

// =============================================================================
// Initialize
// =============================================================================
bool HotkeyManager::Initialize(HWND hwnd)
{
    if (m_initialized)
        return true;

    m_hwnd = hwnd;
    s_instance = this;

    LOG_INFO("HotkeyManager baslatiliyor...");

    // ── Global Hotkey: Ctrl+Alt+Z = Toggle Zoom ──
    // RegisterHotKey: Hangi pencereye tiklanirsa tiklansin, bu tuş kombinasyonu
    // bizim mesaj loop'umuza WM_HOTKEY mesaji gonderir.
    // Python analojisi: keyboard.add_hotkey('ctrl+alt+z', callback)
    //
    // MOD_NOREPEAT = basili tutunca tekrarlama (toggle icin sart)
    //
    // NEDEN MOD_WIN DEGIL:
    //   Win+Z Windows 11'de Snap Layouts'a rezerve — RegisterHotKey basarisiz
    //   doner (sistem kisayollarini override edemeyiz). Win+<harf>
    //   kombinasyonlarinin cogu Windows tarafindan alinmis durumda.
    //   Ctrl+Alt+<harf> ise sistem tarafindan rezerve edilmiyor, guvenli alan.
    if (!RegisterHotKey(hwnd, kHotkeyToggleZoom, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Z'))
    {
        LOG_ERROR("Ctrl+Alt+Z hotkey kaydedilemedi ({}) — baska uygulama kullaniyor olabilir",
            GetLastError());
        // Kritik degil — tray menusunden toggle yapilabilir
    }
    else
    {
        LOG_INFO("  Hotkey: Ctrl+Alt+Z = Toggle Zoom");
    }

    // ── Global Hotkey: Ctrl+Alt+X = Freeze/Pin ──
    if (!RegisterHotKey(hwnd, kHotkeyFreeze, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'X'))
    {
        LOG_ERROR("Ctrl+Alt+X hotkey kaydedilemedi ({})", GetLastError());
    }
    else
    {
        LOG_INFO("  Hotkey: Ctrl+Alt+X = Freeze/Pin");
    }

    // ── Low-Level Mouse Hook ──
    StartMouseHook();

    m_initialized = true;
    LOG_INFO("HotkeyManager basariyla baslatildi");
    return true;
}

// =============================================================================
// Shutdown
// =============================================================================
void HotkeyManager::Shutdown()
{
    if (!m_initialized)
        return;

    StopMouseHook();

    if (m_hwnd)
    {
        UnregisterHotKey(m_hwnd, kHotkeyToggleZoom);
        UnregisterHotKey(m_hwnd, kHotkeyFreeze);
    }

    s_instance = nullptr;
    m_initialized = false;

    LOG_INFO("HotkeyManager kapatildi");
}

// =============================================================================
// HandleHotkey — WM_HOTKEY mesaji geldiginde
// =============================================================================
void HotkeyManager::HandleHotkey(int hotkeyId)
{
    switch (hotkeyId)
    {
    case kHotkeyToggleZoom:
        LOG_INFO("Hotkey: Win+Z (Toggle Zoom)");
        if (m_onToggleZoom) m_onToggleZoom();
        break;

    case kHotkeyFreeze:
        LOG_INFO("Hotkey: Win+Shift+Z (Freeze)");
        if (m_onFreeze) m_onFreeze();
        break;

    default:
        LOG_WARN("Bilinmeyen hotkey ID: {}", hotkeyId);
        break;
    }
}

// =============================================================================
// Mouse Hook — Scroll wheel ile zoom
// =============================================================================
//
// Low-Level Mouse Hook (WH_MOUSE_LL):
//   Sistemdeki TUM mouse olaylarını yakalar — hangi pencereye giderse gitsin.
//   Biz sadece WM_MOUSEWHEEL'i ilgilendiriyoruz (scroll wheel).
//
// DIKKAT: Bu hook message pump'i bloklayabilir!
//   Hook callback'i cok hizli donmeli (Microsoft 300ms limit koyuyor).
//   Agir is yapmak yerine sadece callback'i cagir, gercek isi ana thread'e birak.
//
// Neden static fonksiyon?
//   Win32 hook callback'leri class member function olamaz (calling convention farki).
//   Bu yuzden static fonksiyon + global s_instance pointer kullaniyoruz.
//   Python'da bu sorun yok cunku Python fonksiyonlari first-class object.
//
// =============================================================================
void HotkeyManager::StartMouseHook()
{
    m_mouseHook = SetWindowsHookExW(
        WH_MOUSE_LL,
        LowLevelMouseProc,
        GetModuleHandleW(nullptr),
        0   // 0 = tum thread'ler (global hook)
    );

    if (m_mouseHook)
    {
        LOG_INFO("  Mouse hook aktif (scroll wheel zoom)");
    }
    else
    {
        LOG_WARN("Mouse hook kurulamadi: {}", GetLastError());
    }
}

void HotkeyManager::StopMouseHook()
{
    if (m_mouseHook)
    {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
        LOG_DEBUG("Mouse hook kaldirildi");
    }
}

LRESULT CALLBACK HotkeyManager::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && wParam == WM_MOUSEWHEEL)
    {
        auto* mouseData = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (mouseData)
        {
            // HIWORD(mouseData->mouseData) = scroll delta
            // Pozitif = yukari scroll (zoom in), negatif = asagi (zoom out)
            int delta = GET_WHEEL_DELTA_WPARAM(mouseData->mouseData);
            POINT pt  = mouseData->pt;

            if (s_instance->m_onScroll)
            {
                s_instance->m_onScroll(delta, pt);
            }
        }
    }

    // Onemli: CallNextHookEx ile chain'i devam ettir!
    // Bunu yapmazsan diger uygulamalar scroll olaylarini alamaz.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace BetterMagnifier
