// =============================================================================
// App — orchestrator
// =============================================================================
//
// Glue. Owns the components, starts them in the right order and wires them
// together; does no real work itself.
//
//   MonitorManager  which monitors exist, and the zoom state of each
//   D3DRenderer     the GPU device and one swap chain per monitor
//   DXGICapture     one Desktop Duplication session per monitor
//   OverlayWindow   one fullscreen click-through window per monitor
//   InputThread     the low-level hooks, on a thread of their own
//   HotkeyManager   the RegisterHotKey bindings
//   TrayIcon        tray menu
//
// The startup order is not arbitrary: MonitorManager first because the monitor
// count drives everything after it, then the D3D device, then the per-monitor
// overlay/swap chain/capture triple that depends on the device, and finally
// the hotkeys and tray, which need the message window.
// =============================================================================

#include "pch.h"
#include "App.h"
#include "SystemCursor.h"
#include "resource.h"
#include "Logger.h"

#include <wtsapi32.h>   // WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK

namespace BetterMagnifier {

App* App::s_instance = nullptr;

namespace {

// Should the panel be opened at STARTUP?
//
// The panel itself is always available now — the tray's Settings entry is
// unconditional. What stayed behind BM_PANEL=1 is opening it automatically,
// and that is a test affordance rather than a feature: the application requires
// administrator rights, so a script in an ordinary shell cannot click the tray
// or post the window a message (UIPI drops both), and asking the app to open
// its own panel is the only way to reach it from outside.
bool OpenPanelAtStartup()
{
    static const bool enabled = []() {
        wchar_t buf[8]{};
        const DWORD n = GetEnvironmentVariableW(L"BM_PANEL", buf, 8);
        return (n > 0 && n < 8 && buf[0] == L'1');
    }();
    return enabled;
}

// =============================================================================
// LaunchedAtStartup — did the logon task start us?
// =============================================================================
// The scheduled task passes --startup, and logon is the one moment a
// remembered zoom is wrong: the machine comes back magnified at whatever level
// the last session happened to leave, before anyone has asked for it. At logon
// every monitor stays at 1.00x and the remembered level is ignored for the
// whole session, so nothing carries a level across a boot either.
// =============================================================================
bool LaunchedAtStartup()
{
    static const bool enabled =
        std::wcsstr(GetCommandLineW(), L"--startup") != nullptr;
    return enabled;
}

// =============================================================================
// ApplyStartWithWindows — a scheduled task, not a Run entry
// =============================================================================
// The HKCU Run entry this used to write never actually launched anything: the
// binary is RequireAdministrator, and Windows drops an elevated Run entry at
// logon rather than prompting for UAC. A logon-triggered task with RL HIGHEST
// is the documented way round it, and schtasks.exe ships with Windows, so
// there is no Task Scheduler COM plumbing to own.
//
// The stale Run value is deleted on every call, so upgrading from the version
// that wrote one does not leave it behind.
// =============================================================================
constexpr wchar_t kTaskName[] = L"BetterMagnifier";

bool RunSchtasks(const std::wstring& args)
{
    wchar_t sysDir[MAX_PATH]{};
    if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0)
    {
        LOG_ERROR("GetSystemDirectoryW failed: {}", GetLastError());
        return false;
    }

    // CreateProcessW rather than system(): CREATE_NO_WINDOW keeps a console
    // from flashing over the desktop every time a setting changes.
    std::wstring cmd = L"\"" + std::wstring(sysDir) + L"\\schtasks.exe\" " + args;

    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        LOG_ERROR("Could not launch schtasks: {}", GetLastError());
        return false;
    }

    WaitForSingleObject(pi.hProcess, 10000);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return code == 0;
}

void RemoveLegacyRunEntry()
{
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        RegDeleteValueW(key, kTaskName);
        RegCloseKey(key);
    }
}

bool ApplyStartWithWindows(bool enable)
{
    RemoveLegacyRunEntry();

    if (!enable)
    {
        // schtasks returns non-zero for "no such task" as well as for a real
        // failure, and the requested end state holds either way.
        RunSchtasks(L"/Delete /TN \"" + std::wstring(kTaskName) + L"\" /F");
        return true;
    }

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
    {
        LOG_ERROR("GetModuleFileNameW failed: {}", GetLastError());
        return false;
    }

    // /TR takes the whole command as one argument, and a path with spaces needs
    // its own quotes inside that argument — hence the escaped pair.
    const std::wstring args =
        L"/Create /TN \"" + std::wstring(kTaskName) + L"\""
        L" /TR \"\\\"" + std::wstring(exePath) + L"\\\" --startup\""
        L" /SC ONLOGON /RL HIGHEST /F";

    const bool ok = RunSchtasks(args);
    if (!ok)
        LOG_ERROR("Could not register the logon task");

    return ok;
}

// =============================================================================
// OsMagnifierRunning — is the Windows Magnifier up?
// =============================================================================
//
// Two magnifiers at once is a genuinely confusing state, and it is easy to end
// up in by accident: Win+Plus reaches the OS Magnifier as well as us, because
// that process runs with UIAccess and a hook from here cannot swallow its
// shortcut. Once it is up it magnifies our overlay, so the screen shows our
// magnification of its magnification and neither responds the way you expect.
//
// Detected by window class rather than process name: Magnify.exe also runs
// briefly for the accessibility settings UI without actually magnifying, and
// the window is what says it is really on.
//
// Only reported. Closing another application is the user's call, not ours.
// =============================================================================
bool OsMagnifierRunning()
{
    return FindWindowW(L"Screen Magnifier Window", nullptr) != nullptr;
}

} // anonymous namespace

App::~App()
{
    Shutdown();
}

bool App::Initialize(HINSTANCE hInstance)
{
    if (m_initialized)
        return true;

    m_hInstance = hInstance;
    s_instance  = this;

    LOG_INFO("App starting");

    if (!CreateMessageWindow())
        return false;

    if (!InitializeComponents())
        return false;

    SetupCallbacks();

    m_initialized = true;
    LOG_INFO("App ready — Ctrl+Alt+Z toggles zoom, Ctrl+Alt+X freezes");
    return true;
}

// =============================================================================
// CreateMessageWindow — an invisible but real window
// =============================================================================
//
// A real WS_POPUP that is simply never shown, not a message-only window.
// RegisterHotKey and Shell_NotifyIcon both work with HWND_MESSAGE, but the
// tray's context menu does not: TrackPopupMenu needs an owner that can become
// foreground in order to dismiss correctly, and a message-only window cannot.
// =============================================================================
bool App::CreateMessageWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MessageWndProc;
    wc.hInstance     = m_hInstance;
    wc.lpszClassName = kMsgWindowClass;

    // This window is never shown, but it is the process' owner window, and that
    // is what Alt+Tab, the UAC prompt and Task Manager pick the icon up from.
    wc.hIcon   = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc))
    {
        LOG_ERROR("Message window class registration failed: {}", GetLastError());
        return false;
    }

    m_messageHwnd = CreateWindowExW(
        0,
        kMsgWindowClass,
        L"BetterMagnifier",
        WS_POPUP,               // no border, no title bar
        0, 0, 0, 0,             // zero sized: it is never shown
        nullptr, nullptr,
        m_hInstance,
        nullptr);

    if (!m_messageHwnd)
    {
        LOG_ERROR("Message window could not be created: {}", GetLastError());
        return false;
    }

    // Ask for lock/unlock notifications. Locking switches to the secure
    // desktop, which detaches our low-level hooks for good; we reinstall them
    // on unlock.
    if (!WTSRegisterSessionNotification(m_messageHwnd, NOTIFY_FOR_THIS_SESSION))
    {
        LOG_WARN("WTSRegisterSessionNotification failed ({}), hooks will not "
                 "recover automatically after unlock", GetLastError());
    }

    LOG_DEBUG("Message window created: HWND=0x{:X}",
        reinterpret_cast<uintptr_t>(m_messageHwnd));
    return true;
}

// Reinstall everything the secure desktop tore down.
//
// Capture recovers on its own (DXGICapture::Reinitialize retries twice a
// second), but the low-level hooks do not: Windows detaches them and never
// puts them back, so Win+Plus and Ctrl+Alt+wheel stay dead. RegisterHotKey
// bindings usually survive, and re-registering them is cheap insurance.
void App::OnSessionUnlock()
{
    LOG_INFO("Session unlocked, reinstalling input hooks");

    m_inputThread.Stop();

    if (!m_inputThread.Start(m_messageHwnd,
                             m_settings.General().followMode,
                             m_settings.General().hijackMagnifierKeys))
    {
        LOG_ERROR("Could not reinstall input hooks after unlock");
    }

    const UINT failedMask = m_hotkeyManager.Reregister(m_settings.General());
    m_status.hotkeyFailedMask.store(failedMask, std::memory_order_release);
}

