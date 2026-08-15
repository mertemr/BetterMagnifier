// =============================================================================
// HotkeyManager.cpp — Global Hotkeys Implementation
// =============================================================================

#include "pch.h"
#include "HotkeyManager.h"
#include "Logger.h"

namespace BetterMagnifier {

HotkeyManager::~HotkeyManager()
{
    Shutdown();
}

// =============================================================================
// Initialize
// =============================================================================
bool HotkeyManager::Initialize(HWND hwnd, const GeneralSettings& settings)
{
    if (m_initialized)
        return true;

    m_hwnd = hwnd;
    LOG_INFO("HotkeyManager baslatiliyor...");

    const UINT failed = Reregister(settings);
    m_initialized = true;

    if (failed != 0)
        LOG_WARN("Bazi hotkey'ler kaydedilemedi (bayrak: 0b{:02b})", failed);

    LOG_INFO("HotkeyManager initialised");
    return true;
}

// =============================================================================
// Reregister — re-claim the hotkeys after a settings change
// =============================================================================
//
// Why the defaults are not Win+something: Windows 11 reserves Win+Z for Snap
// Layouts and most other Win+<letter> combinations for itself, and
// RegisterHotKey simply fails on a reserved one — a process cannot override a
// system shortcut. Ctrl+Alt+<letter> is unclaimed territory.
//
// The panel still lets a Win+ combination be chosen; it just shows the failure.
// Genuinely taking Win+Z needs the keyboard hook in InputThread, which is
// opt-in because it also kills Snap Layouts.
//
// MOD_NOREPEAT because these are toggles: auto-repeat while held would flip
// zoom on and off dozens of times a second.
// =============================================================================
UINT HotkeyManager::Reregister(const GeneralSettings& settings)
{
    if (!m_hwnd)
    {
        m_lastFailedMask = 0b11;
        return m_lastFailedMask;
    }

    // Unconditional: UnregisterHotKey on an unregistered id fails quietly, and
    // checking first would only add a branch that can go stale.
    UnregisterHotKey(m_hwnd, kHotkeyToggleZoom);
    UnregisterHotKey(m_hwnd, kHotkeyFreeze);

    UINT failed = 0;

    if (!RegisterHotKey(m_hwnd, kHotkeyToggleZoom,
                        settings.toggleModifiers | MOD_NOREPEAT, settings.toggleVk))
    {
        LOG_ERROR("Toggle hotkey kaydedilemedi ({}): {}",
            GetLastError(),
            ToUtf8(FormatHotkey(settings.toggleModifiers, settings.toggleVk)));
        failed |= 0b01;
    }
    else
    {
        LOG_INFO("  Hotkey: {} = Toggle Zoom",
            ToUtf8(FormatHotkey(settings.toggleModifiers, settings.toggleVk)));
    }

    if (!RegisterHotKey(m_hwnd, kHotkeyFreeze,
                        settings.freezeModifiers | MOD_NOREPEAT, settings.freezeVk))
    {
        LOG_ERROR("Freeze hotkey kaydedilemedi ({}): {}",
            GetLastError(),
            ToUtf8(FormatHotkey(settings.freezeModifiers, settings.freezeVk)));
        failed |= 0b10;
    }
    else
    {
        LOG_INFO("  Hotkey: {} = Freeze/Pin",
            ToUtf8(FormatHotkey(settings.freezeModifiers, settings.freezeVk)));
    }

    m_lastFailedMask = failed;
    return failed;
}

// =============================================================================
// Shutdown
// =============================================================================
void HotkeyManager::Shutdown()
{
    if (!m_initialized)
        return;

    if (m_hwnd)
    {
        UnregisterHotKey(m_hwnd, kHotkeyToggleZoom);
        UnregisterHotKey(m_hwnd, kHotkeyFreeze);
    }

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
        LOG_INFO("Hotkey: Toggle Zoom");
        if (m_onToggleZoom) m_onToggleZoom();
        break;

    case kHotkeyFreeze:
        LOG_INFO("Hotkey: Freeze");
        if (m_onFreeze) m_onFreeze();
        break;

    default:
        LOG_WARN("Bilinmeyen hotkey ID: {}", hotkeyId);
        break;
    }
}

} // namespace BetterMagnifier
