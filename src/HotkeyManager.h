#pragma once

// =============================================================================
// HotkeyManager.h — Global Hotkeys (RegisterHotKey)
// =============================================================================
// Sadece RegisterHotKey ile calisir. Low-level fare/klavye hook'lari
// InputThread'e tasindi — onlar render thread'de OLAMAZ (bkz. InputThread.h).
//
// RegisterHotKey PENCEREYE bagli: WM_HOTKEY mesaji hangi pencereye
// kaydettiysen ona gider. Bu yuzden bu sinif render thread'de kaliyor,
// mesaj penceresinin sahibi orada.
//
// Python analojisi: keyboard.add_hotkey('ctrl+alt+z', cb) — ama Win32'de
// kayit isletim sistemine yapiliyor ve baska uygulama ayni kombinasyonu
// almissa BASARISIZ olur. Bu yuzden hata raporlamasi onemli.
// =============================================================================

#ifndef BETTER_MAGNIFIER_HOTKEY_MANAGER_H
#define BETTER_MAGNIFIER_HOTKEY_MANAGER_H

#include "SettingsStore.h"

#include <windows.h>
#include <functional>

namespace BetterMagnifier {

using HotkeyCallback = std::function<void()>;

class HotkeyManager
{
public:
    HotkeyManager() = default;
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // ── Initialization ──
    // hwnd     : hotkey mesajlarinin gonderilecegi pencere
    // settings : hangi tus kombinasyonlari kaydedilecek
    bool Initialize(HWND hwnd, const GeneralSettings& settings);
    void Shutdown();

    // Ayarlar degisince cagir — eskileri kaldirip yenilerini kaydeder.
    // Doner: basarisiz kayit bayraklari. bit 0 = toggle, bit 1 = freeze.
    UINT Reregister(const GeneralSettings& settings);

    // Son Reregister sonucundaki hata bayraklari. GUI kirmizi uyari
    // satirinda bunu gosteriyor — sadece log'a yazmak yetmiyor, kullanici
    // tusa basip hicbir sey olmadigini gorur ve nedenini bilmez.
    UINT LastFailedMask() const { return m_lastFailedMask; }

    // ── Callback Registration ──
    void SetToggleZoomCallback(HotkeyCallback cb) { m_onToggleZoom = std::move(cb); }
    void SetFreezeCallback(HotkeyCallback cb)     { m_onFreeze = std::move(cb); }

    // ── WM_HOTKEY isleyicisi (App tarafindan cagirilir) ──
    void HandleHotkey(int hotkeyId);

    // Hotkey ID'leri
    static constexpr int kHotkeyToggleZoom = 1;
    static constexpr int kHotkeyFreeze     = 2;

private:
    HWND m_hwnd           = nullptr;
    bool m_initialized    = false;
    UINT m_lastFailedMask = 0;

    HotkeyCallback m_onToggleZoom;
    HotkeyCallback m_onFreeze;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_HOTKEY_MANAGER_H
