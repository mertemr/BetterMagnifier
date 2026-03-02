#pragma once

// =============================================================================
// HotkeyManager.h — Global Hotkeys + Mouse Scroll Hook
// =============================================================================

#ifndef BETTER_MAGNIFIER_HOTKEY_MANAGER_H
#define BETTER_MAGNIFIER_HOTKEY_MANAGER_H

#include <windows.h>
#include <functional>
#include <atomic>
#include <thread>

namespace BetterMagnifier {

// Hotkey action callback tipleri
using HotkeyCallback  = std::function<void()>;
using ScrollCallback  = std::function<void(int delta, POINT mousePos)>;

class HotkeyManager
{
public:
    HotkeyManager() = default;
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // ── Initialization ──
    // hwnd: Hotkey mesajlarinin gonderilecegi pencere
    bool Initialize(HWND hwnd);
    void Shutdown();

    // ── Callback Registration ──
    void SetToggleZoomCallback(HotkeyCallback cb)  { m_onToggleZoom = std::move(cb); }
    void SetFreezeCallback(HotkeyCallback cb)      { m_onFreeze = std::move(cb); }
    void SetScrollCallback(ScrollCallback cb)      { m_onScroll = std::move(cb); }

    // ── WM_HOTKEY isleyicisi (App tarafindan cagirilir) ──
    void HandleHotkey(int hotkeyId);

    // Hotkey ID'leri
    static constexpr int kHotkeyToggleZoom = 1;   // Win+Z
    static constexpr int kHotkeyFreeze     = 2;   // Win+Shift+Z

private:
    // ── Mouse Hook ──
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void StartMouseHook();
    void StopMouseHook();

    HWND    m_hwnd      = nullptr;
    HHOOK   m_mouseHook = nullptr;
    bool    m_initialized = false;

    // Callbacks
    HotkeyCallback  m_onToggleZoom;
    HotkeyCallback  m_onFreeze;
    ScrollCallback  m_onScroll;

    // Hook icin static instance (Win32 callback'ler static olmak zorunda)
    static HotkeyManager* s_instance;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_HOTKEY_MANAGER_H
