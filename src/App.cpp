// =============================================================================
// App.cpp — Ana Uygulama Orkestratoru Implementation
// =============================================================================
//
// Python analojisi:
//   class App:
//       def __init__(self): ...
//       def run(self):
//           while self.running:
//               self.pump_events()
//               self.update()
//
// Bu sinif "tutkal" (glue) katmani. Kendi basina is yapmiyor, sadece
// component'leri dogru sirada baslatip birbirine bagliyor:
//
//   MonitorManager  → hangi monitorler var, her birinin zoom state'i
//   D3DRenderer     → GPU device + her monitor icin swap chain
//   DXGICapture     → her monitor icin desktop duplication session
//   OverlayWindow   → her monitor icin tam ekran click-through pencere
//   HotkeyManager   → Win+Z / Win+Shift+Z / mouse wheel
//   TrayIcon        → sag tik menusu, cift tik toggle
//
// INIT SIRASI KRITIK:
//   MonitorManager once cagrilmali (kac monitor var bilmeliyiz)
//   D3DRenderer sonra (device lazim)
//   Overlay + SwapChain + Capture per-monitor (device'a bagimli)
//   Hotkey + Tray en son (message window'a bagimli)
//
// =============================================================================

#include "pch.h"
#include "App.h"
#include "Logger.h"

namespace BetterMagnifier {

App* App::s_instance = nullptr;

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
// CreateMessageWindow — Gizli mesaj penceresi
// =============================================================================
//
// Neden gorunmez ama GERCEK bir pencere (HWND_MESSAGE degil)?
//   RegisterHotKey ve Shell_NotifyIcon bir HWND istiyor. HWND_MESSAGE parent'li
//   "message-only window" bunlarin ikisinde de calisir AMA tray context menu'de
//   sorun cikarir: TrackPopupMenu'nun menuyu dogru kapatmasi icin pencerenin
//   foreground olabilmesi gerekir, message-only pencereler foreground olamaz.
//   Bu yuzden normal bir WS_POPUP pencere aciyoruz ve hic ShowWindow demiyoruz.
//
// Python analojisi: tkinter'da root.withdraw() — pencere var ama gorunmez.
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

    LOG_DEBUG("Mesaj penceresi olusturuldu: HWND=0x{:X}",
        reinterpret_cast<uintptr_t>(m_messageHwnd));
    return true;
}

// =============================================================================
// InitializeComponents
// =============================================================================
bool App::InitializeComponents()
{
    // ── 1. Monitorler ──
    if (!m_monitorManager.Initialize())
    {
        LOG_ERROR("MonitorManager baslatilamadi");
        return false;
    }

    // ── 2. GPU device ──
    if (!m_renderer.Initialize())
    {
        LOG_ERROR("D3DRenderer baslatilamadi");
        return false;
    }

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
            LOG_ERROR("Monitor {} icin overlay olusturulamadi — bu monitor atlaniyor", i);
            continue;
        }

        // Swap chain (overlay'in HWND'sine bagli)
        if (!m_renderer.CreateSwapChainForWindow(
                overlay.GetHwnd(),
                static_cast<UINT>(mon->Width()),
                static_cast<UINT>(mon->Height()),
                i))
        {
            LOG_ERROR("Monitor {} icin swap chain olusturulamadi", i);
            // Overlay'i yine tutuyoruz — hotkey/tray calismaya devam etsin
        }

        // Desktop Duplication
        DXGICapture capture;
        if (mon->dxgiOutput)
        {
            if (!capture.Initialize(m_renderer.GetDevice(), mon->dxgiOutput.Get()))
            {
                LOG_ERROR("Monitor {} icin capture baslatilamadi", i);
            }
        }
        else
        {
            LOG_WARN("Monitor {} icin DXGI output yok — capture edilemez", i);
        }

        m_overlays.push_back(std::move(overlay));
        m_captures.push_back(std::move(capture));
    }

    if (m_overlays.empty())
    {
        LOG_ERROR("Hicbir monitor icin overlay olusturulamadi");
        return false;
    }

    // ── 4. Hotkey + Tray (message window'a bagli) ──
    m_hotkeyManager.Initialize(m_messageHwnd);
    m_trayIcon.Create(m_messageHwnd, m_hInstance);

    // ── 5. Statik monitor bilgilerini snapshot'a yaz ──
    PublishMonitorInfo();

    return true;
}

// =============================================================================
// SetupCallbacks — Component'leri birbirine bagla
// =============================================================================
// Python analojisi: button.config(command=self.on_click)
// Lambda'lar "this"i yakaliyor — App yasadigi surece gecerli.
// =============================================================================
void App::SetupCallbacks()
{
    m_hotkeyManager.SetToggleZoomCallback([this] { OnToggleZoom(); });
    m_hotkeyManager.SetFreezeCallback([this] { OnFreeze(); });
    m_hotkeyManager.SetScrollCallback([this](int delta, POINT pos) { OnScroll(delta, pos); });

    m_trayIcon.SetToggleCallback([this] { OnToggleZoom(); });
    m_trayIcon.SetExitCallback([] { PostQuitMessage(0); });
}

