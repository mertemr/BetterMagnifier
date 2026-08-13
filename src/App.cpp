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
#include "Logger.h"

#include <wtsapi32.h>   // WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK

namespace BetterMagnifier {

App* App::s_instance = nullptr;

namespace {

// The control panel is OFF unless BM_PANEL=1.
//
// It is not finished: a XAML TextBox in the island takes the whole process down
// with a stowed exception, and the tree is only partly verified without one.
// The magnifier itself does not need it, so it stays behind a switch instead of
// shipping a window that can kill the app. Details: docs/PANEL-BLANK.md.
//
// With the switch on, the panel also opens at startup. The app requires
// administrator rights, so a script in a normal shell cannot click the tray or
// post it a message - UIPI drops both - and this is the only way to reach it
// from a test.
bool PanelEnabled()
{
    static const bool enabled = []() {
        wchar_t buf[8]{};
        const DWORD n = GetEnvironmentVariableW(L"BM_PANEL", buf, 8);
        return (n > 0 && n < 8 && buf[0] == L'1');
    }();
    return enabled;
}

// =============================================================================
// ApplyStartWithWindows — the HKCU Run entry
// =============================================================================
// HKCU rather than HKLM: HKLM needs administrator rights and writes for every
// user on the machine. The entry goes stale if the exe moves, but it is
// rewritten on every settings change.
//
// Caveat worth knowing: if the binary requires elevation, Windows may silently
// skip a Run entry at logon. The reliable route in that case is a scheduled
// task with highest privileges, which is not built. The panel says so.
// =============================================================================
bool ApplyStartWithWindows(bool enable)
{
    constexpr wchar_t kRunKey[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kValueName[] = L"BetterMagnifier";

    HKEY key = nullptr;
    LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (st != ERROR_SUCCESS)
    {
        LOG_ERROR("Run anahtari acilamadi: {}", st);
        return false;
    }

    bool ok = false;

    if (enable)
    {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        {
            LOG_ERROR("GetModuleFileNameW basarisiz: {}", GetLastError());
            RegCloseKey(key);
            return false;
        }

        // Yol bosluk icerebilir; tirnaklamazsak Windows ilk bosluktan keser.
        const std::wstring quoted = L"\"" + std::wstring(exePath) + L"\"";

        st = RegSetValueExW(key, kValueName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(quoted.c_str()),
                static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
        ok = (st == ERROR_SUCCESS);

        if (!ok)
            LOG_ERROR("Run degeri yazilamadi: {}", st);
    }
    else
    {
        st = RegDeleteValueW(key, kValueName);
        // Already absent counts as success: the requested end state holds.
        ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);

        if (!ok)
            LOG_ERROR("Run degeri silinemedi: {}", st);
    }

    RegCloseKey(key);
    return ok;
}

// =============================================================================
// OsMagnifierRunning — Windows Magnifier ayakta mi?
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

// =============================================================================
// Destructor
// =============================================================================
App::~App()
{
    Shutdown();
}

// =============================================================================
// Initialize — Tum component'leri sirayla baslat
// =============================================================================
bool App::Initialize(HINSTANCE hInstance)
{
    if (m_initialized)
        return true;

    m_hInstance = hInstance;
    s_instance  = this;

    LOG_INFO("App baslatiliyor...");

    if (!CreateMessageWindow())
        return false;

    if (!InitializeComponents())
        return false;

    SetupCallbacks();

    m_initialized = true;
    LOG_INFO("App hazir — Ctrl+Alt+Z ile zoom'u ac, Ctrl+Alt+X freeze");
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

    if (!RegisterClassExW(&wc))
    {
        LOG_ERROR("Mesaj penceresi class kaydi basarisiz: {}", GetLastError());
        return false;
    }

    m_messageHwnd = CreateWindowExW(
        0,
        kMsgWindowClass,
        L"BetterMagnifier",
        WS_POPUP,               // Kenarlik/title bar yok
        0, 0, 0, 0,             // 0x0 boyut — hic gosterilmeyecek
        nullptr, nullptr,
        m_hInstance,
        nullptr);

    if (!m_messageHwnd)
    {
        LOG_ERROR("Mesaj penceresi olusturulamadi: {}", GetLastError());
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

// =============================================================================
// InitializeComponents
// =============================================================================
bool App::InitializeComponents()
{
    // Settings first; everything below can depend on them. A missing file is
    // not an error, it is a first run.
    m_settings.Load();

    // Warn rather than silently override: the file may predate the change.
    if (m_settings.General().followMode == FollowMode::MouseAndFocus)
    {
        LOG_WARN("FollowMode=MouseAndFocus currently has no visible effect — the "
                 "source rect no longer follows focalPoint. See OnFocusChanged.");
    }

    if (!m_monitorManager.Initialize())
    {
        LOG_ERROR("MonitorManager initialisation failed");
        return false;
    }

    // ── 2. GPU device ──
    if (!m_renderer.Initialize())
    {
        LOG_ERROR("D3DRenderer baslatilamadi");
        return false;
    }

    m_cursorCache.Initialize(m_renderer.GetDevice());

    // ── 3. Per-monitor: overlay + swap chain + capture ──
    const size_t monitorCount = m_monitorManager.GetMonitorCount();

    // reserve ONEMLI: vector buyurken move ediyor, HWND/COM pointer'lar tasiniyor.
    // reserve olmadan realloc sirasinda gereksiz move + destroy zinciri olusur.
    m_overlays.reserve(monitorCount);
    m_captures.reserve(monitorCount);

    for (size_t i = 0; i < monitorCount; ++i)
    {
        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        // Overlay pencere
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

    if (PanelEnabled())
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

    // No callback, no menu entry: a Settings item that cannot open anything is
    // worse than no item.
    if (PanelEnabled())
        m_trayIcon.SetSettingsCallback([this] { OnShowPanel(); });
}

// =============================================================================
// Run — hybrid message and render loop
// =============================================================================
//
// PeekMessage rather than GetMessage: GetMessage blocks until a message
// arrives, which would stop the render loop dead. Peek returns immediately
// when the queue is empty and the frame gets rendered in that gap.
//
// Pacing comes from Present with vSync while zoom is active, which locks the
// loop to the refresh rate for free. With zoom off nothing is presented, so
// there is no such brake and the loop sleeps instead of spinning.
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

    LOG_INFO("Message loop bitti (exit code: {})", static_cast<int>(msg.wParam));
    return static_cast<int>(msg.wParam);
}

// =============================================================================
// Update — Her frame'de bir kez
// =============================================================================
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
        a.requestedZoom.store(zoom, std::memory_order_relaxed);

        a.originX.store(mon->bounds.left, std::memory_order_relaxed);
        a.originY.store(mon->bounds.top,  std::memory_order_relaxed);
        a.width.store(mon->Width(),  std::memory_order_relaxed);
        a.height.store(mon->Height(), std::memory_order_relaxed);
        a.frozen.store(mon->zoom.isFrozen, std::memory_order_relaxed);
    }

    // Release-ordered last, so the input thread that sees the new epoch also
    // sees every rect written above it.
    if (bumpLayout)
        m_viewportSnapshot.layoutEpoch.fetch_add(1, std::memory_order_release);
}

// =============================================================================
// UpdatePointerCompositing — gercek imleci gizle / geri getir
// =============================================================================
//
// The exposure window is exactly "actively magnifying": the pointer is hidden
// when a monitor is zoomed and comes back the instant it is not.
//
// Gated on MagPathAvailable, and that gate is load-bearing rather than
// defensive. Hiding the pointer without MagShowSystemCursor means
// SetSystemCursor, whose effect outlives the process; if we drew a sprite
// without hiding, the user would see two pointers in different places, which
// is worse than one in the wrong place. So when the safe hide is unavailable
// the whole feature stays off and the pointer behaves natively.
// =============================================================================
void App::UpdatePointerCompositing(bool anyMonitorZoomed)
{
    const bool want = anyMonitorZoomed
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

void App::Update()
{
    m_status.monitorCount.store(m_overlays.size(), std::memory_order_relaxed);
    m_presentedThisTick = false;

    PublishViewportRequests(false);

    // ── Mouse pozisyonunu takip et (magnifier fareyi izler) ──
    POINT cursor{};
    GetCursorPos(&cursor);

    bool anyActive = false;
    const size_t count = m_overlays.size();

    for (size_t i = 0; i < count; ++i)
    {
        MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        // ── Snapshot'i guncelle (GUI thread bunu 10 Hz okuyor) ──
        // Pasif monitorler de raporlanmali, bu yuzden zoom kontrolunden ONCE.
        auto& st = m_status.Monitor(i);
        st.zoomLevel.store(mon->zoom.zoomLevel, std::memory_order_relaxed);
        st.isActive.store(mon->zoom.isActive, std::memory_order_relaxed);
        st.isFrozen.store(mon->zoom.isFrozen, std::memory_order_relaxed);
        st.captureOk.store(
            m_captures[i].IsInitialized() && !m_captures[i].NeedsReinit(),
            std::memory_order_relaxed);
        st.captureExcluded.store(m_overlays[i].IsExcludedFromCapture(),
                                 std::memory_order_relaxed);

        // ── Zoom pasif → overlay'i gizle, capture'a dokunma ──
        if (!mon->zoom.isActive)
        {
            if (m_overlays[i].IsVisible())
                m_overlays[i].Hide();

            // FPS is meaningless with zoom off; zero it so the panel shows a dash.
            const size_t slot = (i < StatusSnapshot::kMaxMonitors)
                              ? i : StatusSnapshot::kMaxMonitors - 1;
            st.fps.store(0.0f, std::memory_order_relaxed);
            m_lastFrameTime[slot] = {};

            // Clear the remembered rect too. Without this, zooming back in on
            // the same region reads as "nothing changed" and the draw is
            // skipped — and after a Present the back buffer contents are
            // undefined, so what appears on screen is garbage.
            m_lastSrcRect[slot] = RECT{};
            continue;
        }

        anyActive = true;

        if (!m_overlays[i].IsVisible())
            m_overlays[i].Show();

        // Only when the cursor has genuinely moved. Updating unconditionally
        // overwrites whatever OnFocusChanged wrote on every frame, which makes
        // focus following look like it does nothing at all.
        const bool cursorMoved = (cursor.x != m_lastCursorPos.x)
                              || (cursor.y != m_lastCursorPos.y);

        if (cursorMoved && !mon->zoom.isFrozen && PtInRect(&mon->bounds, cursor))
        {
            mon->zoom.focalPoint = cursor;
        }

        // Duplication is lost on things like a fullscreen game taking the
        // output. Retrying is free; failure just means trying again next frame.
        if (m_captures[i].NeedsReinit())
            m_captures[i].Reinitialize();

        RenderMonitor(i);
    }

    UpdatePointerCompositing(anyActive);

    // A periodic backstop only. The real mechanism is event-driven
    // (WM_APP_ASSERT_TOPMOST); this catches whatever slips past it, and is
    // rate-limited inside AssertOverlaysTopmost.
    if (anyActive)
        AssertOverlaysTopmost();

    // Present with vSync paces the loop while zoom is on. When nothing was
    // presented — zoom off, or the frame skipped because nothing changed —
    // that brake is absent and the loop would spin a core.
    //
    // 4 ms is short enough to notice a change (a 240 Hz poll) and long enough
    // to not be a spin.
    if (!m_presentedThisTick)
        Sleep(anyActive ? 4 : 8);

    m_lastCursorPos = cursor;
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

    // Timeout 0: take a frame if one is ready, otherwise return immediately.
    // Blocking would be wrong — with no new frame we still need to re-present
    // the previous one, or the view would not follow a moving source rect.
    CapturedFrame frame = capture.AcquireFrame(0);

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

        const float zoom = (std::max)(
            static_cast<float>(vp.zoom.load(std::memory_order_relaxed)),
            ZoomState::kMinZoom);

        // With no new frame the width and height come back zero; fall back to
        // the monitor size, which is what the capture was opened at anyway.
        const long monW = (frame.width  > 0) ? static_cast<long>(frame.width)  : mon->Width();
        const long monH = (frame.height > 0) ? static_cast<long>(frame.height) : mon->Height();

        if (monW <= 0 || monH <= 0)
        {
            capture.ReleaseFrame();
            return;
        }

        const long srcW = (std::max)(1L, static_cast<long>(monW / zoom));
        const long srcH = (std::max)(1L, static_cast<long>(monH / zoom));

        // ── Source origin comes from ViewportController ──
        //
        // This replaces the anchored identity srcOrigin = focal * (1 - 1/zoom).
        // That formula pinned the source to the cursor, which is what made a
        // click land on what it appeared to point at — and also what made the
        // view track the cursor on every single move, leaving no room for
        // edge-push panning. The identity had to go for the view to hold still.
        //
        // Click alignment is not lost, it moves: the real cursor is kept at
        // round(pointer) and the magnified sprite is drawn where the user sees
        // it. Until that sprite exists (a later task), clicks are misaligned
        // while panning, which is expected and temporary.
        //
        // The controller lives on the input thread and advances per mouse
        // event, so the pan stays proportional to mouse motion rather than to
        // frame rate.
        RECT srcRect{};
        srcRect.left = static_cast<long>(vp.srcOriginX.load(std::memory_order_relaxed));
        srcRect.top  = static_cast<long>(vp.srcOriginY.load(std::memory_order_relaxed));

        // The snapshot can be one tick behind a resolution change, and a source
        // rect outside the texture is a device removal, not a glitch.
        srcRect.left = std::clamp(srcRect.left, 0L, (std::max)(0L, monW - srcW));
        srcRect.top  = std::clamp(srcRect.top,  0L, (std::max)(0L, monH - srcH));

        srcRect.right  = srcRect.left + srcW;
        srcRect.bottom = srcRect.top  + srcH;

        // ── Imlec sprite'inin durumu — ATLAMA TESTINDEN ONCE ──
        //
        // This has to be decided before the skip below, and that ordering is
        // the whole point. The skip used to ask only "new frame, or did the
        // source rect move?" — which worked while the rect was anchored to the
        // cursor and therefore changed on every mouse move.
        //
        // Under edge-push the rect deliberately holds still: that IS the
        // feature. So a pointer moving through the middle of the screen changed
        // nothing the test could see, the frame was skipped, and the magnified
        // pointer froze in place while the real one moved. The sprite's
        // position and shape are part of what is on screen, so they belong in
        // the test.
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

        // Nothing new: no frame, no source movement, no cursor movement.
        // Presenting anyway would only block on vSync and burn GPU time.
        const RECT& lastRect = m_lastSrcRect[rectSlot];

        const bool rectSame = (lastRect.left   == srcRect.left)
                           && (lastRect.top    == srcRect.top)
                           && (lastRect.right  == srcRect.right)
                           && (lastRect.bottom == srcRect.bottom);

        const bool spriteSame = (m_lastSpritePos[rectSlot].x == spritePos.x)
                             && (m_lastSpritePos[rectSlot].y == spritePos.y)
                             && (m_lastSpriteShape[rectSlot] == spriteShape);

        if (!frame.isNewFrame && rectSame && spriteSame)
        {
            capture.ReleaseFrame();
            return;
        }

        m_lastSrcRect[rectSlot]     = srcRect;
        m_lastSpritePos[rectSlot]   = spritePos;
        m_lastSpriteShape[rectSlot] = spriteShape;

        // nullptr means "re-use the last frame".
        ID3D11Texture2D* newFrame = (frame.isNewFrame && frame.texture)
                                  ? frame.texture.Get()
                                  : nullptr;

        if (!m_renderer.RenderFrame(newFrame, monitorIndex, srcRect))
        {
            // Henuz hic frame gelmemis olabilir — bir sonraki turda tekrar denenir.
            capture.ReleaseFrame();
            return;
        }

        // ── Kendi imlecimizi icerigin uzerine ciz ──
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
            const float scale = static_cast<float>(zoom);

            // The hotspot is what the user points with, so it — not the
            // sprite's corner — is what lands on the computed position.
            m_renderer.RenderSprite(monitorIndex, shape.srv,
                static_cast<float>(spritePos.x) - shape.hotspotX * scale,
                static_cast<float>(spritePos.y) - shape.hotspotY * scale,
                static_cast<float>(shape.width)  * scale,
                static_cast<float>(shape.height) * scale);
        }

        // vSync only in flip mode.
        //
        // On a layered window, Present makes DWM update the layered surface,
        // which at 2560x1440 is expensive on its own. Waiting for vblank on top
        // of that blocked the render thread for hundreds of milliseconds at a
        // time, so it stopped pumping messages, WM_HOTKEY went unprocessed and
        // the application appeared to hang. That was the observed behaviour:
        // the keys worked for a while and then went silent entirely.
        //
        // So layered mode runs without vSync. Tearing is possible; a frozen
        // application is worse. Frame rate is already bounded by the
        // nothing-changed check above and the sleep in Update.
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
                // Ustel yumusatma — ham 1/dt cok zipliyor, gostergede okunmaz.
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

    // Unconditional after AcquireFrame: skipping it makes the next
    // AcquireFrame fail with "frame already acquired".
    capture.ReleaseFrame();
}

// =============================================================================
// AssertOverlaysTopmost — menu/popup'larin uzerinde kal
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
void App::AssertOverlaysTopmost()
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

    bool any = false;
    for (auto& overlay : m_overlays)
    {
        if (overlay.IsVisible())
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

        // wcsncpy_s + _TRUNCATE: her zaman null-terminated, tasma yok
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

// Win+Z veya tray cift tik / menu → farenin uzerinde oldugu monitorde toggle.
// Windows Magnifier TUM ekranlari birlikte buyutur; bizim farkimiz bu:
// her monitorde BAGIMSIZ zoom.
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

void App::ToggleZoomOnMonitor(size_t i)
{
    const MonitorInfo* before = m_monitorManager.GetMonitor(i);
    if (!before)
        return;

    // Kullanimdaki seviyeyi toggle'DAN ONCE oku: ToggleZoom kapatirken
    // zoomLevel'i kMinZoom'a sifirliyor, sonra okursak 1.0 goruruz.
    const float levelInUse = before->zoom.zoomLevel;
    const bool  wasActive  = before->zoom.isActive;

    m_monitorManager.ToggleZoom(i);

    const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
    if (!mon)
        return;

    const auto ms = m_settings.Monitor(mon->deviceName);

    if (mon->zoom.isActive)
    {
        // Zoom acilinca hangi seviyeden baslasin?
        // rememberZoomLevel aciksa son kullanilan seviye, degilse
        // minZoom'un iki kati.
        float startZoom = m_settings.General().rememberZoomLevel
            ? std::clamp(ms.lastZoom, ms.minZoom, ms.maxZoom)
            : std::clamp(ms.minZoom * 2.0f, ms.minZoom, ms.maxZoom);

        // Turning zoom on at 1.0x reads as "zoom is broken". A stale
        // settings.ini with LastZoom=1 used to produce exactly that, so the
        // floor is enforced here rather than expecting anyone to delete a file.
        if (startZoom <= ms.minZoom)
            startZoom = std::clamp(ms.minZoom * 2.0f, ms.minZoom, ms.maxZoom);

        m_monitorManager.SetZoom(i, startZoom);
        m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: on");
    }
    else
    {
        // Save the level in use now, not at shutdown. By then zoomLevel has
        // already been reset to 1.0 and LastZoom=1 is what reaches the disk.
        if (wasActive
            && m_settings.General().rememberZoomLevel
            && levelInUse > ms.minZoom)
        {
            auto updated = ms;
            updated.lastZoom = std::clamp(levelInUse, ms.minZoom, ms.maxZoom);
            m_settings.SetMonitor(mon->deviceName, updated);
        }

        m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: off");
    }
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
// OnZoomStep — zoom'u bir adim degistir
// =============================================================================
// Kaynaklari: Ctrl+Alt+tekerlek, Win+arti, Win+eksi.
//
// Windows Magnifier davranisini taklit ediyor:
//   Zoom KAPALI + yon(+)  -> ac (baslangic seviyesinde)
//   Zoom ACIK  + yon(+)   -> bir adim buyut
//   Zoom ACIK  + yon(-)   -> bir adim kucult; minZoom'a inince KAPAT
//   off + down  -> nothing; there is nothing below off
//
// Stepping up turns zoom ON, which it did not used to. The old behaviour only
// responded while zoom was already active, because the bare wheel was not
// swallowed and stealing it would have broken normal scrolling. Now that
// Ctrl+Alt+wheel is swallowed the combination is ours, and using it to turn
// zoom on is fair game.
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

        // ── Kapaliyken buyutme istegi = ac ──
        if (!mon->zoom.isActive)
        {
            if (direction <= 0)
                return;   // Kapali olani daha fazla kapatamayiz

            m_monitorManager.ToggleZoom(i);

            const float startZoom = m_settings.General().rememberZoomLevel
                ? std::clamp(ms.lastZoom, ms.minZoom, ms.maxZoom)
                : std::clamp(ms.minZoom + ms.zoomStep, ms.minZoom, ms.maxZoom);

            m_monitorManager.SetZoom(i, startZoom);
            m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: on");

            LOG_INFO("Monitor {} zoom acildi ({:.2f}x) — Win+arti / Ctrl+Alt+tekerlek", i, startZoom);
            return;
        }

        // ── Acikken adim ──
        const float step = (direction > 0) ? ms.zoomStep : -ms.zoomStep;
        m_monitorManager.AdjustZoom(i, step);

        // ── minZoom'a inildiyse kapat ──
        // Windows Magnifier'in Win+eksi davranisi. Not: minZoom ayarda 1.0'dan
        // buyukse (orn. 1.5) o seviyede kapanir — tuhaf gorunebilir ama tutarli.
        const MonitorInfo* after = m_monitorManager.GetMonitor(i);
        if (after && direction < 0 && after->zoom.zoomLevel <= ms.minZoom)
        {
            // Kapatmadan ONCE kullanilan seviyeyi sakla: ToggleZoom
            // zoomLevel'i minZoom'a sifirliyor.
            if (m_settings.General().rememberZoomLevel)
            {
                auto updated = ms;
                updated.lastZoom = std::clamp(ms.minZoom + ms.zoomStep,
                                              ms.minZoom, ms.maxZoom);
                m_settings.SetMonitor(mon->deviceName, updated);
            }

            m_monitorManager.ToggleZoom(i);
            m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: off");
            LOG_INFO("Monitor {} zoom kapandi (minZoom'a inildi)", i);
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

    // Sifir boyutlu pencereleri yoksay
    if (rc.right <= rc.left || rc.bottom <= rc.top)
        return;

    const POINT center{ (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };

    MonitorInfo* target = m_monitorManager.FindByPoint(center);
    if (!target)
        return;

    // Only when that monitor is actually magnified and not frozen.
    if (!target->zoom.isActive || target->zoom.isFrozen)
        return;

    // SetCursorPos was tried here and reverted.
    //
    // The idea was to move the CURSOR rather than the anchor, preserving the
    // "anchor == cursor" invariant so focus following and click alignment could
    // both hold at once.
    //
    // It was destructive in practice. Moving the pointer triggers hover and
    // focus in whatever now sits under it, which raises another
    // EVENT_OBJECT_FOCUS, which moves it again: a feedback loop. Walking down a
    // context menu sent the pointer flying up and off the screen.
    //
    // Guarding with "skip if the cursor is already inside the focused window"
    // did not save it either — a menu can leave focus on its owner window while
    // the pointer sits in the menu, so the guard misses and the pointer gets
    // thrown to the owner's centre.
    //
    // The lesson: moving the pointer without the user asking races every piece
    // of UI that reacts to pointer position. A magnifier has no business doing
    // it.
    //
    // INERT SINCE THE VIEWPORT MOVED TO ViewportController. focalPoint no
    // longer feeds the source rect, so FollowMode::MouseAndFocus currently does
    // nothing visible. Deliberately left rather than deleted: the state is
    // still correct and the panel still exposes the mode. Reconnecting it means
    // asking the controller to centre a monitor on a point, which is a small
    // addition but belongs with the pointer work rather than ahead of it.
    target->zoom.focalPoint = center;
}

// =============================================================================
// ResolveMonitorIndex — wParam'i monitor indeksine cevir
// =============================================================================
// kFocusedMonitor sentinel'i = "farenin uzerinde oldugu monitor".
// Diger degerler dogrudan indeks. Sinir disi indeks false doner.
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
// ApplySettings — settings changed, push them into the engine
// =============================================================================
// The panel writes SettingsStore and only then posts the message, so reading
// here is safe without synchronisation. That ordering is load-bearing; reverse
// it and this reads values that have not been written yet.
// =============================================================================
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

    EdgePushConfig cfg;
    cfg.enabled      = (g.followMode == FollowMode::EdgePush);
    cfg.bandFraction = g.edgeBandFraction;
    m_inputThread.SetEdgePushConfig(cfg);

    m_inputThread.Pointer().SetSpeed(g.pointerSpeed);
    m_inputThread.Pointer().SetCompensation(g.pointerCompensation);
    m_inputThread.Pointer().SetLockToMonitor(g.lockPointerToMonitor);

    LOG_INFO("Pointer settings applied: speed={:.2f} comp={:.2f} lock={} "
             "scaling={} edgePush={} band={:.2f}",
             g.pointerSpeed, g.pointerCompensation,
             g.lockPointerToMonitor ? "on" : "off",
             g.pointerScaling ? "on" : "off",
             cfg.enabled ? "on" : "off",
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

void App::ApplySettings()
{
    LOG_INFO("Ayarlar uygulaniyor...");

    const auto& g = m_settings.General();

    // Hotkey'leri yeniden kaydet, sonucu panele bildir
    const UINT failedMask = m_hotkeyManager.Reregister(g);
    m_status.hotkeyFailedMask.store(failedMask, std::memory_order_release);

    // Input thread'in atomic bayraklarini guncelle
    m_inputThread.SetFollowMode(g.followMode);
    m_inputThread.SetHijackMagnifierKeys(g.hijackMagnifierKeys);

    ApplyPointerSettings();

    // Mevcut zoom yeni sinirlarin disinda kaldiysa iceri cek
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

    // Yikma sirasi: capture (duplication session) → swap chain → overlay pencere
    m_captures.clear();

    for (size_t i = 0; i < m_overlays.size(); ++i)
        m_renderer.RemoveRenderTarget(i);

    m_overlays.clear();

    m_monitorManager.Refresh();

    // Yeniden kur (InitializeComponents'in per-monitor kismi)
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

    // Yeni monitor bilgilerini snapshot'a yaz — panel basliklari guncellensin
    PublishMonitorInfo();

    // Bump the layout epoch so the input thread re-reads every rect and
    // re-clamps. Without this the controller keeps panning against the old
    // resolution and srcOrigin sticks to a bound that no longer exists.
    PublishViewportRequests(true);

    m_controlPanel.NotifyDisplayChange();

    LOG_INFO("Pipeline yeniden kuruldu ({} monitor)", m_overlays.size());
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
    UpdatePointerCompositing(false);
    m_cursorCache.Clear();

    // GUI thread first: the panel holds pointers to m_settings and m_status and
    // has to be gone before those become invalid.
    m_controlPanel.Stop();

    // 1. Input thread'i sonra durdur — hook'lar kalkmadan mesaj penceresini
    // yikmak, yolda olan bir PostMessage'in olu HWND'ye gitmesi demek.
    m_inputThread.Stop();

    // 1a. Hotkey kayitlarini kaldir
    m_hotkeyManager.Shutdown();

    // 2. Tray icon'u kaldir
    m_trayIcon.Destroy();

    // 3. Duplication session'lari kapat (device'dan once!)
    m_captures.clear();

    // 4. Swap chain'leri birak — SONRA pencereleri yik.
    // Ters sira yaparsak swap chain yok olmus bir HWND'ye referans tutar.
    for (size_t i = 0; i < m_overlays.size(); ++i)
        m_renderer.RemoveRenderTarget(i);

    // 5. Overlay pencereleri yik
    m_overlays.clear();

    // 6. Message window
    if (m_messageHwnd)
    {
        WTSUnRegisterSessionNotification(m_messageHwnd);
        DestroyWindow(m_messageHwnd);
        m_messageHwnd = nullptr;
    }
    UnregisterClassW(kMsgWindowClass, m_hInstance);

    // 7. Ayarlari kaydet — son zoom seviyeleri dahil
    if (m_settings.General().rememberZoomLevel)
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

    // 8. m_renderer destructor'i device'i en son birakir (member olarak)

    s_instance    = nullptr;
    m_initialized = false;

    LOG_INFO("App kapatildi");
}

} // namespace BetterMagnifier