bool App::InitializeComponents()
{
    // Settings first; everything below can depend on them. A missing file is
    // not an error, it is a first run.
    m_settings.Load();

    if (!m_monitorManager.Initialize())
    {
        LOG_ERROR("MonitorManager initialisation failed");
        return false;
    }

    // ── GPU device ──
    if (!m_renderer.Initialize())
    {
        LOG_ERROR("D3DRenderer initialisation failed");
        return false;
    }

    m_cursorCache.Initialize(m_renderer.GetDevice());
    m_osdCache.Initialize(m_renderer.GetDevice());

    // ── Per-monitor: overlay + swap chain + capture ──
    const size_t monitorCount = m_monitorManager.GetMonitorCount();

    // reserve matters: growing the vector moves its elements, and these own
    // HWNDs and COM pointers. Without it a reallocation drags every overlay
    // through a move and a destroy for no reason.
    m_overlays.reserve(monitorCount);
    m_captures.reserve(monitorCount);

    for (size_t i = 0; i < monitorCount; ++i)
    {
        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        OverlayWindow overlay;
        if (!overlay.Create(m_hInstance, *mon, i))
        {
            LOG_ERROR("Overlay creation failed for monitor {}, skipping it", i);
            continue;
        }

        if (!m_renderer.CreateSwapChainForWindow(
                overlay.GetHwnd(),
                static_cast<UINT>(mon->Width()),
                static_cast<UINT>(mon->Height()),
                i))
        {
            // Keep the overlay anyway so the hotkeys and tray still work.
            LOG_ERROR("Swap chain creation failed for monitor {}", i);
        }

        DXGICapture capture;
        if (mon->dxgiOutput)
        {
            if (!capture.Initialize(m_renderer.GetDevice(), mon->dxgiOutput.Get()))
                LOG_ERROR("Capture initialisation failed for monitor {}", i);
        }
        else
        {
            LOG_WARN("No DXGI output for monitor {}, it cannot be captured", i);
        }

        m_overlays.push_back(std::move(overlay));
        m_captures.push_back(std::move(capture));
    }

    if (m_overlays.empty())
    {
        LOG_ERROR("No overlay could be created for any monitor");
        return false;
    }

    m_hotkeyManager.Initialize(m_messageHwnd, m_settings.General());
    m_trayIcon.Create(m_messageHwnd, m_hInstance);

    m_status.hotkeyFailedMask.store(m_hotkeyManager.LastFailedMask(),
                                    std::memory_order_release);

    // The hooks live on their own thread, not this one; see InputThread.h.
    // Failure here costs wheel zoom and pointer scaling but not the app.
    //
    // Attach and the first layout publish must both happen BEFORE Start: the
    // hook can fire on the very next mouse move, and it reads both.
    m_inputThread.Attach(&m_viewport, &m_viewportSnapshot);
    PublishViewportRequests(true);

    if (!m_inputThread.Start(m_messageHwnd,
                             m_settings.General().followMode,
                             m_settings.General().hijackMagnifierKeys))
    {
        LOG_WARN("InputThread failed to start — wheel zoom and pointer scaling are off");
    }

    PublishMonitorInfo();

    // Push the loaded settings into the input thread. Without this the pointer
    // and edge-push settings sat unread until the user changed one.
    ApplyPointerSettings();

    // Same reasoning, for the render loop's rate ceiling: it depends on the
    // MaxFps setting and on each monitor's refresh rate, and neither is known
    // until both the settings and the monitors have been read.
    ApplyFrameRateCaps();

    // Same reason, plus a migration: a copy that had this on was carrying a Run
    // entry Windows never honoured, and only a settings change would have
    // replaced it with the logon task.
    // Only when it is on: the off case would spawn schtasks on every launch to
    // delete a task that is not there. Turning it off goes through the panel,
    // which calls ApplySettings.
    if (m_settings.General().startWithWindows)
        ApplyStartWithWindows(true);
    else
        RemoveLegacyRunEntry();

    // Nothing can be mid-download here, so an abandoned update leaves nothing
    // behind.
    ClearUpdateStagingDir();

    m_updateChecker.Start(m_messageHwnd, &m_status);

    if (OpenPanelAtStartup())
        OnShowPanel();

    return true;
}

// =============================================================================
// SetupCallbacks — wire the components together
// =============================================================================
// The lambdas capture "this" and stay valid for as long as App does, which is
// the whole process lifetime.
// =============================================================================
void App::SetupCallbacks()
{
    m_hotkeyManager.SetToggleZoomCallback([this] { OnToggleZoom(); });
    m_hotkeyManager.SetFreezeCallback([this] { OnFreeze(); });
    // Wheel zoom arrives from InputThread as WM_APP_ZOOM_STEP.

    m_trayIcon.SetToggleCallback([this] { OnToggleZoom(); });
    m_trayIcon.SetExitCallback([] { PostQuitMessage(0); });

    m_trayIcon.SetSettingsCallback([this] { OnShowPanel(); });

    // A direct call would work - the tray callback is already on this thread -
    // but routing it like the panel keeps one path into the updater.
    m_trayIcon.SetCheckUpdateCallback([this] {
        PostMessageW(m_messageHwnd, WM_APP_UPDATE_ACTION, kUpdateCheckNow, 0);
    });
}

// =============================================================================
// Run — hybrid message and render loop
// =============================================================================
//
// PeekMessage rather than GetMessage: GetMessage blocks until a message
// arrives, which would stop the render loop dead. Peek returns immediately
// when the queue is empty and the frame gets rendered in that gap.
//
// Pacing comes from the per-monitor FramePacers and the FrameWaiter at the
// end of Update, not from Present.
// =============================================================================
int App::Run()
{
    if (!m_initialized)
    {
        LOG_ERROR("Run() called before Initialize()");
        return 1;
    }

    m_running = true;
    MSG msg{};

    LOG_INFO("Message loop started");

    while (m_running)
    {
        // Drain the queue before rendering.
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!m_running)
            break;

        Update();
    }

    LOG_INFO("Message loop ended (exit code: {})", static_cast<int>(msg.wParam));
    return static_cast<int>(msg.wParam);
}

// =============================================================================
// PublishViewportRequests — render -> input thread
// =============================================================================
//
// Zoom is mutated from eight places in this file. Rather than notifying the
// input thread at each of them — where the failure mode of forgetting one is a
// silent desync — the settled value is published once per frame. A call site
// cannot forget to be included in a loop it does not know about.
// =============================================================================
void App::PublishViewportRequests(bool bumpLayout)
{
    const size_t count = m_overlays.size();
    m_viewportSnapshot.monitorCount.store(count, std::memory_order_relaxed);

    // Whether anything the input thread acts on actually moved. Only the
    // render thread writes these, so reading them back to compare is not a
    // race, and it keeps the answer here rather than in a shadow copy that
    // could drift from what was published.
    bool changed = bumpLayout;

    for (size_t i = 0; i < count; ++i)
    {
        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        MonitorViewportAtomic& a = m_viewportSnapshot.Monitor(i);

        // Zoom off means zoom 1: the controller must not keep panning a
        // monitor that is no longer magnified.
        const double zoom = mon->zoom.isActive
                          ? static_cast<double>(mon->zoom.zoomLevel) : 1.0;

        if (a.requestedZoom.load(std::memory_order_relaxed) != zoom)
            changed = true;

        a.requestedZoom.store(zoom, std::memory_order_relaxed);

        a.originX.store(mon->bounds.left, std::memory_order_relaxed);
        a.originY.store(mon->bounds.top,  std::memory_order_relaxed);
        a.width.store(mon->Width(),  std::memory_order_relaxed);
        a.height.store(mon->Height(), std::memory_order_relaxed);

        if (a.frozen.load(std::memory_order_relaxed) != mon->zoom.isFrozen)
            changed = true;

        a.frozen.store(mon->zoom.isFrozen, std::memory_order_relaxed);
    }

    // Release-ordered last, so the input thread that sees the new epoch also
    // sees every rect written above it.
    if (bumpLayout)
        m_viewportSnapshot.layoutEpoch.fetch_add(1, std::memory_order_release);

    // Wake the input thread rather than let the request sit until its 16 ms
    // sync timer. This runs every frame, so it has to be conditional: an
    // unconditional post would be a thread message per frame per monitor for
    // a value that had not moved.
    if (changed)
        m_inputThread.RequestSync();
}

// =============================================================================
// UpdatePointerCompositing — hide and restore the real pointer
// =============================================================================
//
// The exposure window is exactly "the pointer is on a magnified monitor", and
// it has to be that narrow rather than "some monitor is magnified". Hiding the
// system pointer is global while the sprite is drawn only over magnified
// content, so the wider condition left the user with no pointer at all the
// moment it moved onto an unmagnified display. That was invisible for as long
// as the monitor lock had no way out; the break-out made it reachable.
//
// Gated on MagPathAvailable, and that gate is load-bearing rather than
// defensive. Hiding the pointer without MagShowSystemCursor means
// SetSystemCursor, whose effect outlives the process; if we drew a sprite
// without hiding, the user would see two pointers in different places, which
// is worse than one in the wrong place. So when the safe hide is unavailable
// the whole feature stays off and the pointer behaves natively.
// =============================================================================
void App::UpdatePointerCompositing(bool anyMonitorZoomed, bool pointerOnMagnified)
{
    const bool want = pointerOnMagnified
                   && m_settings.General().pointerScaling
                   && SystemCursor::MagPathAvailable()
                   && !m_pointerCompositingBroken;

    // Checked on the transition into magnifying rather than per frame: that is
    // the moment it matters and it costs one FindWindow.
    if (anyMonitorZoomed && !m_wasZoomed)
    {
        const bool clash = OsMagnifierRunning();
        m_status.osMagnifierRunning.store(clash, std::memory_order_relaxed);

        if (clash)
        {
            LOG_WARN("Windows Magnifier is running as well. It magnifies our overlay, so "
                     "the two stack and neither behaves as expected. Close it (Win+Esc).");
        }
    }
    else if (!anyMonitorZoomed)
    {
        m_status.osMagnifierRunning.store(false, std::memory_order_relaxed);
    }
    m_wasZoomed = anyMonitorZoomed;

    if (want == m_pointerCompositing)
        return;

    m_pointerCompositing = want;

    // Order matters on the way in: enable scaling first so the pointer is
    // already being tracked when it disappears. On the way out, show the real
    // pointer before releasing control, so there is never a frame with none.
    if (want)
    {
        m_inputThread.Pointer().SetEnabled(true);
        SystemCursor::Hide();
    }
    else
    {
        SystemCursor::Restore();
        m_inputThread.Pointer().SetEnabled(false);
    }
}