// =============================================================================
// Run — Hybrid message loop (game-loop pattern)
// =============================================================================
//
// GetMessage yerine PeekMessage:
//   GetMessage mesaj gelene kadar BLOKLAR — render dongusu durur.
//   PeekMessage bloklamaz — mesaj yoksa hemen doner, biz render yapabiliriz.
//
// Frame pacing:
//   Zoom aktifse Present(vSync=true) bizi monitor refresh rate'ine kilitler
//   (60/144 Hz). Bu dogal frame limiter — ekstra Sleep gerekmez.
//   Zoom pasifse hicbir sey render edilmiyor, o zaman CPU'yu yakmamak icin
//   kisa Sleep koyuyoruz.
// =============================================================================
int App::Run()
{
    if (!m_initialized)
    {
        LOG_ERROR("Run() cagirildi ama App initialize edilmemis!");
        return 1;
    }

    m_running = true;
    MSG msg{};

    LOG_INFO("Message loop basladi");

    while (m_running)
    {
        // ── Bekleyen tum mesajlari isle ──
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
void App::Update()
{
    m_status.monitorCount.store(m_overlays.size(), std::memory_order_relaxed);

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

            // Zoom kapaliyken FPS anlamsiz — panelde "—" gorunmesi icin sifirla
            st.fps.store(0.0f, std::memory_order_relaxed);
            m_lastFrameTime[i < StatusSnapshot::kMaxMonitors
                            ? i : StatusSnapshot::kMaxMonitors - 1] = {};
            continue;
        }

        anyActive = true;

        if (!m_overlays[i].IsVisible())
            m_overlays[i].Show();

        // ── Freeze aktif degilse focal point'i fareye kilitle ──
        // Fare bu monitorde degilse focal point'i oldugu yerde birak.
        if (!mon->zoom.isFrozen && PtInRect(&mon->bounds, cursor))
        {
            mon->zoom.focalPoint = cursor;
        }

        // ── Capture recovery (fullscreen oyun acilip kapaninca) ──
        if (m_captures[i].NeedsReinit())
        {
            m_captures[i].Reinitialize();
            // Basarisiz olduysa sorun degil — sonraki frame tekrar denenir
        }

        RenderMonitor(i);
    }

    // Hicbir monitor aktif degil → bosa CPU yakma.
    // Zoom aktifse Present(vSync) bizi zaten refresh rate'e kilitliyor.
    // ponytail: sabit 8ms; idle'da MsgWaitForMultipleObjects daha dogru olur
    // ama olay bazli uyanma icin overlay/hotkey akisini yeniden kurmak gerekir.
    if (!anyActive)
        Sleep(8);
}

// =============================================================================
// RenderMonitor — Tek monitor icin capture → render → present
// =============================================================================
//
// Zoom matematigi:
//   Ekran 1920x1080, zoom = 2.0x ise:
//     Kaynak bolge = 1920/2 x 1080/2 = 960x540
//     Bu 960x540 bolge, 1920x1080 overlay'e gerilir → 2x buyume
//
//   Focal point = bu bolgenin merkezi (genelde fare pozisyonu).
//   Bolge monitor sinirlarini asmasin diye clamp ediyoruz.
//
// Python analojisi:
//   w, h = mon_w / zoom, mon_h / zoom
//   left = clamp(focal_x - w/2, 0, mon_w - w)
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

    // ── Frame yakala ──
    // timeout 0 = bloklamadan sor: yeni frame varsa al, yoksa hemen don.
    // Bloklamiyoruz cunku frame gelmese de eski goruntuyu tekrar sunmamiz gerekir
    // (yoksa fare hareket ederken zoom bolgesi guncellenmez).
    CapturedFrame frame = capture.AcquireFrame(0);

    if (frame.isNewFrame && frame.texture)
    {
        // ── Zoom bolgesini hesapla (monitor-local koordinatlar) ──
        const float zoom = (std::max)(mon->zoom.zoomLevel, ZoomState::kMinZoom);

        const long monW = static_cast<long>(frame.width);
        const long monH = static_cast<long>(frame.height);

        const long srcW = static_cast<long>(monW / zoom);
        const long srcH = static_cast<long>(monH / zoom);

        // Focal point ekran koordinatlarinda — monitor-local'a cevir
        const long focalX = mon->zoom.focalPoint.x - mon->bounds.left;
        const long focalY = mon->zoom.focalPoint.y - mon->bounds.top;

        RECT srcRect{};
        srcRect.left   = std::clamp(focalX - srcW / 2, 0L, (std::max)(0L, monW - srcW));
        srcRect.top    = std::clamp(focalY - srcH / 2, 0L, (std::max)(0L, monH - srcH));
        srcRect.right  = srcRect.left + srcW;
        srcRect.bottom = srcRect.top  + srcH;

        m_renderer.RenderFrame(frame.texture.Get(), monitorIndex, srcRect);
        m_renderer.Present(monitorIndex, true);

        // ── FPS olcumu ──
        // Present'ten SONRA olcuyoruz, cunku vSync bekleyisi de frame
        // suresinin parcasi. Iki frame arasi sureyi 1/dt ile FPS'e ceviriyoruz.
        //
        // Python analojisi: time.perf_counter() farki. Fark: steady_clock
        // monotonic garantisi veriyor — sistem saati geri alinsa bile bozulmaz.
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

    // AcquireFrame'den sonra HER DURUMDA ReleaseFrame — yoksa sonraki
    // AcquireFrame "frame already acquired" ile patlar.
    capture.ReleaseFrame();
}

