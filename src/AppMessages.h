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

// wParam sentinel: not a specific monitor, whichever one holds the cursor.
inline constexpr WPARAM kFocusedMonitor = static_cast<WPARAM>(-1);

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_APP_MESSAGES_H