// =============================================================================
// UpdateOsd — raise a readout when a monitor's state moves
// =============================================================================
//
// Every zoom change used to be silent. The level lived in the control panel,
// which is off by default and behind a tray menu, so stepping with Win+Plus or
// the wheel changed the picture and told you nothing about where you had got
// to. On a magnified screen that matters more than usual: the content looks
// much the same at 3x and at 4x until you go looking for a landmark.
//
// Called for active monitors only, but the previous state is recorded for every
// monitor including idle ones. Otherwise turning zoom on at the same level it
// was last used at reads as "nothing changed" and says nothing.
// =============================================================================
void App::UpdateOsd(size_t monitorIndex, const MonitorInfo& mon)
{
    const size_t slot = (monitorIndex < StatusSnapshot::kMaxMonitors)
                      ? monitorIndex : StatusSnapshot::kMaxMonitors - 1;
    OsdState& osd = m_osd[slot];

    const bool  active = mon.zoom.isActive;
    const bool  frozen = mon.zoom.isFrozen;
    const float zoom   = mon.zoom.zoomLevel;

    // Quantised to what the readout can actually show. Without this a zoom that
    // differs in the fourth decimal re-raises the OSD every frame and it never
    // goes away.
    const float shown = std::round(zoom * 100.0f) / 100.0f;

    const bool first = !osd.seen;
    osd.seen = true;

    const bool becameActive   = active && !osd.lastActive;
    const bool becameInactive = !active && osd.lastActive;
    const bool zoomMoved      = active && osd.lastActive && (shown != osd.lastZoom);
    const bool freezeMoved    = active && osd.lastActive && (frozen != osd.lastFrozen);

    osd.lastActive = active;
    osd.lastZoom   = shown;
    osd.lastFrozen = frozen;

    // Nothing on the first observation. Starting up with zoom already on would
    // otherwise greet the user with a readout they did not ask for.
    if (first)
        return;

    if (!becameActive && !becameInactive && !zoomMoved && !freezeMoved)
        return;

    osd.persistent = false;

    // Stepping down past minZoom closes magnification, and the readout has to
    // say so. Left to itself the last level raised — "1.25x", the step above the
    // floor — simply stayed up for its remaining duration with the view already
    // back to normal size, which reads as a magnifier that has stopped working
    // rather than one that has been switched off.
    if (becameInactive)
    {
        osd.text  = L"Off";
        osd.until = std::chrono::steady_clock::now() + kOsdDuration;
        return;
    }

    // Frozen is a state, not an event, and it is treated as one: the readout
    // stays up for as long as the freeze lasts. A view that has quietly stopped
    // following the pointer looks identical to one that is broken, and this is
    // the difference between the two.
    //
    // It fades rather than holding full strength, though. Persistent and opaque,
    // it is a permanent pill over the bottom of a screen the user is trying to
    // read; faded, it still answers "why has this stopped moving" without
    // occupying the display to do it.
    if (frozen)
    {
        osd.text       = L"Frozen";
        osd.until      = std::chrono::steady_clock::now() + kOsdDuration;
        osd.persistent = true;
        return;
    }

    osd.text  = freezeMoved ? std::wstring(L"Live")
                            : std::format(L"{:.2f}x", shown);
    osd.until = std::chrono::steady_clock::now() + kOsdDuration;
}

// =============================================================================
// Update — once per loop iteration
// =============================================================================
void App::Update()
{
    m_status.monitorCount.store(m_overlays.size(), std::memory_order_relaxed);
    m_presentedThisTick = false;

    // Deferred to the first frame: the check is a network call, and how
    // promptly the magnifier appears matters more. Costs an exchange and a
    // SetEvent.
    if (!m_updateCheckStarted)
    {
        m_updateCheckStarted = true;
        MaybeCheckForUpdates();
    }

    PublishViewportRequests(false);

    // ── Follow the pointer ──
    POINT cursor{};
    GetCursorPos(&cursor);

    bool   anyActive = false;
    bool   pointerOnMagnified = false;
    size_t activeCount = 0;
    float  singleZoom  = 1.0f;   // only meaningful when activeCount == 1
    const size_t count = m_overlays.size();

    for (size_t i = 0; i < count; ++i)
    {
        MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        // ── Publish to the snapshot; the GUI thread reads it at 10 Hz ──
        // Idle monitors have to be reported too, so this comes BEFORE the check.
        auto& st = m_status.Monitor(i);
        st.zoomLevel.store(mon->zoom.zoomLevel, std::memory_order_relaxed);
        st.isActive.store(mon->zoom.isActive, std::memory_order_relaxed);
        st.isFrozen.store(mon->zoom.isFrozen, std::memory_order_relaxed);
        st.captureOk.store(
            m_captures[i].IsInitialized() && !m_captures[i].NeedsReinit(),
            std::memory_order_relaxed);
        st.captureExcluded.store(m_overlays[i].IsExcludedFromCapture(),
                                 std::memory_order_relaxed);

        // Before the inactive early-out: an idle monitor's state still has to
        // be recorded, or turning zoom back on at the same level reads as no
        // change at all and says nothing.
        UpdateOsd(i, *mon);

        const size_t slot = (i < StatusSnapshot::kMaxMonitors)
                          ? i : StatusSnapshot::kMaxMonitors - 1;

        // A blocked Win+Plus queues a "Disabled" readout (see OnZoomStep) —
        // that still needs a frame drawn to actually show it, so it is the
        // one thing that keeps an inactive monitor out of the early-out below.
        // A persistent readout does not expire, but it only belongs to an active
        // monitor — freeze is the only thing that sets it — so it is not part of
        // this test.
        const bool osdPending = !m_osd[slot].text.empty()
                              && std::chrono::steady_clock::now() < m_osd[slot].until;

        // ── Zoom off: hide the overlay, leave the capture alone ──
        if (!mon->zoom.isActive && !osdPending)
        {
            if (m_overlays[i].IsVisible())
                m_overlays[i].Hide();

            // FPS is meaningless with zoom off; zero it so the panel shows a dash.
            st.fps.store(0.0f, std::memory_order_relaxed);
            m_lastFrameTime[slot] = {};

            // Forget the cadence too. Turning zoom back on minutes later must
            // not be held back by a deadline computed for the last session.
            m_pacers[slot].Reset();

            // Clear the remembered rect too. Without this, zooming back in on
            // the same region reads as "nothing changed" and the draw is
            // skipped — and after a Present the back buffer contents are
            // undefined, so what appears on screen is garbage.
            m_lastSrcRect[slot] = RECT{};

            // And close the duplication session. Left open, it costs the OS a
            // copy of every desktop update on this monitor whether or not we
            // ever acquire one — a game here paid for a magnifier that was not
            // magnifying it. See DXGICapture::Suspend.
            if (m_captures[i].Suspend())
                m_renderer.ReleaseSourceTexture(i);

            st.captureIdle.store(true, std::memory_order_relaxed);
            continue;
        }

        st.captureIdle.store(false, std::memory_order_relaxed);

        // The session was closed while this monitor was idle, so zoom-on now
        // has to reopen it, and the renderer has no frame to draw until the
        // new session delivers one. m_awaitFirstFrame lets RenderMonitor wait
        // briefly for it, so the overlay is shown and presented in the same
        // tick as before rather than sitting on screen with nothing drawn.
        if (m_captures[i].Resume())
        {
            m_awaitFirstFrame[slot] = true;
        }
        else if (m_captures[i].NeedsReinit())
        {
            // Duplication is lost on things like a fullscreen game taking the
            // output. Retrying is free; failure just means trying again next
            // frame.
            m_captures[i].Reinitialize();
        }

        if (mon->zoom.isActive)
        {
            anyActive = true;
            ++activeCount;
            singleZoom = mon->zoom.zoomLevel;

            if (PtInRect(&mon->bounds, cursor))
                pointerOnMagnified = true;
        }

        if (!m_overlays[i].IsVisible())
            m_overlays[i].Show();

        RenderMonitor(i);
    }

    UpdatePointerCompositing(anyActive, pointerOnMagnified);

    // Published every frame rather than at each of the eight places zoom is
    // changed. SetState compares first and only calls the shell when the state
    // it would show has actually moved.
    m_trayIcon.SetState(activeCount, singleZoom);

    // A periodic backstop only. The real mechanism is event-driven
    // (WM_APP_ASSERT_TOPMOST); this catches whatever slips past it, and is
    // rate-limited inside AssertOverlaysTopmost.
    if (anyActive)
        AssertOverlaysTopmost(true);

    ApplyRenderThreadPriority(anyActive);

    // -- Idle until the next monitor is due a frame --
    //
    // This is where the loop's rate is actually decided. FrameWaiter rather
    // than Sleep, for the reasons in FramePacer.h.
    auto wait = TimeUntilNextFrame(std::chrono::steady_clock::now());

    if (!anyActive)
    {
        // Nothing magnified: the loop only has to notice a hotkey, and one of
        // those arrives as a message and breaks the wait anyway.
        wait = std::chrono::milliseconds(8);
    }
    else if (wait == std::chrono::steady_clock::duration::max()
          || wait <= std::chrono::steady_clock::duration::zero())
    {
        // No pacer has a deadline, which means they are all uncapped - the
        // flip-model overlay, where Present(1) blocks on vblank and is the
        // brake. It only brakes when something was presented, so an idle tick
        // still needs a floor under it.
        wait = m_presentedThisTick
             ? std::chrono::steady_clock::duration::zero()
             : std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                   std::chrono::milliseconds(4));
    }

    // A backstop on the wait itself. At a hand-set MaxFps of 10 the interval
    // is 100 ms, and going that long without looking at capture recovery or
    // the readout's expiry makes the application feel stuck even though it is
    // doing exactly what it was told.
    const auto kMaxIdle =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(50));

    m_frameWaiter.WaitOrMessage((std::min)(wait, kMaxIdle));
}

// =============================================================================
// ApplyFrameRateCaps - what each monitor's pacer is aiming for
// =============================================================================
//
// Two inputs and either can move on its own: the user's MaxFps setting, and
// the monitor's refresh rate, which changes when a display is swapped, rotated
// or reconfigured. So this is called from Initialize, from ApplySettings and
// from OnDisplayChange rather than computed once.
// =============================================================================
void App::ApplyFrameRateCaps()
{
    // Under BM_OVERLAY_FLIP the swap chain is flip-model and Present gets
    // SyncInterval 1, which is this same brake applied by the display itself
    // and applied more accurately. Pacing on top of it would only add a
    // second, coarser cadence beating against the first.
    const bool vSyncPaces = UseFlipOverlay();
    const unsigned maxFps = m_settings.General().maxFps;

    for (size_t i = 0; i < StatusSnapshot::kMaxMonitors; ++i)
    {
        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        const unsigned refresh = mon ? mon->refreshRate : 0u;

        m_pacers[i].SetCap(vSyncPaces ? 0u : ResolveFrameRateCap(maxFps, refresh));
    }

    if (vSyncPaces)
    {
        LOG_INFO("Frame pacing off: the flip-model overlay presents with vSync");
    }
    else
    {
        LOG_INFO("Frame pacing: MaxFps={} ({}), monitor 0 capped at {} fps, "
                 "high-resolution timer {}",
                 maxFps,
                 maxFps == 0 ? "follow the display" : "explicit override",
                 m_pacers[0].Cap(),
                 m_frameWaiter.IsHighResolution() ? "yes" : "no");
    }
}