// =============================================================================
// PublishMonitorInfo — statik monitor bilgilerini snapshot'a yaz
// =============================================================================
// Panel bunlari kart basliklarinda gosteriyor. Sadece init ve
// WM_DISPLAYCHANGE'de cagriliyor — her frame degil.
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
    POINT cursor{};
    GetCursorPos(&cursor);

    const MonitorInfo* target = m_monitorManager.FindByPoint(cursor);
    if (!target)
    {
        LOG_WARN("Fare hicbir monitorde bulunamadi, toggle atlandi");
        return;
    }

    // FindByPoint pointer donuyor, bize index lazim — handle ile esle
    const auto& monitors = m_monitorManager.GetMonitors();
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        if (monitors[i].hMonitor == target->hMonitor)
        {
            m_monitorManager.ToggleZoom(i);

            const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
            if (mon && mon->zoom.isActive)
            {
                // Zoom acilinca 2x'ten basla (1.0x'te acmak anlamsiz)
                m_monitorManager.SetZoom(i, 2.0f);
                m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: Aktif");
            }
            else
            {
                m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: Pasif");
            }
            return;
        }
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

// Mouse wheel → zoom.
//
// DIKKAT: Low-level mouse hook scroll'u ENGELLEMEZ, altindaki uygulamaya da
// gider. Bu yuzden scroll'u sadece o monitorde zoom AKTIFKEN yakaliyoruz.
// Boylece normal scroll davranisini hicbir zaman bozmuyoruz — zoom kapaliysa
// hicbir sey olmuyor.
void App::OnScroll(int delta, POINT mousePos)
{
    const MonitorInfo* target = m_monitorManager.FindByPoint(mousePos);
    if (!target || !target->zoom.isActive)
        return;

    const auto& monitors = m_monitorManager.GetMonitors();
    for (size_t i = 0; i < monitors.size(); ++i)
    {
        if (monitors[i].hMonitor == target->hMonitor)
        {
            const float step = (delta > 0) ? ZoomState::kZoomStep : -ZoomState::kZoomStep;
            m_monitorManager.AdjustZoom(i, step);
            return;
        }
    }
}

// =============================================================================
// OnDisplayChange — Monitor takildi/cikarildi/cozunurluk degisti
// =============================================================================
//
// MonitorManager::Refresh() listeyi SIFIRDAN kuruyor — eski MonitorInfo'daki
// dxgiOutput pointer'lari gecersiz oluyor. Bu yuzden capture/overlay/swap chain
// zincirinin TAMAMINI yikip yeniden kurmak zorundayiz.
//
// Zoom state'leri MonitorManager device name uzerinden koruyor, onlari
// kaybetmiyoruz.
// =============================================================================
void App::OnDisplayChange()
{
    LOG_INFO("Display degisikligi — pipeline yeniden kuruluyor");

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

    LOG_INFO("Pipeline yeniden kuruldu ({} monitor)", m_overlays.size());
}

// =============================================================================
// MessageWndProc — Gizli mesaj penceresinin mesaj isleyicisi
// =============================================================================
//
// Neden static? Win32 WndProc'lar C fonksiyon pointer'i olmak zorunda,
// member function olamaz. s_instance ile App'e ulasiyoruz.
// Python'da bu sorun yok — bound method zaten callable.
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

    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// =============================================================================
// Shutdown — Ters sirada yik
// =============================================================================
//
// C++ RAII kurali: son olusturulan ilk yikilir (LIFO).
// Burada elle sirayi zorluyoruz cunku member declaration sirasi bizim
// istedigimiz yikma sirasi degil.
// =============================================================================
void App::Shutdown()
{
    if (!m_initialized)
        return;

    LOG_INFO("App kapatiliyor...");

    m_running = false;

    // 1. Girdi kaynaklarini kes (hook + hotkey) — artik callback gelmesin
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

    // 6. Mesaj penceresi
    if (m_messageHwnd)
    {
        DestroyWindow(m_messageHwnd);
        m_messageHwnd = nullptr;
    }
    UnregisterClassW(kMsgWindowClass, m_hInstance);

    // 7. m_renderer destructor'i device'i en son birakir (member olarak)

    s_instance    = nullptr;
    m_initialized = false;

    LOG_INFO("App kapatildi");
}

} // namespace BetterMagnifier
