// =============================================================================
// InputThread.cpp
// =============================================================================

#include "pch.h"
#include "InputThread.h"
#include "AppMessages.h"
#include "SystemCursor.h"
#include "Logger.h"

#include <future>

namespace BetterMagnifier {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Events produced by SendInput carry LLKHF_INJECTED / LLMHF_INJECTED. Ignoring
// them by default, for two reasons.
//
// First, automation must not be able to drive the application. If a script
// dies between pressing and releasing Win, Ctrl or Alt — or the release event
// is simply lost — that modifier stays logically down, and from then on every
// "+", "-" or wheel tick the user makes reads as a zoom command. The app
// appears to zoom on its own. This happened.
//
// Second, a magnifier should respond to the physical input in front of it.
// There is no reason for another process to be steering it remotely.
//
// BM_ALLOW_INJECTED=1 turns the filter off, for scripted verification only.
bool IgnoreInjectedInput()
{
    static const bool allow = []() {
        wchar_t buf[8]{};
        const DWORD n = GetEnvironmentVariableW(L"BM_ALLOW_INJECTED", buf, 8);
        return (n > 0 && n < 8 && buf[0] == L'1');
    }();
    return !allow;
}

} // anonymous namespace

InputThread* InputThread::s_instance = nullptr;

InputThread::~InputThread()
{
    Stop();
}

// =============================================================================
// Start — thread'i baslat, hook kurulumunu bekle
// =============================================================================
//
// Start blocks on a promise because the hooks must be installed from inside the
// thread — they attach to that thread's queue — yet the caller needs to know
// whether they came up. The thread writes the result, Start reads it.
// =============================================================================
bool InputThread::Start(HWND targetHwnd, FollowMode initialMode, bool hijackMagnifierKeys)
{
    if (m_running.load(std::memory_order_acquire))
        return true;

    if (!targetHwnd)
    {
        LOG_ERROR("InputThread::Start — targetHwnd null!");
        return false;
    }

    m_target = targetHwnd;
    m_followMode.store(initialMode, std::memory_order_relaxed);
    m_hijackMagnifierKeys.store(hijackMagnifierKeys, std::memory_order_relaxed);
    s_instance = this;

    std::promise<bool> ready;
    std::future<bool> readyFuture = ready.get_future();

    m_thread = std::thread([this, p = std::move(ready)]() mutable {
        m_threadId.store(GetCurrentThreadId(), std::memory_order_release);

        const bool ok = InstallHooks();
        p.set_value(ok);

        if (!ok)
            return;

        m_running.store(true, std::memory_order_release);
        ThreadMain();
        m_running.store(false, std::memory_order_release);

        RemoveHooks();
    });

    if (!readyFuture.get())
    {
        LOG_ERROR("InputThread hook kurulumu basarisiz");
        if (m_thread.joinable())
            m_thread.join();
        s_instance = nullptr;
        return false;
    }

    LOG_INFO("InputThread baslatildi (thread id: {})",
        m_threadId.load(std::memory_order_acquire));
    return true;
}