// =============================================================================
// TimeUntilNextFrame - how long the loop may idle
// =============================================================================
// The soonest deadline across the monitors that are actually magnified.
// duration::max() means none of them has one, which is either "nothing is
// magnified" or "every pacer is uncapped"; the caller distinguishes.
// =============================================================================
std::chrono::steady_clock::duration App::TimeUntilNextFrame(
    std::chrono::steady_clock::time_point now) const
{
    using duration = std::chrono::steady_clock::duration;

    duration soonest = duration::max();

    for (size_t i = 0; i < m_overlays.size(); ++i)
    {
        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon || !mon->zoom.isActive)
            continue;

        const size_t slot = (i < StatusSnapshot::kMaxMonitors)
                          ? i : StatusSnapshot::kMaxMonitors - 1;

        const duration d = m_pacers[slot].TimeUntilDue(now);
        if (d <= duration::zero())
            return duration::zero();

        soonest = (std::min)(soonest, d);
    }

    return soonest;
}

// =============================================================================
// ApplyRenderThreadPriority
// =============================================================================
//
// A magnifier competes for the CPU with the application it is magnifying, and
// under a fullscreen game that competition is one it loses: the view stutters
// exactly when the user most needs it to be readable. One class above normal
// is enough to be scheduled promptly without pre-empting anything that
// matters, and it is given back the moment nothing is magnified so an idle
// tray application is not sitting above everything else on the machine.
//
// Latched, because SetThreadPriority every tick is a syscall for nothing.
// =============================================================================
void App::ApplyRenderThreadPriority(bool anyActive)
{
    if (anyActive == m_renderPriorityRaised)
        return;

    const int priority = anyActive ? THREAD_PRIORITY_ABOVE_NORMAL
                                   : THREAD_PRIORITY_NORMAL;

    if (!SetThreadPriority(GetCurrentThread(), priority))
    {
        // Not fatal, and not worth retrying every tick: latch anyway so the
        // log does not fill up with it.
        LOG_WARN("SetThreadPriority failed: {}", GetLastError());
    }
    else
    {
        LOG_DEBUG("Render thread priority {}", anyActive ? "raised" : "restored");
    }

    // EcoQoS, the other half. With a fullscreen game in the foreground Windows
    // 11 treats this process as background work and may throttle it — on a
    // hybrid CPU that means efficiency cores, where a thread priority does not
    // follow it. Opt out while magnifying; hand the decision back to the
    // system (ControlMask 0) when idle, where throttling a tray icon is right.
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = anyActive ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    throttling.StateMask   = 0;   // with the bit controlled: throttling off

    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                               &throttling, sizeof(throttling)))
    {
        LOG_WARN("SetProcessInformation(ProcessPowerThrottling) failed: {}", GetLastError());
    }

    m_renderPriorityRaised = anyActive;
}

