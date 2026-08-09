#pragma once

// Low-level input hooks, on their own thread.
//
// Why a separate thread, the whole reason this file exists: low-level hooks
// (WH_MOUSE_LL, WH_KEYBOARD_LL) are invoked on the message queue of the thread
// that INSTALLED them. The render thread blocks in Present for up to a frame,
// so a hook living there puts every mouse and key event in the system behind
// our frame. Exceed LowLevelHooksTimeout (300 ms by default) and Windows
// silently uninstalls the hook.
//
// This thread does no real work: callbacks PostMessage and return.
//
// A hook's thread MUST run a GetMessage loop or the callback is never invoked.

#ifndef BETTER_MAGNIFIER_INPUT_THREAD_H
#define BETTER_MAGNIFIER_INPUT_THREAD_H

#include "SettingsStore.h"   // FollowMode
#include "ViewportController.h"
#include "ViewportSnapshot.h"

#include <windows.h>
#include <thread>
#include <atomic>
#include <cstdint>

namespace BetterMagnifier {

class InputThread
{
public:
    InputThread() = default;
    ~InputThread();

    InputThread(const InputThread&) = delete;
    InputThread& operator=(const InputThread&) = delete;

    // targetHwnd receives the PostMessage traffic (the message window).
    //
    // Hooks are installed inside the thread; Start does not return until that
    // has either succeeded or failed (promise/future handoff).
    bool Start(HWND targetHwnd, FollowMode initialMode, bool hijackMagnifierKeys);

    // Post WM_QUIT to the thread, remove hooks, join. Idempotent.
    void Stop();

    // Live setting changes. Reading an atomic flag in the callback is cheaper
    // and race-free compared to installing and removing hooks.
    void SetFollowMode(FollowMode mode);
    void SetHijackMagnifierKeys(bool enable);

    // Both objects are owned by App and outlive this thread. Call before Start.
    //
    // The controller is mutated ONLY on this thread: edge-push has to advance
    // per mouse event to stay proportional to mouse motion rather than to frame
    // rate, and that is this thread's event stream.
    void Attach(ViewportController* controller, ViewportSnapshot* snapshot);

    void SetEdgePushConfig(const EdgePushConfig& cfg);

private:
    void ThreadMain();
    bool InstallHooks();
    void RemoveHooks();

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild,
                                      DWORD idEventThread, DWORD dwmsEventTime);

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    std::atomic<DWORD> m_threadId{0};   // Stop() posts here

    // Pull render-thread requests (zoom, monitor layout) into the controller.
    // Idempotent, and cheap when nothing changed: one compare per monitor.
    void SyncFromRequests();

    // Copy the controller's state out for the render thread.
    void PublishViewport();

    std::atomic<FollowMode> m_followMode{FollowMode::Mouse};
    std::atomic<bool>       m_hijackMagnifierKeys{true};

    ViewportController* m_viewport = nullptr;
    ViewportSnapshot*   m_snapshot = nullptr;

    // Split into scalars rather than std::atomic<EdgePushConfig>: the struct is
    // 16 bytes and an atomic that wide is not guaranteed lock-free, which is
    // not something to find out inside a low-level hook.
    std::atomic<bool>  m_cfgEnabled{true};
    std::atomic<float> m_cfgBandFraction{0.12f};

    // Input thread only; no synchronisation needed.
    std::uint64_t m_seenLayoutEpoch = 0;

    // Zoom can change with the mouse perfectly still (a hotkey, the panel), and
    // then nothing would pull the request through until the user moved. A timer
    // covers that; 16 ms keeps the lag under one frame.
    static constexpr UINT_PTR kSyncTimerId = 1;
    static constexpr UINT     kSyncTimerMs = 16;

    HWND          m_target       = nullptr;
    HHOOK         m_mouseHook    = nullptr;
    HHOOK         m_keyboardHook = nullptr;
    HWINEVENTHOOK m_focusHook    = nullptr;

    // Separate hook for EVENT_SYSTEM_MENUPOPUPSTART: that event is 0x0006
    // while the object events are 0x8002+, and one range spanning both would
    // drag in dozens of unrelated events.
    HWINEVENTHOOK m_popupHook    = nullptr;

    // Win32 hook callbacks must be static (calling convention), hence the
    // instance pointer. There is only ever one InputThread.
    static InputThread* s_instance;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_INPUT_THREAD_H