// =============================================================================
// InstallHooks — thread ICINDE cagrilir
// =============================================================================
bool InputThread::InstallHooks()
{
    // ── Fare hook'u (scroll wheel zoom) ──
    m_mouseHook = SetWindowsHookExW(
        WH_MOUSE_LL,
        LowLevelMouseProc,
        GetModuleHandleW(nullptr),
        0   // 0 = global (tum thread'ler)
    );

    if (!m_mouseHook)
    {
        LOG_ERROR("WH_MOUSE_LL kurulamadi: {}", GetLastError());
        return false;
    }
    LOG_INFO("  Mouse hook installed (on the input thread)");

    // Always installed; whether it swallows anything is an atomic flag read in
    // the callback. Cheaper than installing and removing the hook on every
    // settings change, and free of the races that would come with it.
    //
    // Failure is not fatal — it only means the Magnifier shortcuts are not
    // taken over.
    m_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        GetModuleHandleW(nullptr),
        0);

    if (!m_keyboardHook)
        LOG_WARN("WH_KEYBOARD_LL kurulamadi: {} — Win+arti/eksi devralma devre disi",
            GetLastError());
    else
        LOG_INFO("  Keyboard hook installed");

    // WINEVENT_OUTOFCONTEXT delivers the callback on this thread instead of
    // injecting a DLL, which is why this thread must pump messages.
    // SKIPOWNPROCESS keeps our own overlay and panel from triggering it.
    //
    // The SHOW..FOCUS range covers both focus changes and newly shown windows
    // such as dropdowns and flyouts; the handler sorts out which is which.
    m_focusHook = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_FOCUS,
        nullptr,
        WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!m_focusHook)
        LOG_WARN("EVENT_OBJECT_* hook failed: {} — focus tracking and popup "
                 "detection are off", GetLastError());
    else
        LOG_INFO("  Focus and popup hook installed");

    // Classic menus, such as the tray's context menu, raise
    // EVENT_SYSTEM_MENUPOPUPSTART instead. It needs its own hook because that
    // event is 0x0006 while the range above starts at 0x8002.
    m_popupHook = SetWinEventHook(
        EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPSTART,
        nullptr,
        WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!m_popupHook)
        LOG_WARN("EVENT_SYSTEM_MENUPOPUPSTART hook kurulamadi: {}", GetLastError());
    else
        LOG_INFO("  Menu popup hook'u aktif");

    return true;
}

void InputThread::RemoveHooks()
{
    if (m_popupHook)
    {
        UnhookWinEvent(m_popupHook);
        m_popupHook = nullptr;
        LOG_DEBUG("Menu popup hook'u kaldirildi");
    }

    if (m_focusHook)
    {
        UnhookWinEvent(m_focusHook);
        m_focusHook = nullptr;
        LOG_DEBUG("Odak + popup hook'u kaldirildi");
    }

    if (m_keyboardHook)
    {
        UnhookWindowsHookEx(m_keyboardHook);
        m_keyboardHook = nullptr;
        LOG_DEBUG("Klavye hook'u kaldirildi");
    }

    if (m_mouseHook)
    {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
        LOG_DEBUG("Mouse hook kaldirildi");
    }
}