// =============================================================================
// RenderMonitor — capture, render and present one monitor
// =============================================================================
//
// The arithmetic: on a 1920x1080 monitor at 2x, the source region is 960x540,
// stretched across the full 1920x1080 overlay. Where that region sits comes
// from ViewportController, not from here.
// =============================================================================
void App::RenderMonitor(size_t monitorIndex)
{
    if (monitorIndex >= m_captures.size())
        return;

    MonitorInfo* mon = m_monitorManager.GetMonitor(monitorIndex);
    if (!mon)
        return;

    auto& capture = m_captures[monitorIndex];
    if (!capture.IsInitialized())
        return;

    // -- Frame pacing --
    //
    // The gate is here, before the capture, rather than around the Present.
    // Everything in this function costs something the display cannot show more
    // often than once per refresh: acquiring a duplication frame, copying a
    // whole monitor's worth of pixels, a fullscreen shader pass, and on a
    // layered window a DWM surface update of the entire monitor. Doing any of
    // that twice between two refreshes is work taken from whatever application
    // is being magnified, which under a fullscreen game is the whole problem.
    //
    // Skipping the acquire is not skipping the frame: Desktop Duplication
    // accumulates while we are not asking, and hands over the composed result
    // with the metadata for everything that changed in the meantime.
    const size_t pacerSlot = (monitorIndex < StatusSnapshot::kMaxMonitors)
                           ? monitorIndex : StatusSnapshot::kMaxMonitors - 1;

    const auto tickNow = std::chrono::steady_clock::now();
    if (!m_pacers[pacerSlot].Due(tickNow))
        return;

    m_pacers[pacerSlot].Serviced(tickNow);

    // Timeout 0: take a frame if one is ready, otherwise return immediately.
    // Blocking would be wrong — with no new frame we still need to re-present
    // the previous one, or the view would not follow a moving source rect.
    //
    // The one exception is the first acquire after a resume. There is no
    // previous frame then, so not waiting means not drawing, and the overlay
    // that was just shown sits there with whatever it last held. The first
    // frame of a new session is the current desktop and normally arrives at
    // once; the bound is only there so a session that never delivers cannot
    // stall the loop. Spent after one attempt whatever the outcome.
    UINT acquireTimeout = 0;
    if (m_awaitFirstFrame[pacerSlot])
    {
        m_awaitFirstFrame[pacerSlot] = false;
        acquireTimeout = kFirstFrameTimeoutMs;
    }

    CapturedFrame frame = capture.AcquireFrame(acquireTimeout);

    // -- Keep the intermediate texture current, whatever we decide to draw --
    //
    // Straight after the acquire, before any path can return: the store is
    // updated with only the regions that changed, which is correct only if no
    // new frame is ever absorbed without being copied. A frame skipped here
    // would leave its regions stale with nothing left to repaint them.
    //
    // Deliberately not inside the skip test further down, for the same
    // reason: the renderer's copy is also the last-frame store the anchor pans
    // across. Copy always, draw sometimes.
    const bool haveNewImage = frame.isNewFrame && frame.texture;

    if (haveNewImage)
    {
        const auto& dirty = capture.DirtyRects();
        m_renderer.UpdateSourceTexture(monitorIndex, frame.texture.Get(),
            capture.DirtyKnown() ? dirty.data() : nullptr,
            capture.DirtyKnown() ? dirty.size() : 0);
    }

    // Drawn even when no new frame arrived.
    //
    // Desktop Duplication produces nothing while the screen content is static.
    // Rendering only on isNewFrame meant that on a still screen, moving the
    // mouse left the magnified region frozen where it was. The renderer keeps a
    // copy of the last frame, so a moved source rect can be re-sampled from it.
    {
        // Zoom comes from the SNAPSHOT, not from MonitorManager, and the
        // distinction is the whole fix for the slide-in artefact while zooming.
        //
        // srcOrigin is computed by the controller for the zoom the controller
        // has applied. Sizing the rect from MonitorManager's zoom instead meant
        // that during the window between the render thread publishing a new
        // zoom and the input thread applying it — up to one sync tick, and
        // every single step of a zoom ramp — the origin and the extent belonged
        // to different zoom levels. The rect was simply wrong, and it visibly
        // slid into place as the two caught up.
        //
        // Reading both from the same source makes them consistent by
        // construction. The cost is that a zoom change can appear one tick
        // late, which is invisible; an inconsistent rect was not.
        const MonitorViewportAtomic& vp = m_viewportSnapshot.Monitor(monitorIndex);

        // ── An inactive monitor renders 1:1, whatever the snapshot still says ──
        //
        // This monitor is only being drawn at all because a readout is up on it
        // ("Disabled", "Off"), and the overlay under that readout has to be an
        // exact copy of the desktop or it announces itself.
        //
        // The snapshot is not that. It carries the LAST APPLIED viewport, and
        // the input thread only revisits it when it next syncs — so between zoom
        // being switched off here and that sync happening over there, vp still
        // holds the old zoom and the old origin. Rendering those put a magnified,
        // offset copy of the desktop on screen for the frame or two before the
        // input thread caught up: the flick of zoom seen when Win+Plus is pressed
        // on a monitor the user has disabled.
        const bool live = mon->zoom.isActive;

        const float zoom = live
            ? (std::max)(static_cast<float>(vp.zoom.load(std::memory_order_relaxed)),
                         ZoomState::kMinZoom)
            : 1.0f;

        // With no new frame the width and height come back zero; fall back to
        // the monitor size, which is what the capture was opened at anyway.
        const long monW = (frame.width  > 0) ? static_cast<long>(frame.width)  : mon->Width();
        const long monH = (frame.height > 0) ? static_cast<long>(frame.height) : mon->Height();

        if (monW <= 0 || monH <= 0)
            return;

        const long srcW = (std::max)(1L, static_cast<long>(monW / zoom));
        const long srcH = (std::max)(1L, static_cast<long>(monH / zoom));

        // ── Source origin comes from ViewportController ──
        //
        // Not the anchored identity srcOrigin = focal * (1 - 1/zoom), which
        // would track the cursor on every move and leave no room for edge push.
        // Click alignment comes from the sprite instead: the real cursor is
        // kept at round(pointer) and the sprite is drawn where the user sees it.
        //
        // The controller lives on the input thread and advances per mouse
        // event, so the pan stays proportional to mouse motion rather than to
        // frame rate.
        RECT srcRect{};
        srcRect.left = live ? static_cast<long>(vp.srcOriginX.load(std::memory_order_relaxed)) : 0L;
        srcRect.top  = live ? static_cast<long>(vp.srcOriginY.load(std::memory_order_relaxed)) : 0L;

        // The snapshot can be one tick behind a resolution change, and a source
        // rect outside the texture is a device removal, not a glitch.
        srcRect.left = std::clamp(srcRect.left, 0L, (std::max)(0L, monW - srcW));
        srcRect.top  = std::clamp(srcRect.top,  0L, (std::max)(0L, monH - srcH));

        srcRect.right  = srcRect.left + srcW;
        srcRect.bottom = srcRect.top  + srcH;

        // ── The cursor sprite's state — BEFORE the skip test ──
        //
        // Decided before the skip below, because under edge push the source
        // rect deliberately holds still: the sprite's position and shape are
        // then the only sign that anything on screen moved, and without them
        // the magnified pointer froze while the real one kept going.
        const size_t rectSlot = (monitorIndex < StatusSnapshot::kMaxMonitors)
                              ? monitorIndex : StatusSnapshot::kMaxMonitors - 1;

        CursorCache::Shape shape;
        CursorCache::State cursorState = CursorCache::State::Hidden;
        POINT spritePos{ 0, 0 };

        // Shape identity, so an arrow turning into an I-beam without moving
        // still redraws. The SRV pointer is stable per cached shape, which
        // makes it a usable identity without keeping the HCURSOR around.
        const void* spriteShape = nullptr;

        if (m_pointerCompositing)
        {
            cursorState = m_cursorCache.Current(shape);

            // Fail closed. The real pointer is hidden right now, so a sprite we
            // cannot draw means the user has no pointer at all and no way to
            // click their way out of it. A single miss is not worth reacting to
            // — a shape can fail to decode once — but a run of them is.
            if (cursorState == CursorCache::State::Failed)
            {
                if (++m_spriteFailures >= kSpriteFailureLimit)
                {
                    LOG_ERROR("Cursor sprite failed {} frames running — restoring the "
                              "real pointer and disabling pointer compositing",
                              m_spriteFailures);
                    SystemCursor::Restore();
                    m_inputThread.Pointer().SetEnabled(false);
                    m_pointerCompositing = false;
                    m_pointerCompositingBroken = true;
                }
            }
            else
            {
                m_spriteFailures = 0;
            }

            if (cursorState == CursorCache::State::Ok)
            {
                const double vx = m_viewportSnapshot.pointerX.load(std::memory_order_relaxed);
                const double vy = m_viewportSnapshot.pointerY.load(std::memory_order_relaxed);

                // Rounded to whole target pixels: sub-pixel motion that cannot
                // change a single pixel is not worth a frame.
                spritePos.x = std::lround((vx - mon->bounds.left - srcRect.left) * zoom);
                spritePos.y = std::lround((vy - mon->bounds.top  - srcRect.top ) * zoom);
                spriteShape = shape.srv;
            }
        }

        // ── The on-screen readout — also before the skip test ──
        //
        // Same reasoning as the sprite. The readout appears and expires without
        // anything else on screen having to move, so if it is not part of "did
        // anything change" it either never gets drawn or, worse, gets drawn and
        // then stays up forever on a still screen.
        OsdCache::Label osdLabel;
        const void* osdShape  = nullptr;
        float       osdAlpha  = 0.0f;

        {
            const OsdState& osd = m_osd[rectSlot];
            const auto      now = std::chrono::steady_clock::now();

            if (!osd.text.empty() && (now < osd.until || osd.persistent))
            {
                // Sized from the monitor rather than fixed, so it stays the
                // same apparent size on a 4K display as on a 1080p one. Not
                // scaled by zoom: this is our own UI drawn on top of the
                // magnified content, not part of it.
                const int fontPx = std::clamp(static_cast<int>(mon->Height() / 38), 14, 44);

                // Smootherstep across the last kOsdFade, down to zero or, for a
                // persistent readout, to kOsdFadedOpacity. Zero slope at both
                // ends, so it neither snaps off nor crawls.
                const float target = osd.persistent ? kOsdFadedOpacity : 0.0f;

                float t = 0.0f;
                if (now < osd.until)
                {
                    const auto left = osd.until - now;
                    t = (left >= kOsdFade)
                      ? 1.0f
                      : std::chrono::duration<float>(left).count()
                          / std::chrono::duration<float>(kOsdFade).count();
                }
                t = std::clamp(t, 0.0f, 1.0f);

                const float ease = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
                osdAlpha = target + (1.0f - target) * ease;

                // Applied by the sprite shader rather than baked into the
                // bitmap: an animated fade through the cache would upload a
                // texture per step.
                if (osdAlpha > 0.004f && m_osdCache.Acquire(osd.text, fontPx, 1.0f, osdLabel))
                    osdShape = osdLabel.srv;
                else
                    osdAlpha = 0.0f;
            }
        }

        // Quantised so a fading readout redraws while it fades, without asking
        // for a frame per imperceptible step.
        const int osdAlphaStep = static_cast<int>(osdAlpha * 64.0f);

        // -- Did the change land inside what this monitor is magnifying? --
        //
        // Desktop Duplication reports which regions it repainted. A magnifier
        // showing one corner of the screen has no reason to redraw because a
        // video is playing in another, and at 2x or more the magnified region
        // is a small fraction of the screen, so this is the common case rather
        // than the lucky one.
        //
        // The rects are in the TEXTURE's coordinates, which on a rotated output
        // is not desktop space - hence the transform. Getting that wrong would
        // not look like a rotation bug; it would look like a portrait monitor
        // that quietly stops repainting.
        bool contentChanged = haveNewImage;

        if (haveNewImage && capture.DirtyKnown())
        {
            D3D11_TEXTURE2D_DESC texDesc{};
            frame.texture->GetDesc(&texDesc);

            const RECT texRect = DesktopRectToTextureRect(
                capture.GetRotation(), texDesc.Width, texDesc.Height, srcRect);

            contentChanged = capture.DirtyIntersects(texRect);
        }

        // Nothing new: no changed pixels under the view, no source movement,
        // no cursor movement. Presenting anyway would only burn GPU time.
        const RECT& lastRect = m_lastSrcRect[rectSlot];

        const bool rectSame = (lastRect.left   == srcRect.left)
                           && (lastRect.top    == srcRect.top)
                           && (lastRect.right  == srcRect.right)
                           && (lastRect.bottom == srcRect.bottom);

        const bool spriteSame = (m_lastSpritePos[rectSlot].x == spritePos.x)
                             && (m_lastSpritePos[rectSlot].y == spritePos.y)
                             && (m_lastSpriteShape[rectSlot] == spriteShape);

        const bool osdSame = (m_lastOsdShape[rectSlot] == osdShape)
                          && (m_lastOsdAlpha[rectSlot] == osdAlphaStep);

        // Held, not released: see the end of this function.
        if (!contentChanged && rectSame && spriteSame && osdSame)
            return;

        m_lastSrcRect[rectSlot]     = srcRect;
        m_lastSpritePos[rectSlot]   = spritePos;
        m_lastSpriteShape[rectSlot] = spriteShape;
        m_lastOsdShape[rectSlot]    = osdShape;
        m_lastOsdAlpha[rectSlot]    = osdAlphaStep;

        // nullptr because the copy already happened above, unconditionally.
        // Passing the texture again here would repeat it.
        //
        // srcRect is in desktop coordinates; on a rotated output the texture is
        // not, so the renderer needs the rotation to sample it upright. Read
        // from the capture rather than from MonitorInfo: it is the object that
        // produced the texture, and it re-reads the orientation on recovery.
        if (!m_renderer.RenderFrame(nullptr, monitorIndex, srcRect, capture.GetRotation()))
        {
            // No frame may have arrived yet; the next tick tries again.
            return;
        }

        // ── Draw our own pointer over the content ──
        //
        // Shape and position were resolved above, before the skip test, because
        // they are part of deciding whether this frame is worth drawing at all.
        //
        // spritePos is where the user should SEE the pointer,
        // (V - srcOrigin) * zoom. The real OS cursor sits at round(V), the
        // source pixel underneath that sprite — and that identity is the whole
        // reason clicks land on what the pointer appears to point at.
        if (cursorState == CursorCache::State::Ok)
        {
            // Content scale times the user's preference. Above 1 draws a
            // pointer larger than the content it sits on, which is what low
            // vision generally wants and is the entire reason the setting
            // exists — it was offered in the panel and quietly ignored here for
            // long enough to be worth naming.
            //
            // Only the sprite's extent scales. spritePos is where the hotspot
            // must land, and that is fixed by the OS cursor position, so a
            // bigger sprite grows around the point rather than moving it.
            const float scale = static_cast<float>(zoom) * m_cursorScale;

            // The hotspot is what the user points with, so it — not the
            // sprite's corner — is what lands on the computed position.
            m_renderer.RenderSprite(monitorIndex, shape.srv,
                static_cast<float>(spritePos.x) - shape.hotspotX * scale,
                static_cast<float>(spritePos.y) - shape.hotspotY * scale,
                static_cast<float>(shape.width)  * scale,
                static_cast<float>(shape.height) * scale);
        }

        // ── The readout goes last, so it is on top of everything ──
        //
        // Bottom centre: out of the way of the pointer, which spends its time
        // wherever the user is working, and away from the top edge where menus
        // and title bars live. Drawn at 1:1 — this is our own UI over the
        // magnified content, not content to be magnified.
        if (osdShape)
        {
            const float x = (static_cast<float>(mon->Width()) -
                             static_cast<float>(osdLabel.width)) * 0.5f;
            const float y = static_cast<float>(mon->Height())
                          - static_cast<float>(osdLabel.height)
                          - static_cast<float>(mon->Height()) * 0.08f;

            m_renderer.RenderSprite(monitorIndex, osdLabel.srv, x, y,
                static_cast<float>(osdLabel.width),
                static_cast<float>(osdLabel.height),
                osdAlpha);
        }

        // vSync only in flip mode. On a layered window, a vblank wait on top of
        // the DWM surface update blocked the render thread for hundreds of
        // milliseconds, WM_HOTKEY went unprocessed and the application appeared
        // to hang. Tearing is possible without it; a frozen application is
        // worse. FramePacer is the brake instead.
        m_renderer.Present(monitorIndex, UseFlipOverlay());
        m_presentedThisTick = true;

        // Measured after Present, because the vSync wait is part of the frame.
        // steady_clock rather than system_clock: it is monotonic, so the
        // reading survives the wall clock being adjusted underneath it.
        const auto now = std::chrono::steady_clock::now();
        const size_t slot = (monitorIndex < StatusSnapshot::kMaxMonitors)
                          ? monitorIndex : StatusSnapshot::kMaxMonitors - 1;
        auto& lastTime = m_lastFrameTime[slot];

        if (lastTime.time_since_epoch().count() != 0)
        {
            const float dt = std::chrono::duration<float>(now - lastTime).count();
            if (dt > 0.0f)
            {
                // Exponentially smoothed: raw 1/dt jumps far too much to read.
                const float instant = 1.0f / dt;
                auto& fpsSlot = m_status.Monitor(monitorIndex).fps;
                const float prev = fpsSlot.load(std::memory_order_relaxed);
                const float smoothed = (prev <= 0.0f) ? instant
                                                      : (prev * 0.9f + instant * 0.1f);
                fpsSlot.store(smoothed, std::memory_order_relaxed);
            }
        }
        lastTime = now;
    }

    // The frame is NOT released here. It is held until the next AcquireFrame,
    // which releases it first — the order ReleaseFrame's documentation
    // recommends. While we own the frame the OS accumulates desktop updates
    // instead of copying each one into the duplication surface, so a game
    // presenting faster than we acquire stops paying for copies nobody reads.
    // Suspend, recovery and teardown all release it on their own way out.
}

