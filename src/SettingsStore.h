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
    Mouse,           // the view is centred on the pointer (legacy behaviour)
    MouseAndFocus,   // also follows EVENT_OBJECT_FOCUS
    EdgePush,        // the view holds still until the pointer reaches a band
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

    // EdgePush by default. Mouse was the old default for one reason — it kept
    // the view anchored to the cursor, which was the only way clicks landed
    // where they appeared. Our own cursor sprite removed that constraint, so
    // the view can hold still without costing alignment, and a view that stops
    // sliding on every twitch is far less tiring to read.
    //
    // MouseAndFocus is currently inert: focalPoint no longer feeds the source
    // rect. See App::OnFocusChanged.
    FollowMode followMode        = FollowMode::EdgePush;

    bool       startWithWindows  = false;
    bool       rememberZoomLevel = true;

    // ── Edge-push panning ──
    // Band width as a fraction of the axis; ViewportController clamps the
    // result to [80, 300] screen pixels at use time.
    float edgeBandFraction = 0.12f;

    // ── Pointer ──
    // Scale mouse motion so the magnified pointer is usable. Off falls back to
    // native behaviour: the pointer flies at zoom times speed and no sprite is
    // drawn.
    bool  pointerScaling = true;

    // scale = pointerSpeed / pow(zoom, pointerCompensation)
    //
    // 1.0 compensation maps hand movement 1:1 onto the magnified screen, which
    // is right in theory and measured as far too slow: content is zoom times
    // further apart than it looks. 0.2 tested right; the theory oversold how
    // much correction the pointer wants.
    float pointerSpeed       = 1.0f;
    float pointerCompensation = 0.2f;

    // Sprite size relative to zoom. Above 1 draws a pointer larger than the
    // content scale, which low-vision users generally want.
    float cursorScale = 1.0f;

    // Keep the pointer on the monitor it is magnifying. On by default, and not
    // an accident: with a zoomed edge the pointer used to slip onto the next
    // display exactly when the user was reaching for the edge of the magnified
    // content. Edge-push already gets you to that edge.
    bool  lockPointerToMonitor = true;
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
