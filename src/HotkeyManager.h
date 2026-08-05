#pragma once

// Global hotkeys through RegisterHotKey.
//
// The low-level mouse and keyboard hooks live in InputThread; they cannot run
// here (see InputThread.h). RegisterHotKey is window-bound: WM_HOTKEY goes to
// whichever window registered it, so this stays on the render thread with the
// message window.
//
// Registration goes through the OS, so it FAILS when another process already
// owns the combination. Reporting that failure matters.

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

    bool Initialize(HWND hwnd, const GeneralSettings& settings);
    void Shutdown();

    // Unregisters the old bindings and registers the ones in settings.
    // Returns a failure mask: bit 0 = toggle, bit 1 = freeze.
    UINT Reregister(const GeneralSettings& settings);

    // Last Reregister result. Logging alone is not enough: the user presses a
    // key, nothing happens, and has no way to know why.
    UINT LastFailedMask() const { return m_lastFailedMask; }

    void SetToggleZoomCallback(HotkeyCallback cb) { m_onToggleZoom = std::move(cb); }
    void SetFreezeCallback(HotkeyCallback cb)     { m_onFreeze = std::move(cb); }

    void HandleHotkey(int hotkeyId);

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