// =============================================================================
// AssertOverlaysTopmost — stay above menus and popups
// =============================================================================
// Two callers:
//   1. WM_APP_ASSERT_TOPMOST, when the input thread sees a popup or menu
//      appear. This is the real path — a dropdown opens and is used inside a
//      second, far faster than any poll would catch.
//   2. Update, every tick, as a backstop for whatever slips past.
//
// Rate limited because EVENT_OBJECT_SHOW fires constantly. 40 ms is under two
// frames, so it is invisible to the user, but it stops a SetWindowPos storm
// and the z-order churn that comes with it.
// =============================================================================
void App::AssertOverlaysTopmost(bool onlyIfCovered)
{
    // BM_NO_TOPMOST_FIGHT=1 disables this: popups stay live but unmagnified
    // and doubled. See FightPopupZOrder in pch.h.
    if (!FightPopupZOrder())
        return;

    const auto now = std::chrono::steady_clock::now();

    if (m_lastTopmostAssert.time_since_epoch().count() != 0
        && now - m_lastTopmostAssert < std::chrono::milliseconds(40))
    {
        return;
    }

    // The coverage walk has a clock of its own. Stamping m_lastTopmostAssert
    // for a check that asserted nothing would hold back a popup event arriving
    // just after it; not stamping anything would run the walk every tick.
    if (onlyIfCovered)
    {
        if (m_lastCoverCheck.time_since_epoch().count() != 0
            && now - m_lastCoverCheck < std::chrono::milliseconds(40))
        {
            return;
        }
        m_lastCoverCheck = now;
    }

    bool any = false;
    for (auto& overlay : m_overlays)
    {
        if (overlay.IsVisible() && (!onlyIfCovered || overlay.IsCovered()))
        {
            overlay.EnsureTopmost();
            any = true;
        }
    }

    // Only stamp the time when something was actually asserted, so the rate
    // limit is not already spent on the first popup after zoom comes on.
    if (any)
        m_lastTopmostAssert = now;
}

// =============================================================================
// PublishMonitorInfo — the static monitor fields, for the panel's card headers
// =============================================================================
// Init and WM_DISPLAYCHANGE only. None of this changes per frame.
// =============================================================================
void App::PublishMonitorInfo()
{
    const size_t count = m_monitorManager.GetMonitorCount();
    m_status.monitorCount.store(count, std::memory_order_release);

    for (size_t i = 0; i < count && i < StatusSnapshot::kMaxMonitors; ++i)
    {
        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        auto& st = m_status.Monitor(i);

        // wcsncpy_s + _TRUNCATE: always null-terminated, never overruns
        wcsncpy_s(st.deviceName, MonitorStatus::kNameCapacity,
                  mon->deviceName.c_str(), _TRUNCATE);

        st.width.store(mon->Width(), std::memory_order_relaxed);
        st.height.store(mon->Height(), std::memory_order_relaxed);
        st.refreshRate.store(static_cast<int>(mon->refreshRate), std::memory_order_relaxed);
        st.dpiPercent.store(static_cast<int>(mon->ScaleFactor() * 100.0f),
                            std::memory_order_relaxed);
        st.isPrimary.store(mon->isPrimary, std::memory_order_relaxed);
    }
}

// =============================================================================
// Event Handlers
// =============================================================================

// Hotkey, tray double click or tray menu: toggles the monitor the pointer is on.
// Windows Magnifier magnifies every display together; independent per-monitor
// zoom is the whole difference.
void App::OnToggleZoom()
{
    size_t index = 0;
    if (!ResolveMonitorIndex(kFocusedMonitor, index))
    {
        LOG_WARN("Cursor is on no known monitor, toggle skipped");
        return;
    }

    ToggleZoomOnMonitor(index);
}

// =============================================================================
// RememberZoom — rememberZoomLevel, minus the logon case
// =============================================================================
// Every read of the setting goes through here, so a session started by the
// logon task neither restores a remembered level nor writes one back.
// =============================================================================
bool App::RememberZoom() const
{
    return m_settings.General().rememberZoomLevel && !LaunchedAtStartup();
}

void App::ToggleZoomOnMonitor(size_t i)
{
    const MonitorInfo* before = m_monitorManager.GetMonitor(i);
    if (!before)
        return;

    // Read the level in use BEFORE toggling: turning zoom off resets zoomLevel
    // to kMinZoom, so reading afterwards would see 1.0.
    const float levelInUse = before->zoom.zoomLevel;
    const bool  wasActive  = before->zoom.isActive;

    m_monitorManager.ToggleZoom(i);

    MonitorInfo* mon = m_monitorManager.GetMonitor(i);
    if (!mon)
        return;

    // This is the explicit enable/disable action (hotkey or panel switch), as
    // opposed to Win+Plus/Win+Minus stepping zoom on or down to off. Only this
    // path sets or clears userDisabled, so a stepped-off monitor still comes
    // back with Win+Plus while a disabled one does not.
    mon->zoom.userDisabled = !mon->zoom.isActive;

    const auto ms = m_settings.Monitor(mon->deviceName);

    if (mon->zoom.isActive)
    {
        // Which level does zoom come on at? The last one used when
        // rememberZoomLevel is set, otherwise twice the minimum.
        float startZoom = RememberZoom()
            ? std::clamp(ms.lastZoom, ms.minZoom, ms.maxZoom)
            : std::clamp(ms.minZoom * 2.0f, ms.minZoom, ms.maxZoom);

        // Turning zoom on at 1.0x reads as "zoom is broken". A stale
        // settings.ini with LastZoom=1 used to produce exactly that, so the
        // floor is enforced here rather than expecting anyone to delete a file.
        if (startZoom <= ms.minZoom)
            startZoom = std::clamp(ms.minZoom * 2.0f, ms.minZoom, ms.maxZoom);

        m_monitorManager.SetZoom(i, startZoom);
    }
    else
    {
        // Save the level in use now, not at shutdown. By then zoomLevel has
        // already been reset to 1.0 and LastZoom=1 is what reaches the disk.
        if (wasActive
            && RememberZoom()
            && levelInUse > ms.minZoom)
        {
            auto updated = ms;
            updated.lastZoom = std::clamp(levelInUse, ms.minZoom, ms.maxZoom);
            m_settings.SetMonitor(mon->deviceName, updated);
        }
    }

    // The tray is not told here. Zoom is mutated from eight places and the
    // notification was already missing from most of them; Update publishes the
    // settled state every frame instead, for the same reason
    // PublishViewportRequests does.
}

void App::OnFreeze()
{
    POINT cursor{};
    GetCursorPos(&cursor);

    const MonitorInfo* target = m_monitorManager.FindByPoint(cursor);
    if (!target)
        return;

    const auto& monitors = m_monitorManager.GetMonitors();
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        if (monitors[i].hMonitor == target->hMonitor)
        {
            m_monitorManager.ToggleFreezeOnMonitor(i);
            return;
        }
    }
}

// =============================================================================
// OnZoomStep — change zoom by one increment
// =============================================================================
// Sources: Ctrl+Alt+wheel, Win+Plus, Win+Minus.
//
// Mirrors Windows Magnifier:
//   off + up    -> turn on, at the starting level
//   on  + up    -> one step in
//   on  + down  -> one step out, and OFF once it reaches minZoom
//   off + down  -> nothing; there is nothing below off
//
// Stepping up may turn zoom on because Ctrl+Alt+wheel is swallowed: the
// combination is ours, and nothing else sees it.
// =============================================================================
void App::OnZoomStep(int direction)
{
    POINT cursor{};
    GetCursorPos(&cursor);

    const MonitorInfo* target = m_monitorManager.FindByPoint(cursor);
    if (!target)
        return;

    const auto& monitors = m_monitorManager.GetMonitors();
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        if (monitors[i].hMonitor != target->hMonitor)
            continue;

        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            return;

        const auto ms = m_settings.Monitor(mon->deviceName);

        // ── Asking to zoom in while off means "turn on" ──
        // ...unless the user explicitly disabled this monitor (Ctrl+Alt+Z or
        // the panel switch); that toggle is the only way back on. Raise the
        // readout anyway, or the keypress reads as having done nothing.
        if (!mon->zoom.isActive)
        {
            if (direction <= 0)
                return;   // nothing below off

            if (mon->zoom.userDisabled)
            {
                const size_t slot = (i < StatusSnapshot::kMaxMonitors)
                                  ? i : StatusSnapshot::kMaxMonitors - 1;
                m_osd[slot].text  = L"Disabled";
                m_osd[slot].until = std::chrono::steady_clock::now() + kOsdDuration;
                return;
            }

            m_monitorManager.ToggleZoom(i);

            const float startZoom = RememberZoom()
                ? std::clamp(ms.lastZoom, ms.minZoom, ms.maxZoom)
                : std::clamp(ms.minZoom + ms.zoomStep, ms.minZoom, ms.maxZoom);

            m_monitorManager.SetZoom(i, startZoom);

            LOG_INFO("Monitor {} zoom on ({:.2f}x) — Win+Plus / Ctrl+Alt+wheel", i, startZoom);
            return;
        }

        // ── Already on: step ──
        const float step = (direction > 0) ? ms.zoomStep : -ms.zoomStep;
        m_monitorManager.AdjustZoom(i, step);

        // ── Reaching minZoom turns it off ──
        // Windows Magnifier's Win+Minus behaviour. Note that a configured
        // minZoom above 1.0 (say 1.5) closes at that level, which can look odd
        const MonitorInfo* after = m_monitorManager.GetMonitor(i);
        if (after && direction < 0 && after->zoom.zoomLevel <= ms.minZoom)
        {
            // Store the level in use BEFORE closing: ToggleZoom resets
            // zoomLevel to minZoom.
            if (RememberZoom())
            {
                auto updated = ms;
                updated.lastZoom = std::clamp(ms.minZoom + ms.zoomStep,
                                              ms.minZoom, ms.maxZoom);
                m_settings.SetMonitor(mon->deviceName, updated);
            }

            m_monitorManager.ToggleZoom(i);
            LOG_INFO("Monitor {} zoom off (stepped down to minZoom)", i);
        }

        return;
    }
}