// =============================================================================
// ThreadMain — the message loop the hooks need in order to be called
// =============================================================================
// No window is involved; these are thread-only messages. The loop exists purely
// to keep this thread pumping so the hook callbacks can run.
//
// GetMessage rather than PeekMessage: nothing is rendered here, so blocking is
// the correct behaviour and costs no CPU while idle.
// =============================================================================
void InputThread::ThreadMain()
{
    // A thread timer (hwnd == nullptr) delivers WM_TIMER straight to this
    // loop. It exists so a zoom change with the mouse perfectly still still
    // reaches the controller: nothing else would pull the request through
    // until the user moved the mouse.
    SetTimer(nullptr, kSyncTimerId, kSyncTimerMs, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message == WM_TIMER && msg.hwnd == nullptr)
        {
            SyncFromRequests();
            PublishViewport();

            if (++m_livenessTicks >= kLivenessEveryTicks)
            {
                m_livenessTicks = 0;
                CheckHookLiveness();
            }
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(nullptr, kSyncTimerId);
}

void InputThread::Attach(ViewportController* controller, ViewportSnapshot* snapshot)
{
    m_viewport = controller;
    m_snapshot = snapshot;
    m_pointer.Attach(controller, snapshot);
}

void InputThread::SetViewportConfig(const ViewportConfig& cfg)
{
    m_cfgMode.store(cfg.mode, std::memory_order_relaxed);
    m_cfgBandFraction.store(cfg.bandFraction, std::memory_order_relaxed);
}

// =============================================================================
// SyncFromRequests — apply the render thread's requests to the controller
// =============================================================================
//
// Called from the mouse hook and from the sync timer. Idempotent, and cheap
// when nothing changed: one double compare per monitor.
// =============================================================================
void InputThread::SyncFromRequests()
{
    if (!m_viewport || !m_snapshot)
        return;

    ViewportConfig cfg;
    cfg.mode         = m_cfgMode.load(std::memory_order_relaxed);
    cfg.bandFraction = m_cfgBandFraction.load(std::memory_order_relaxed);
    m_viewport->SetConfig(cfg);

    const double px = m_snapshot->pointerX.load(std::memory_order_relaxed);
    const double py = m_snapshot->pointerY.load(std::memory_order_relaxed);

    // Layout first: a zoom applied against a stale monitor rect would clamp
    // srcOrigin to the wrong bound.
    const std::uint64_t epoch = m_snapshot->layoutEpoch.load(std::memory_order_acquire);
    if (epoch != m_seenLayoutEpoch)
    {
        m_seenLayoutEpoch = epoch;

        const std::size_t count = m_snapshot->monitorCount.load(std::memory_order_relaxed);
        m_viewport->SetMonitorCount(count);

        for (std::size_t i = 0; i < count; ++i)
        {
            const MonitorViewportAtomic& a = m_snapshot->Monitor(i);
            m_viewport->SetMonitorRect(i,
                a.originX.load(std::memory_order_relaxed),
                a.originY.load(std::memory_order_relaxed),
                a.width.load(std::memory_order_relaxed),
                a.height.load(std::memory_order_relaxed));
        }
        m_viewport->ReclampAll();
    }

    for (std::size_t i = 0; i < m_viewport->MonitorCount(); ++i)
    {
        const double want = m_snapshot->Monitor(i).requestedZoom.load(std::memory_order_relaxed);
        if (want != m_viewport->Zoom(i))
            m_viewport->SetZoom(i, want, px, py);
    }

    // Keyboard focus, applied last so it wins over a zoom that was requested in
    // the same tick — the user just moved focus, and that is the more recent
    // statement of where they are looking.
    //
    // Acquire against App's release: seeing the new epoch guarantees seeing the
    // coordinates written before it.
    const std::uint64_t focus = m_snapshot->focusEpoch.load(std::memory_order_acquire);
    if (focus != m_seenFocusEpoch)
    {
        m_seenFocusEpoch = focus;

        const std::size_t mi = m_snapshot->focusMonitor.load(std::memory_order_relaxed);
        if (mi < m_viewport->MonitorCount()
            && !m_snapshot->Monitor(mi).frozen.load(std::memory_order_relaxed))
        {
            m_viewport->CenterOn(mi,
                m_snapshot->focusX.load(std::memory_order_relaxed),
                m_snapshot->focusY.load(std::memory_order_relaxed));
        }
    }
}

// =============================================================================
// CheckHookLiveness — catch a hook that was silently uninstalled
// =============================================================================
//
// Windows removes a low-level hook without notice when the callback overruns
// LowLevelHooksTimeout, and offers no way to ask whether ours survived. So it
// is inferred: if the OS cursor has moved but no mouse event reached us since
// the last check, nothing is delivering events and the hook is gone.
//
// Reinstalling is the good outcome. The bad one has to be handled too: if the
// hook cannot be brought back while the pointer is hidden, the user has no
// pointer and no way to click anything to fix it. Then the only correct move is
// to give the real pointer back and stop pretending.
// =============================================================================
void InputThread::CheckHookLiveness()
{
    POINT now{};
    if (!GetCursorPos(&now))
        return;

    const std::uint64_t events = m_mouseEvents.load(std::memory_order_relaxed);

    const bool cursorMoved = (m_lastSeenCursor.x != -1 || m_lastSeenCursor.y != -1) &&
                             (now.x != m_lastSeenCursor.x || now.y != m_lastSeenCursor.y);
    const bool sawEvents   = (events != m_lastSeenEvents);

    m_lastSeenCursor = now;
    m_lastSeenEvents = events;

    if (!cursorMoved || sawEvents)
        return;

    LOG_WARN("Mouse hook appears dead (cursor moved, no events) — reinstalling");

    RemoveHooks();
    if (InstallHooks())
    {
        m_pointer.Resync();
        LOG_INFO("Hooks reinstalled");
        return;
    }

    LOG_ERROR("Hook reinstall failed — restoring the system pointer and "
              "disabling pointer scaling");
    SystemCursor::Restore();
    m_pointer.SetEnabled(false);
}

void InputThread::PublishViewport()
{
    if (!m_viewport || !m_snapshot)
        return;

    for (std::size_t i = 0; i < m_viewport->MonitorCount(); ++i)
    {
        const MonitorViewport& v = m_viewport->Viewport(i);
        MonitorViewportAtomic& a = m_snapshot->Monitor(i);
        a.srcOriginX.store(v.srcOriginX, std::memory_order_relaxed);
        a.srcOriginY.store(v.srcOriginY, std::memory_order_relaxed);
        a.zoom.store(v.zoom, std::memory_order_relaxed);
    }
}

// =============================================================================
// Stop
// =============================================================================
void InputThread::Stop()
{
    const DWORD tid = m_threadId.load(std::memory_order_acquire);

    if (tid != 0)
    {
        // Thread-only WM_QUIT — GetMessage 0 dondurur, loop cikar.
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }

    if (m_thread.joinable())
        m_thread.join();

    m_threadId.store(0, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    s_instance = nullptr;
}

void InputThread::SetFollowMode(FollowMode mode)
{
    m_followMode.store(mode, std::memory_order_relaxed);
}

void InputThread::SetHijackMagnifierKeys(bool enable)
{
    const bool prev = m_hijackMagnifierKeys.exchange(enable, std::memory_order_relaxed);

    if (prev != enable)
    {
        LOG_INFO("Magnifier shortcut takeover {}{}",
            enable ? "ON" : "OFF",
            enable ? " — Win+Plus/Minus, Ctrl+Alt+wheel and Win+middle-click come to us"
                   : "");
    }
}

// =============================================================================
// LowLevelMouseProc — MUST RETURN FAST
// =============================================================================
//
// Called for every mouse event in the system. Doing work in here is not an
// option: the interesting events are handed to the render thread by
// PostMessage and the callback returns.
//
// PostMessage, never SendMessage. SendMessage waits for the target thread to
// process the message, and the render thread can be blocked in Present for a
// whole frame — which would stall this callback into LowLevelHooksTimeout and
// get the hook uninstalled.
// =============================================================================
LRESULT CALLBACK InputThread::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    // ── Pointer tracking and edge-push ──
    //
    // Outside the m_hijackMagnifierKeys check below on purpose: that flag
    // governs whether we take over Magnifier's SHORTCUTS. Panning the view is
    // not a shortcut, and turning shortcut takeover off must not freeze the
    // magnified view in place.
    //
    // The event is not swallowed. Input scaling arrives in a later task; for
    // now the OS keeps moving the cursor exactly as it always did, and this
    // only advances srcOrigin.
    if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE && lParam &&
        s_instance && s_instance->m_viewport && s_instance->m_snapshot)
    {
        InputThread* self = s_instance;
        auto* mm = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // Counted before any filtering: the liveness check asks "is anything
        // reaching us at all", and a filtered event still proves the hook is
        // installed.
        self->m_mouseEvents.fetch_add(1, std::memory_order_relaxed);

        // Injected filtering lives in PointerInput now, and it has to: our own
        // SetCursorPos comes back marked injected, so "ignore all injected" is
        // no longer a rule this layer can apply. Only the blanket automation
        // guard stays here, and only when it is not our own echo.
        const bool foreignInjected =
            (mm->flags & LLMHF_INJECTED) && IgnoreInjectedInput() &&
            !self->m_pointer.Enabled();

        if (!foreignInjected)
        {
            self->SyncFromRequests();
            const bool consumed = self->m_pointer.OnMouseMove(*mm);
            self->PublishViewport();

            if (consumed)
                return 1;
        }
    }

    if (nCode == HC_ACTION && s_instance && s_instance->m_target &&
        s_instance->m_hijackMagnifierKeys.load(std::memory_order_relaxed))
    {
        auto* data = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // Synthetic mouse events are ignored; see IgnoreInjectedInput.
        if (data && (data->flags & LLMHF_INJECTED) && IgnoreInjectedInput())
            return CallNextHookEx(nullptr, nCode, wParam, lParam);

        // Ctrl+Alt+wheel steps zoom. Windows Magnifier's own combination,
        // taken over here.
        //
        // Not the bare wheel: unless the hook swallows an event it also reaches
        // the application underneath, so plain-wheel zoom scrolled the page at
        // the same time. Swallowing Ctrl+Alt+wheel ends the double effect and
        // leaves the bare wheel scrolling normally.
        if (wParam == WM_MOUSEWHEEL && data)
        {
            const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool altDown  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;

            if (ctrlDown && altDown)
            {
                const int delta = GET_WHEEL_DELTA_WPARAM(data->mouseData);
                PostMessageW(s_instance->m_target, WM_APP_ZOOM_STEP,
                             (delta > 0) ? kZoomIn : kZoomOut, 0);

                // YUT — alttaki uygulama ne zoom ne scroll gormesin.
                return 1;
            }
        }

        // ── Win + orta tik = zoom bolgesini sabitle/coz ──
        //
        // Freeze from the mouse, since the hand is already there. MBUTTONUP is
        // swallowed too: leaving it through hands the application below half a
        // middle click, which in a browser opens a tab.
        if (wParam == WM_MBUTTONDOWN || wParam == WM_MBUTTONUP)
        {
            const bool winDown =
                (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

            if (winDown)
            {
                if (wParam == WM_MBUTTONDOWN)
                {
                    PostMessageW(s_instance->m_target, WM_APP_TOGGLE_FREEZE,
                                 kFocusedMonitor, 0);
                }
                return 1;   // ikisini de yut
            }
        }
    }

    // Always continue the chain. Returning 1 above is deliberate on the few
    // events we mean to swallow; dropping the rest would starve every other
    // application of mouse input.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// =============================================================================
// LowLevelKeyboardProc — take over the Windows Magnifier shortcuts
// =============================================================================
//
// Returning 1 swallows the event: it never reaches the chain, and Windows does
// not see it.
//
// Why a hook rather than RegisterHotKey:
//   Win+Plus and Win+Minus are reserved for the Windows Magnifier.
//   RegisterHotKey cannot claim them; it just fails. Swallowing the event in a
//   low-level hook is the only way to actually take a system shortcut.
//
// Taken over, while Win is held:
//   VK_OEM_PLUS  / VK_ADD      -> step zoom up   (turns zoom ON if it is off)
//   VK_OEM_MINUS / VK_SUBTRACT -> step zoom down (turns it OFF at minZoom)
//
//   Both the main row and the numpad, because the Windows Magnifier accepts
//   both and the muscle memory goes with it.
//
// Not swallowed: the Win key itself. Eating that would break the Start menu,
// Win+D, Win+E and everything else. Only the KeyDown of the paired key goes.
//
// The trade is deliberate: with takeover on, these keys no longer start the
// Windows Magnifier. That is the point.
//
// Out of reach entirely, protected by the kernel and Winlogon: Ctrl+Alt+Del
// and Win+L. No hook can intercept those.
//
// A hook also does not fire while a higher-integrity window has focus (Task
// Manager, a UAC prompt). Desktop Duplication does not work on the secure
// desktop either, so the boundary is already there and this adds nothing to it.
// =============================================================================
LRESULT CALLBACK InputThread::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && s_instance->m_target &&
        s_instance->m_hijackMagnifierKeys.load(std::memory_order_relaxed) &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        // Synthetic key events are ignored; see IgnoreInjectedInput.
        if (kb && (kb->flags & LLKHF_INJECTED) && IgnoreInjectedInput())
            return CallNextHookEx(nullptr, nCode, wParam, lParam);

        if (kb)
        {
            // 0x8000 is the "currently down" bit.
            const bool winDown =
                (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

            if (winDown)
            {
                const bool isPlus  = (kb->vkCode == VK_OEM_PLUS)  || (kb->vkCode == VK_ADD);
                const bool isMinus = (kb->vkCode == VK_OEM_MINUS) || (kb->vkCode == VK_SUBTRACT);

                if (isPlus || isMinus)
                {
                    PostMessageW(s_instance->m_target, WM_APP_ZOOM_STEP,
                                 isPlus ? kZoomIn : kZoomOut, 0);

                    // YUT — Windows Magnifier acilmasin.
                    return 1;
                }
            }

            // ── Panic exit: Ctrl+Alt+Shift+Q ──
            //
            // The overlay is fullscreen, topmost and opaque. If the render
            // thread wedges for any reason it stops pumping messages, WM_HOTKEY
            // never arrives, and the user is stranded behind an image of their
            // own desktop. That happened once and cost a trip to Task Manager.
            //
            // This runs on the input thread, which never blocks — its callbacks
            // only PostMessage and return — so the path survives a dead render
            // thread, which is the entire point.
            //
            // Hiding the overlays with ShowWindow instead would not do: the
            // windows belong to the render thread, and hiding them from another
            // thread needs that thread's cooperation. In a wedge that is
            // precisely what is missing. Ending the process always works.
            if (kb->vkCode == 'Q'
                && (GetAsyncKeyState(VK_CONTROL) & 0x8000)
                && (GetAsyncKeyState(VK_MENU)    & 0x8000)
                && (GetAsyncKeyState(VK_SHIFT)   & 0x8000))
            {
                // Key repeat would otherwise run this several times.
                static std::atomic<bool> panicStarted{false};
                if (!panicStarted.exchange(true, std::memory_order_relaxed))
                {
                    LOG_WARN("PANIK CIKISI (Ctrl+Alt+Shift+Q) — nazik kapatma deneniyor");

                    // Pointer first, before anything that can fail or stall.
                    // This shortcut exists for the case where the app has gone
                    // wrong, and "the app went wrong AND took my mouse pointer
                    // with it" is the state it must never leave behind.
                    SystemCursor::Restore();

                    // 1. Nazik yol: mesaj penceresine WM_CLOSE.
                    PostMessageW(s_instance->m_target, WM_CLOSE, 0, 0);

                    // 2. Hook icinde BEKLEMEK YASAK (LowLevelHooksTimeout).
                    //    Ayri, detached bir thread bekleyip gerekirse zorlar.
                    std::thread([]{
                        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                        LOG_ERROR("Nazik kapatma 1.5 sn'de tamamlanmadi — TerminateProcess");
                        // Restore again: the graceful path may have hung after
                        // re-hiding, and TerminateProcess runs no cleanup.
                        SystemCursor::Restore();
                        TerminateProcess(GetCurrentProcess(), 1);
                    }).detach();
                }
                return 1;   // Q'yu yut
            }
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// =============================================================================
// WinEventProc — klavye odagi degisti
// =============================================================================
// WINEVENT_SKIPOWNPROCESS sayesinde kendi pencerelerimiz buraya dusmez —
// panelde gezinirken zoom bolgesinin ziplamasi engellendi.
// =============================================================================
void CALLBACK InputThread::WinEventProc(
    HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG /*idChild*/,
    DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/)
{
    if (!hwnd || !s_instance || !s_instance->m_target)
        return;

    // A popup or menu just appeared: re-assert topmost immediately.
    //
    // Menus and dropdowns are created HWND_TOPMOST and created after us, so
    // they land above the overlay and the user sees the popup twice. Polling is
    // far too slow for a dropdown, which opens and is used inside a second.
    //
    // EVENT_OBJECT_SHOW fires constantly. Rate limiting lives on the engine
    // side, in App::MessageWndProc; this only reports.
    if (event == EVENT_SYSTEM_MENUPOPUPSTART || event == EVENT_OBJECT_SHOW)
    {
        PostMessageW(s_instance->m_target, WM_APP_ASSERT_TOPMOST, 0, 0);
        return;
    }

    if (event != EVENT_OBJECT_FOCUS)
        return;

    // idObject == OBJID_CLIENT: gercek bir kontrol odaklandi.
    // Menu/scrollbar/caret gibi alt nesneleri yoksayiyoruz — zoom bolgesini
    // her scrollbar tiklamasinda ziplatmak istemiyoruz.
    if (idObject != OBJID_CLIENT)
        return;

    if (s_instance->m_followMode.load(std::memory_order_relaxed) != FollowMode::MouseAndFocus)
        return;

    PostMessageW(s_instance->m_target, WM_APP_FOCUS_CHANGED, 0,
                 reinterpret_cast<LPARAM>(hwnd));
}

} // namespace BetterMagnifier
