#pragma once

// WinUI 3 control panel, living on its own STA thread.
//
// Why a separate thread: main.cpp puts the process' main thread in an MTA and
// the render loop blocks in Present. XAML wants an STA whose message loop is
// free to pump, so it cannot share either.
//
// Why XAML Islands and not a standalone WinUI 3 Window: Application::Start owns
// the message loop of the thread that calls it, and DesktopWindowXamlSource is
// the supported way to put XAML inside an existing Win32 app.
//
// Why the UI is built in code rather than markup: compiled .xaml needs the WinUI
// XAML compiler targets wired into a plain Win32 vcxproj. There are about
// twenty controls here, which is manageable by hand, and the Fluent look is not
// lost - the default theme dictionary is already loaded.
//
// Thread boundary:
//   GUI    -> engine : PostMessage(engineHwnd, WM_APP_*)
//   engine -> GUI    : StatusSnapshot atomics, polled at 10 Hz
// Nothing else is shared.

#ifndef BETTER_MAGNIFIER_CONTROL_PANEL_H
#define BETTER_MAGNIFIER_CONTROL_PANEL_H

#include <windows.h>
#include <thread>
#include <atomic>
#include <memory>
#include <future>

namespace BetterMagnifier {

class SettingsStore;
class StatusSnapshot;

class ControlPanel
{
public:
    ControlPanel();
    ~ControlPanel();

    ControlPanel(const ControlPanel&) = delete;
    ControlPanel& operator=(const ControlPanel&) = delete;

    // First call starts the GUI thread, later calls bring the window forward.
    //
    // engineHwnd : where the GUI posts WM_APP_* messages
    // settings   : the GUI writes it, the engine re-reads on WM_APP_SETTINGS_CHANGED
    // status     : the GUI polls it, the engine writes it every frame
    //
    // If the Windows App Runtime is missing, this logs, shows a message box once
    // and returns. Magnification, hotkeys and the tray menu keep working.
    void Show(HWND engineHwnd, SettingsStore* settings, StatusSnapshot* status);

    // Monitor list changed; rebuild the cards if the panel is up. Thread-safe.
    void NotifyDisplayChange();

    // A hotkey capture finished — bound, cancelled or refused. The panel shows
    // the bindings as static text and puts its buttons into a "press a key"
    // state, so it has to be told; nothing it polls would reveal either.
    // Thread-safe.
    void NotifyHotkeysChanged();

    // Shut the GUI thread down and join it. Idempotent.
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    void ThreadMain();

    // Everything below runs on the GUI thread only.
    void BuildUi();
    void RebuildMonitorCards();
    void BuildSettingsTab();
    void StartLiveTimer();
    void UpdateLiveValues();

    // Controls -> SettingsStore -> disk -> engine, in that order. Reversing it
    // would have the engine read the values we have not written yet.
    void PushSettings();

    // Re-read settings.ini from disk and push it to the engine, for when the
    // file was edited by hand.
    void ReloadFromDisk();

    // XAML types stay out of the header so that App.h, which includes this,
    // does not pull the whole WinUI projection into every translation unit.
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_stopping{false};
    std::atomic<DWORD> m_threadId{0};

    // Lets Stop() wait for the thread with a deadline. Joining unconditionally
    // would hang shutdown if the XAML loop ever refused to end.
    std::promise<void> m_exited;
    std::future<void>  m_exitedFuture;

    // Application::Start may only be called once per process, so a panel that
    // failed to come up, or was stopped, cannot be started again.
    std::atomic<bool>  m_startAttempted{false};

    HWND            m_engineHwnd = nullptr;
    SettingsStore*  m_settings   = nullptr;
    StatusSnapshot* m_status     = nullptr;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_CONTROL_PANEL_H