// =============================================================================
// OnFocusChanged — keyboard focus moved, follow it
// =============================================================================
// Targets the centre of the focused window. The caret position would be more
// precise, but reading it needs UI Automation and works inconsistently from
// one application to the next; out of scope.
//
// Left alone while frozen: the user pinned the view on purpose.
// =============================================================================
void App::OnFocusChanged(HWND focused)
{
    if (!focused)
        return;

    if (m_settings.General().followMode != FollowMode::MouseAndFocus)
        return;

    RECT rc{};
    if (!GetWindowRect(focused, &rc))
        return;

    if (rc.right <= rc.left || rc.bottom <= rc.top)
        return;

    const POINT center{ (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };

    // The index, not just the MonitorInfo: the request travels to the input
    // thread, which knows monitors by index and nothing else.
    size_t index = 0;
    bool   found = false;
    const auto& monitors = m_monitorManager.GetMonitors();
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        if (PtInRect(&monitors[i].bounds, center))
        {
            index = i;
            found = true;
            break;
        }
    }

    if (!found)
        return;

    // Only when that monitor is actually magnified and not frozen.
    if (!monitors[index].zoom.isActive || monitors[index].zoom.isFrozen)
        return;

    // Publish, do not apply. The source rect belongs to ViewportController,
    // which lives on the input thread; writing it from here would be the shared
    // mutable state this design exists to avoid.
    //
    // Coordinates before the epoch, and the epoch with release ordering, so an
    // input thread that sees the new request also sees the position it refers
    // to. Reversed, it would centre on the PREVIOUS focus.
    m_viewportSnapshot.focusMonitor.store(index, std::memory_order_relaxed);
    m_viewportSnapshot.focusX.store(static_cast<double>(center.x), std::memory_order_relaxed);
    m_viewportSnapshot.focusY.store(static_cast<double>(center.y), std::memory_order_relaxed);
    m_viewportSnapshot.focusEpoch.fetch_add(1, std::memory_order_release);

    // The view moves and the pointer does not. Moving the pointer to follow
    // focus was tried and reverted: it is a feedback loop through hover and
    // focus (docs/ARCHITECTURE.md, "Lessons learned"). The cost is that while
    // focus is driving, the sprite can be off screen until the next mouse move
    // re-anchors the view; the panel's hint says so.
}

// =============================================================================
// ResolveMonitorIndex — turn a wParam into a monitor index
// =============================================================================
// The kFocusedMonitor sentinel means "whichever monitor holds the cursor".
// Anything else is an index; out of range returns false.
// =============================================================================
bool App::ResolveMonitorIndex(WPARAM wparam, size_t& outIndex) const
{
    const auto& monitors = m_monitorManager.GetMonitors();

    if (wparam == kFocusedMonitor)
    {
        POINT cursor{};
        GetCursorPos(&cursor);

        for (size_t i = 0; i < monitors.size(); ++i)
        {
            if (PtInRect(&monitors[i].bounds, cursor))
            {
                outIndex = i;
                return true;
            }
        }
        return false;
    }

    if (wparam < monitors.size())
    {
        outIndex = static_cast<size_t>(wparam);
        return true;
    }

    return false;
}

// =============================================================================
// ApplyPointerSettings — push the pointer and edge-push settings to the input thread
// =============================================================================
//
// Separate from ApplySettings because it has to run at STARTUP too, and
// ApplySettings does not: it is only reachable from WM_APP_SETTINGS_CHANGED.
// That gap was a real bug — everything in settings.ini below was ignored until
// the user happened to change something, so a hand-edited file or a value saved
// last session simply did not take effect. Reported as "the setting does nothing".
//
// Everything here is an atomic the input thread reads on its next event, so it
// applies live: a slider is felt while it is being dragged.
// =============================================================================
void App::ApplyPointerSettings()
{
    const auto& g = m_settings.General();

    // Both mouse modes map onto Anchored. The difference between them is only
    // whether keyboard focus ALSO moves the view; how the pointer moves it is
    // the same in both.
    ViewportConfig cfg;
    cfg.mode = (g.followMode == FollowMode::EdgePush) ? PanMode::EdgePush
                                                      : PanMode::Anchored;
    cfg.bandFraction = g.edgeBandFraction;
    m_inputThread.SetViewportConfig(cfg);

    m_inputThread.Pointer().SetSpeed(g.pointerSpeed);
    m_inputThread.Pointer().SetCompensation(g.pointerCompensation);
    m_inputThread.Pointer().SetLockToMonitor(g.lockPointerToMonitor);

    // Read by the render thread in RenderMonitor. Clamped to the same range the
    // loader uses, so a hand-edited INI cannot produce a sprite that covers the
    // screen or one too small to find.
    const float wantScale = std::clamp(g.cursorScale, 0.5f, 4.0f);

    if (wantScale != m_cursorScale)
    {
        m_cursorScale = wantScale;

        // Force the next frame. The nothing-changed test compares the sprite's
        // position and shape, and resizing changes neither — so dragging the
        // size slider on a still screen would do nothing visible until the
        // mouse happened to move.
        m_lastSpriteShape.fill(nullptr);
    }

    LOG_INFO("Pointer settings applied: speed={:.2f} comp={:.2f} lock={} "
             "scaling={} pan={} band={:.2f}",
             g.pointerSpeed, g.pointerCompensation,
             g.lockPointerToMonitor ? "on" : "off",
             g.pointerScaling ? "on" : "off",
             cfg.mode == PanMode::EdgePush ? "edge-push" : "anchored",
             g.edgeBandFraction);

    // Turning pointer scaling off has to give the real pointer back straight
    // away, not on the next zoom change.
    if (!g.pointerScaling && m_pointerCompositing)
    {
        SystemCursor::Restore();
        m_inputThread.Pointer().SetEnabled(false);
        m_pointerCompositing = false;
    }
}

// =============================================================================
// ApplySettings — settings changed, push them into the engine
// =============================================================================
// The panel writes SettingsStore and only then posts the message, so reading
// here is safe without synchronisation. That ordering is load-bearing; reverse
// it and this reads values that have not been written yet.
// =============================================================================
void App::ApplySettings()
{
    LOG_INFO("Applying settings");

    const auto& g = m_settings.General();

    // Re-register the hotkeys and report the outcome to the panel
    const UINT failedMask = m_hotkeyManager.Reregister(g);
    m_status.hotkeyFailedMask.store(failedMask, std::memory_order_release);

    // Update the input thread's atomic flags
    m_inputThread.SetFollowMode(g.followMode);
    m_inputThread.SetHijackMagnifierKeys(g.hijackMagnifierKeys);

    ApplyPointerSettings();
    ApplyFrameRateCaps();

    // Pull the current zoom back inside the new limits
    for (size_t i = 0; i < m_monitorManager.GetMonitorCount(); ++i)
    {
        MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        const auto ms = m_settings.Monitor(mon->deviceName);

        if (mon->zoom.zoomLevel > ms.maxZoom)
            m_monitorManager.SetZoom(i, ms.maxZoom);
        else if (mon->zoom.zoomLevel < ms.minZoom)
            m_monitorManager.SetZoom(i, ms.minZoom);
    }

    ApplyStartWithWindows(g.startWithWindows);
}

// =============================================================================
// OnHotkeyCaptured — a key combination came back from the input thread
// =============================================================================
//
// Saved and applied immediately rather than staged for an OK button. The panel
// has no OK button — every other setting there applies live — and a binding
// that only takes effect later is a binding the user cannot try.
// =============================================================================
void App::OnHotkeyCaptured(UINT modifiers, UINT packed)
{
    const unsigned which = (packed >> 16) & 0xFFFFu;
    const UINT     vk    = packed & 0xFFFFu;

    if (vk == 0)
    {
        LOG_INFO("Hotkey capture ended without a binding");
        m_controlPanel.NotifyHotkeysChanged();
        return;
    }

    auto& g = m_settings.MutableGeneral();

    // A modifier-less binding is accepted but is a bad idea: RegisterHotKey
    // takes it system-wide, so a bare "Z" would swallow that key everywhere.
    // Refused rather than warned about, because the user cannot undo it from a
    // panel they can no longer type into.
    if (modifiers == 0)
    {
        LOG_WARN("Refusing a hotkey with no modifier (vk={}): it would be claimed "
                 "system-wide and make that key unusable everywhere", vk);
        m_controlPanel.NotifyHotkeysChanged();
        return;
    }

    if (which == kHotkeyFreeze)
    {
        g.freezeModifiers = modifiers;
        g.freezeVk        = vk;
    }
    else
    {
        g.toggleModifiers = modifiers;
        g.toggleVk        = vk;
    }

    m_settings.Save();

    const UINT failedMask = m_hotkeyManager.Reregister(g);
    m_status.hotkeyFailedMask.store(failedMask, std::memory_order_release);

    LOG_INFO("Hotkey {} bound to {}",
             which == kHotkeyFreeze ? "freeze" : "toggle",
             ToUtf8(FormatHotkey(modifiers, vk)));

    // The panel shows the bindings as text, so it has to be told; the live
    // timer only refreshes the monitor cards.
    m_controlPanel.NotifyHotkeysChanged();
}

// =============================================================================
// Updates
// =============================================================================

void App::MaybeCheckForUpdates()
{
    const GeneralSettings& g = m_settings.General();

    if (!g.checkForUpdates)
    {
        LOG_INFO("Update: checking is off in settings");
        return;
    }

    const long long now = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    constexpr long long kFloorSeconds = 24LL * 60LL * 60LL;

    if (now - g.lastUpdateCheck < kFloorSeconds)
    {
        LOG_INFO("Update: checked within the last 24 hours, skipping");
        return;
    }

    // Stamped before the check, not after: a failed check should still consume
    // the day's allowance, or a machine with no network asks on every start.
    m_settings.MutableGeneral().lastUpdateCheck = now;
    m_settings.Save();

    m_updateChecker.RequestCheck(false);
}

void App::OnUpdateState(UpdateState state)
{
    if (state != UpdateState::Available)
        return;

    ReleaseInfo info;
    if (!m_updateChecker.LatestRelease(info))
        return;

    // "Not now" should mean not now, not "ask me every launch".
    const std::wstring& skipped = m_settings.General().skippedVersion;
    if (!skipped.empty() && CompareVersion(info.version, skipped) <= 0)
    {
        LOG_INFO("Update: {} was skipped by the user, staying quiet",
                 ToUtf8(info.version));
        return;
    }

    // One balloon per version per session.
    if (m_announcedVersion == info.version)
        return;

    m_announcedVersion = info.version;
    m_trayIcon.ShowUpdateBalloon(info.version);
}

