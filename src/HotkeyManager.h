#pragma once

// =============================================================================
// HotkeyManager.h — Global Hotkeys (RegisterHotKey)
// =============================================================================
// Sadece RegisterHotKey ile calisir. Low-level fare/klavye hook'lari
// InputThread'e tasindi — onlar render thread'de olamaz (bkz. InputThread.h).
//
// RegisterHotKey PENCEREYE bagli: WM_HOTKEY mesaji hangi pencereye
// kaydettiysen ona gider. Bu yuzden bu sinif render thread'de kaliyor,
// mesaj penceresinin sahibi orada.
// =============================================================================

#ifndef BETTER_MAGNIFIER_HOTKEY_MANAGER_H
#define BETTER_MAGNIFIER_HOTKEY_MANAGER_H

#include "SettingsStore.h"

#include <windows.h>
#include <functional>

namespace BetterMagnifier {

// Hotkey action callback tipi
using HotkeyCallback = std::function<void()>;

class HotkeyManager
{
public:
    HotkeyManager() = default;
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // ── Initialization ──
    // hwnd: Hotkey mesajlarinin gonderilecegi pencere
    // settings: hangi tus kombinasyonlari kaydedilecek
    bool Initialize(HWND hwnd, const GeneralSettings& settings);
    void Shutdown();

    // Ayarlar degisince cagir — eskileri kaldirip yenilerini kaydeder.
    // Basarisiz kayitlari raporlar: bit 0 = toggle basarisiz, bit 1 = freeze basarisiz.
    UINT Reregister(const GeneralSettings& settings);

    // Son Reregister'in sonucu — GUI kirmizi uyari satirini bununla cizecek.
    UINT LastFailedMask() const { return m_lastFailedMask; }

    // ── Callback Registration ──
    void SetToggleZoomCallback(HotkeyCallback cb)  { m_onToggleZoom = std::move(cb); }
    void SetFreezeCallback(HotkeyCallback cb)      { m_onFreeze = std::move(cb); }

    // ── WM_HOTKEY isleyicisi (App tarafindan cagirilir) ──
    void HandleHotkey(int hotkeyId);

    // Hotkey ID'leri
    // NOT: Varsayilanda Win+<harf> kullanmiyoruz — Windows 11 cogunu rezerve
    // etmis (Win+Z = Snap Layouts). Ctrl+Alt+<harf> guvenli alan.
    static constexpr int kHotkeyToggleZoom = 1;   // varsayilan: Ctrl+Alt+Z
    static constexpr int kHotkeyFreeze     = 2;   // varsayilan: Ctrl+Alt+X

    // Reregister maskesi
    static constexpr UINT kFailedToggle = 0b01;
    static constexpr UINT kFailedFreeze = 0b10;

private:
    HWND    m_hwnd           = nullptr;
    bool    m_initialized    = false;
    UINT    m_lastFailedMask = 0;

    // Callbacks
    HotkeyCallback  m_onToggleZoom;
    HotkeyCallback  m_onFreeze;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_HOTKEY_MANAGER_H
