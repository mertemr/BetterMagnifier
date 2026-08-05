#pragma once

// INI-backed settings at %APPDATA%\BetterMagnifier\settings.ini
//
// INI rather than JSON because Win32 ships the reader and writer
// (Get/WritePrivateProfileStringW): no parser to own, no dependency to add.
// The settings are flat, and per-monitor blocks map naturally onto sections
// keyed by device name.
//
// Thread ownership: written by whoever changes settings, read by the render
// thread when it gets WM_APP_SETTINGS_CHANGED. Safe without a lock only
// because the writer finishes writing before posting that message.

#ifndef BETTER_MAGNIFIER_SETTINGS_STORE_H
#define BETTER_MAGNIFIER_SETTINGS_STORE_H

#include <windows.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <filesystem>

namespace BetterMagnifier {

enum class FollowMode
{
    Mouse,
    MouseAndFocus,   // also follows EVENT_OBJECT_FOCUS
};

struct GeneralSettings
{
    // Stored as MOD_* flags plus a virtual key, which is exactly the pair
    // RegisterHotKey wants.
    UINT       toggleModifiers   = MOD_CONTROL | MOD_ALT;
    UINT       toggleVk          = 'Z';
    UINT       freezeModifiers   = MOD_CONTROL | MOD_ALT;
    UINT       freezeVk          = 'X';

    // Take over the Windows Magnifier shortcuts through the low-level hooks:
    // Win+Plus / Win+Minus step zoom, Ctrl+Alt+wheel steps zoom and is
    // swallowed so the page does not also scroll, Win+middle-click freezes.
    //
    // On by default. Turning it off leaves only the RegisterHotKey bindings.
    bool       hijackMagnifierKeys = true;

    // Mouse by default. MouseAndFocus moves the zoom region to the keyboard
    // focus, but that detaches the anchor from the cursor, and then clicks no
    // longer land where they appear (see App::OnFocusChanged). Both cannot
    // hold at once, so working clicks win and focus following is opt-in.
    FollowMode followMode        = FollowMode::Mouse;

    bool       startWithWindows  = false;
    bool       rememberZoomLevel = true;
};

// Keyed by device name, e.g. "\\\\.\\DISPLAY1"
struct MonitorSettings
{
    float minZoom  = 1.0f;
    float maxZoom  = 10.0f;
    float zoomStep = 0.25f;
    float lastZoom = 2.0f;   // used when rememberZoomLevel is on
};

// "Ctrl+Alt+Z" <-> (MOD_CONTROL|MOD_ALT, 'Z')
// Modifiers: Ctrl, Alt, Shift, Win (case-insensitive). Keys: A-Z, 0-9, F1-F24.
//
// On failure returns false and leaves modifiers/vk UNTOUCHED, so a caller can
// keep its current binding when the text is invalid.
bool ParseHotkey(std::wstring_view text, UINT& modifiers, UINT& vk);

// Modifier order is always Ctrl, Alt, Shift, Win so that a format/parse round
// trip is stable.
std::wstring FormatHotkey(UINT modifiers, UINT vk);

class SettingsStore
{
public:
    SettingsStore() = default;

    // A missing file is not an error: returns true with defaults. Unparseable
    // or nonsensical values fall back to defaults individually.
    bool Load();

    // Creates the directory if needed.
    bool Save() const;

    const GeneralSettings& General() const { return m_general; }
    GeneralSettings&       MutableGeneral() { return m_general; }

    // Unknown monitor yields defaults.
    MonitorSettings Monitor(const std::wstring& deviceName) const;
    void SetMonitor(const std::wstring& deviceName, const MonitorSettings& s);

    static std::filesystem::path FilePath();

private:
    GeneralSettings m_general;
    std::unordered_map<std::wstring, MonitorSettings> m_monitors;
};

#ifdef _DEBUG
// Assert-based self-check, run from main on Debug startup. There is no test
// framework here, and this is the only component that is pure logic.
void SettingsStoreSelfCheck();
#endif

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_SETTINGS_STORE_H