void App::OnUpdateAction(WPARAM action)
{
    switch (action)
    {
    case kUpdateCheckNow:
        m_updateChecker.RequestCheck(true);
        break;

    case kUpdateInstall:
        m_updateChecker.RequestInstall();
        break;

    case kUpdateSkip:
    {
        ReleaseInfo info;
        if (m_updateChecker.LatestRelease(info))
        {
            m_settings.MutableGeneral().skippedVersion = info.version;
            m_settings.Save();
            LOG_INFO("Update: {} skipped by the user", ToUtf8(info.version));
        }
        break;
    }

    default:
        LOG_WARN("Update: unknown action {}", static_cast<unsigned>(action));
        break;
    }
}

// =============================================================================
// OnShowPanel — open the control panel
// =============================================================================
// The panel lives on its own STA thread; the first call creates it. Without the
// Windows App Runtime the panel does not open and the magnifier is unaffected.
// See ControlPanel.h.
// =============================================================================
void App::OnShowPanel()
{
    m_controlPanel.Show(m_messageHwnd, &m_settings, &m_status);
}

// =============================================================================
// OnDisplayChange — a monitor was added, removed or resized
// =============================================================================
//
// MonitorManager::Refresh rebuilds the list from scratch, which invalidates
// every dxgiOutput pointer the old MonitorInfo held. The whole
// capture/overlay/swap-chain chain therefore has to be torn down and rebuilt,
// not patched.
//
// Zoom state survives: MonitorManager carries it across keyed by device name.
// =============================================================================
void App::OnDisplayChange()
{
    LOG_INFO("Display change — rebuilding the pipeline");

    // Teardown order: capture (duplication session), swap chain, overlay window
    m_captures.clear();

    for (size_t i = 0; i < m_overlays.size(); ++i)
        m_renderer.RemoveRenderTarget(i);

    m_overlays.clear();

    m_monitorManager.Refresh();

    // Rebuild — the per-monitor part of InitializeComponents
    const size_t monitorCount = m_monitorManager.GetMonitorCount();
    m_overlays.reserve(monitorCount);
    m_captures.reserve(monitorCount);

    for (size_t i = 0; i < monitorCount; ++i)
    {
        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        OverlayWindow overlay;
        if (!overlay.Create(m_hInstance, *mon, i))
            continue;

        m_renderer.CreateSwapChainForWindow(
            overlay.GetHwnd(),
            static_cast<UINT>(mon->Width()),
            static_cast<UINT>(mon->Height()),
            i);

        DXGICapture capture;
        if (mon->dxgiOutput)
            capture.Initialize(m_renderer.GetDevice(), mon->dxgiOutput.Get());

        m_overlays.push_back(std::move(overlay));
        m_captures.push_back(std::move(capture));
    }

    // Publish the new monitor info so the panel's card headers update
    PublishMonitorInfo();

    // Bump the layout epoch so the input thread re-reads every rect and
    // re-clamps. Without this the controller keeps panning against the old
    // resolution and srcOrigin sticks to a bound that no longer exists.
    PublishViewportRequests(true);

    m_controlPanel.NotifyDisplayChange();

    // Refresh rates move with the monitor list: a display swapped for a faster
    // one, or a mode change, and every pacer is now aiming at the wrong rate.
    ApplyFrameRateCaps();

    LOG_INFO("Pipeline rebuilt ({} monitors)", m_overlays.size());
}

// =============================================================================
// MessageWndProc — window procedure for the hidden message window
// =============================================================================
// Static because a WndProc must be a plain function pointer; the instance is
// reached through s_instance.
// =============================================================================
LRESULT CALLBACK App::MessageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_HOTKEY:
        if (s_instance)
            s_instance->m_hotkeyManager.HandleHotkey(static_cast<int>(wParam));
        return 0;

    case TrayIcon::kTrayCallbackMsg:
        if (s_instance)
            s_instance->m_trayIcon.HandleMessage(wParam, lParam);
        return 0;

    case WM_DISPLAYCHANGE:
        if (s_instance)
            s_instance->OnDisplayChange();
        return 0;

    case WM_APP_ZOOM_STEP:
        if (s_instance)
        {
            // kZoomOut is (WPARAM)-1 and WPARAM is unsigned, so the round trip
            // goes through intptr_t to read the sign back.
            const int dir = (static_cast<intptr_t>(wParam) > 0) ? +1 : -1;
            s_instance->OnZoomStep(dir);
        }
        return 0;

    case WM_APP_FOCUS_CHANGED:
        if (s_instance)
            s_instance->OnFocusChanged(reinterpret_cast<HWND>(lParam));
        return 0;

    case WM_APP_SETTINGS_CHANGED:
        if (s_instance)
            s_instance->ApplySettings();
        return 0;

    case WM_APP_SET_ZOOM:
        if (s_instance)
        {
            size_t index = 0;
            if (s_instance->ResolveMonitorIndex(wParam, index))
            {
                const float zoom = static_cast<float>(static_cast<int>(lParam)) / 1000.0f;
                s_instance->m_monitorManager.SetZoom(index, zoom);
            }
        }
        return 0;

    case WM_APP_TOGGLE_ZOOM:
        if (s_instance)
        {
            size_t index = 0;
            if (s_instance->ResolveMonitorIndex(wParam, index))
                s_instance->ToggleZoomOnMonitor(index);
        }
        return 0;

    case WM_APP_TOGGLE_FREEZE:
        if (s_instance)
        {
            size_t index = 0;
            if (s_instance->ResolveMonitorIndex(wParam, index))
                s_instance->m_monitorManager.ToggleFreezeOnMonitor(index);
        }
        return 0;

    case WM_APP_ASSERT_TOPMOST:
        if (s_instance)
            s_instance->AssertOverlaysTopmost();
        return 0;

    case WM_APP_SHOW_PANEL:
        if (s_instance)
            s_instance->OnShowPanel();
        return 0;

    case WM_APP_UPDATE_STATE:
        if (s_instance)
            s_instance->OnUpdateState(static_cast<UpdateState>(wParam));
        return 0;

    case WM_APP_UPDATE_ACTION:
        if (s_instance)
            s_instance->OnUpdateAction(wParam);
        return 0;

    case WM_APP_CAPTURE_HOTKEY:
        if (s_instance)
            s_instance->m_inputThread.ArmHotkeyCapture(wParam);
        return 0;

    case WM_APP_HOTKEY_CAPTURED:
        if (s_instance)
            s_instance->OnHotkeyCaptured(static_cast<UINT>(wParam),
                                         static_cast<UINT>(lParam));
        return 0;

    case WM_WTSSESSION_CHANGE:
        if (s_instance && wParam == WTS_SESSION_UNLOCK)
            s_instance->OnSessionUnlock();

        // Clear the hidden-pointer state on lock. Not politeness: the secure
        // desktop has its own pointer, and if the switch also drops our hide
        // while our flag still says "hidden", Hide() would early-out on
        // return and never re-apply — leaving the real pointer visible
        // underneath our sprite. Restoring here forces the next frame to
        // re-apply it.
        if (wParam == WTS_SESSION_LOCK)
            SystemCursor::Restore();
        return 0;

    // The process is about to end. With the pointer hidden this is the last
    // chance to put it back.
    case WM_QUERYENDSESSION:
    case WM_ENDSESSION:
        SystemCursor::Restore();
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// =============================================================================
// Shutdown — tear down in dependency order
// =============================================================================
// Forced by hand rather than left to destructors: member declaration order is
// not the order these have to die in.
// =============================================================================
void App::Shutdown()
{
    if (!m_initialized)
        return;

    LOG_INFO("App shutting down");

    m_running = false;

    // Before anything else can fail: give the pointer back. Every other
    // teardown step is recoverable by restarting the app; a hidden pointer is
    // not, because the user cannot click anything to fix it.
    UpdatePointerCompositing(false, false);
    m_cursorCache.Clear();
    m_osdCache.Clear();

    // Update thread first: it posts to the message window and writes m_status,
    // both of which are about to go.
    m_updateChecker.Stop();

    // GUI thread next: the panel holds pointers to m_settings and m_status and
    // has to be gone before those become invalid.
    m_controlPanel.Stop();

    // 1. Input thread next: destroying the message window while the hooks are
    //    still up means an in-flight PostMessage aimed at a dead HWND.
    m_inputThread.Stop();

    // 1a. Drop the hotkey registrations
    m_hotkeyManager.Shutdown();

    // 2. Remove the tray icon
    m_trayIcon.Destroy();

    // 3. Close the duplication sessions — before the device
    m_captures.clear();

    // 4. Release the swap chains, THEN destroy the windows. The other order
    //    leaves a swap chain holding a reference to a destroyed HWND.
    for (size_t i = 0; i < m_overlays.size(); ++i)
        m_renderer.RemoveRenderTarget(i);

    // 5. Destroy the overlay windows
    m_overlays.clear();

    // 6. Message window
    if (m_messageHwnd)
    {
        WTSUnRegisterSessionNotification(m_messageHwnd);
        DestroyWindow(m_messageHwnd);
        m_messageHwnd = nullptr;
    }
    UnregisterClassW(kMsgWindowClass, m_hInstance);

    // 7. Save the settings, last zoom levels included
    if (RememberZoom())
    {
        for (size_t i = 0; i < m_monitorManager.GetMonitorCount(); ++i)
        {
            const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
            if (!mon)
                continue;

            // Only monitors that are currently zoomed.
            //
            // Writing unconditionally stored LastZoom=1 for every idle monitor,
            // because an inactive monitor's zoomLevel is 1.0. The next session
            // then "turned on" at 1.0x and magnified nothing. The level in use
            // at the moment of switching off is already saved in OnToggleZoom.
            if (!mon->zoom.isActive)
                continue;

            auto ms = m_settings.Monitor(mon->deviceName);
            if (mon->zoom.zoomLevel <= ms.minZoom)
                continue;

            ms.lastZoom = std::clamp(mon->zoom.zoomLevel, ms.minZoom, ms.maxZoom);
            m_settings.SetMonitor(mon->deviceName, ms);
        }
    }
    m_settings.Save();

    // 8. m_renderer is a member, so its destructor releases the device last

    s_instance    = nullptr;
    m_initialized = false;

    LOG_INFO("App shut down");
}

} // namespace BetterMagnifier
