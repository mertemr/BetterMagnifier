#pragma once

// Cross-thread message constants.
//
// The input thread talks to the render thread only through these messages.
// No shared mutable state, no locks: the Win32 message queue is the queue.
//
// PostMessage, never SendMessage. SendMessage waits for the target thread to
// process the message, and the render thread can be blocked in Present for a
// whole frame. In a low-level hook that stall hits LowLevelHooksTimeout and
// Windows silently uninstalls the hook.

#ifndef BETTER_MAGNIFIER_APP_MESSAGES_H
#define BETTER_MAGNIFIER_APP_MESSAGES_H

#include <windows.h>

namespace BetterMagnifier {

// Tray icon callback; TrayIcon::Create puts this in uCallbackMessage.
inline constexpr UINT WM_APP_TRAY             = WM_APP + 1;

// Settings changed, re-read SettingsStore.
inline constexpr UINT WM_APP_SETTINGS_CHANGED = WM_APP + 2;

// Set zoom level. wParam = monitor index, lParam = zoom * 1000 (2.50x -> 2500).
inline constexpr UINT WM_APP_SET_ZOOM         = WM_APP + 3;

// Toggle zoom / freeze. wParam = monitor index, or kFocusedMonitor.
inline constexpr UINT WM_APP_TOGGLE_ZOOM      = WM_APP + 4;
inline constexpr UINT WM_APP_TOGGLE_FREEZE    = WM_APP + 5;

// Step zoom by one increment. wParam = kZoomIn or kZoomOut.
// lParam unused: the render thread calls GetCursorPos itself, since the few
// milliseconds between event and handling keep the cursor on the same monitor.
//
// Differs from TOGGLE_ZOOM in that this can also turn zoom ON when it is off,
// matching Windows Magnifier's Win+Plus, and turns it off at minZoom.
inline constexpr UINT WM_APP_ZOOM_STEP        = WM_APP + 6;

inline constexpr WPARAM kZoomIn  = 1;
inline constexpr WPARAM kZoomOut = static_cast<WPARAM>(-1);

// Keyboard focus moved. lParam = focused HWND.
inline constexpr UINT WM_APP_FOCUS_CHANGED    = WM_APP + 7;

// A popup or menu just appeared, re-assert topmost immediately.
//
// Menus and dropdowns are created HWND_TOPMOST after us, so they land above
// the overlay. Polling is too slow for dropdowns: they open and get used in
// well under a poll interval, and the popup shows twice until the next tick.
inline constexpr UINT WM_APP_ASSERT_TOPMOST   = WM_APP + 8;

// Open the control panel. The tray menu calls App::OnShowPanel directly; this
// exists so the panel can also be opened from outside, which is the only way to
// reach it without clicking a tray icon.
inline constexpr UINT WM_APP_SHOW_PANEL       = WM_APP + 9;

// ── Hotkey capture ──
//
// The panel cannot ask the user to TYPE a hotkey: a XAML TextBox on the
// island's STA thread takes the whole process down with a stowed exception, and
// NumberBox goes the same way because it embeds one (docs/PANEL-BLANK.md). So
// the binding is captured from a real key press instead — through the
// WH_KEYBOARD_LL hook this application already installs for other reasons.
//
// Better than a text field anyway: nothing to mistype, no parser to disagree
// with, and left/right modifier and extended-key questions never come up
// because the answer arrives as the (modifiers, vk) pair RegisterHotKey wants.
//
// panel -> engine. wParam = kHotkeyToggle or kHotkeyFreeze.
inline constexpr UINT WM_APP_CAPTURE_HOTKEY   = WM_APP + 10;

// input thread -> engine.
//   wParam = MOD_* flags
//   lParam = (which << 16) | virtual key
//
// "which" travels back rather than being remembered on the engine side, so a
// capture that was cancelled and restarted for the other binding cannot deliver
// its answer to the first one.
//
// vk == 0 means cancelled — Escape, or the capture timing out — and the
// existing binding stands.
inline constexpr UINT WM_APP_HOTKEY_CAPTURED  = WM_APP + 11;

inline constexpr WPARAM kHotkeyToggle = 0;
inline constexpr WPARAM kHotkeyFreeze = 1;

// wParam sentinel: not a specific monitor, whichever one holds the cursor.
inline constexpr WPARAM kFocusedMonitor = static_cast<WPARAM>(-1);

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_APP_MESSAGES_H
