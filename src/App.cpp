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
    // ── 0. Ayarlar ──
    // Diger her sey ayarlara bagli olabilir, en once yukleniyor.
    // Dosya yoksa varsayilanlarla devam eder — ilk calistirma hata degil.
    m_settings.Load();

    // MouseAndFocus acikken capa imlecten koparak odaklanan pencereye gidiyor,
    // dolayisiyla TIKLAMA GORDUGUN YERE GITMIYOR (bkz. OnFocusChanged).
    // Ikisi ayni anda mumkun degil. Ayar dosyasi eski varsayilanla yazilmis
    // olabilir, o yuzden sessizce degistirmek yerine uyariyoruz.
    if (m_settings.General().followMode == FollowMode::MouseAndFocus)
    {
        LOG_WARN("FollowMode=MouseAndFocus — zoom bolgesi klavye odagini takip "
                 "edecek AMA tiklama hizalamasi bozulur. Tiklamanin dogru yere "
                 "gitmesini istiyorsan settings.ini'de FollowMode=Mouse yap.");
    }

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
    m_hotkeyManager.Initialize(m_messageHwnd, m_settings.General());
    m_trayIcon.Create(m_messageHwnd, m_hInstance);

    m_status.hotkeyFailedMask.store(m_hotkeyManager.LastFailedMask(),
                                    std::memory_order_release);

    // ── 5. Input thread ──
    // Hook'lar BURADA, render thread'de DEGIL (bkz. InputThread.h).
    // Basarisiz olursa scroll zoom calismaz ama uygulama ayakta kalir.
    if (!m_inputThread.Start(m_messageHwnd,
                             m_settings.General().followMode,
                             m_settings.General().hijackMagnifierKeys))
    {
        LOG_WARN("InputThread baslatilamadi — mouse wheel zoom devre disi");
    }

    // ── 6. Statik monitor bilgilerini snapshot'a yaz ──
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
    // Scroll artik InputThread'den WM_APP_SCROLL_ZOOM olarak geliyor.

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
    m_presentedThisTick = false;

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
            const size_t slot = (i < StatusSnapshot::kMaxMonitors)
                              ? i : StatusSnapshot::kMaxMonitors - 1;
            st.fps.store(0.0f, std::memory_order_relaxed);
            m_lastFrameTime[slot] = {};

            // Son cizilen bolgeyi de sifirla. Yoksa zoom tekrar acildiginda
            // ayni bolge cikarsa "degisen yok" deyip cizimi atlariz —
            // FLIP_DISCARD'da Present sonrasi back buffer icerigi TANIMSIZ,
            // yani ekranda cop gorunur.
            m_lastSrcRect[slot] = RECT{};
            continue;
        }

        anyActive = true;

        if (!m_overlays[i].IsVisible())
            m_overlays[i].Show();

        // ── Freeze aktif degilse focal point'i fareye kilitle ──
        //
        // ONEMLI: sadece fare GERCEKTEN HAREKET ETTIYSE. Yoksa klavye odagi
        // takibinin (OnFocusChanged) yazdigi focal point'i her frame eziyoruz
        // ve Tab ile odak degistirmek hicbir sey yapmiyor gibi gorunuyor.
        const bool cursorMoved = (cursor.x != m_lastCursorPos.x)
                              || (cursor.y != m_lastCursorPos.y);

        if (cursorMoved && !mon->zoom.isFrozen && PtInRect(&mon->bounds, cursor))
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
    // Periyodik yedek: olay bazli tetikleme (WM_APP_ASSERT_TOPMOST) asil yol,
    // bu sadece kacan durumlar icin ag. 250 ms yeterince seyrek.
    if (anyActive)
        AssertOverlaysTopmost();

    // ── Bosa donmeyi engelle ──
    // Zoom aktifken Present(vSync) loop'u dogal olarak frame hizina kilitler.
    // Ama hicbir monitore Present etmediysek (zoom kapali, ya da hicbir sey
    // degismedigi icin cizim atlandi) o fren yok — loop CPU'yu yakar.
    //
    // 4 ms, degisimi fark etmek icin yeterince kisa (240 Hz'lik yoklama),
    // bos dongu icin yeterince uzun.
    if (!m_presentedThisTick)
        Sleep(anyActive ? 4 : 8);

    // Bir sonraki frame'de "fare hareket etti mi" karsilastirmasi icin
    m_lastCursorPos = cursor;
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

    // Yeni frame GELMESE DE ciziyoruz.
    //
    // Desktop Duplication ekran icerigi degismedikce frame vermez. Eskiden
    // sadece isNewFrame durumunda render ediliyordu; sabit bir ekranda fareyi
    // gezdirince zoom bolgesi oldugu yerde kaliyordu. Renderer son frame'in
    // kopyasini tuttugu icin artik focal point degistiginde onu yeniden
    // olceklendirip sunabiliyoruz.
    {
        // ── Zoom bolgesini hesapla (monitor-local koordinatlar) ──
        const float zoom = (std::max)(mon->zoom.zoomLevel, ZoomState::kMinZoom);

        // Yeni frame yoksa frame.width/height sifir gelir — monitor
        // boyutuna duseriyoruz (capture zaten monitor boyutunda acildi).
        const long monW = (frame.width  > 0) ? static_cast<long>(frame.width)  : mon->Width();
        const long monH = (frame.height > 0) ? static_cast<long>(frame.height) : mon->Height();

        if (monW <= 0 || monH <= 0)
        {
            capture.ReleaseFrame();
            return;
        }

        const long srcW = (std::max)(1L, static_cast<long>(monW / zoom));
        const long srcH = (std::max)(1L, static_cast<long>(monH / zoom));

        // Focal point ekran koordinatlarinda — monitor-local'a cevir
        const long focalX = mon->zoom.focalPoint.x - mon->bounds.left;
        const long focalY = mon->zoom.focalPoint.y - mon->bounds.top;

        // ── IMLEC CAPALI DONUSUM (ortalama DEGIL) ──
        //
        // Eskiden bolge focal point'in ORTASINA kuruluyordu:
        //     srcOrigin = focal - srcSize/2
        // Bunun sonucu: imlecin altindaki nokta overlay'in MERKEZINDE
        // ciziliyordu, ama gercek imlec bitmap'i sistem tarafindan kendi
        // gercek konumunda ciziliyor. Ikisi farkli yerde => gordugun yer ile
        // tikladigin yer uyusmuyordu, zoom acikken Windows kullanilamiyordu.
        //
        // Simdi bolgeyi focal point'e CAPALIYORUZ:
        //     srcOrigin = focal * (1 - 1/zoom)
        //
        // Bu donusumde ekran konumu s, kaynak pikseli (srcOrigin + s/zoom)
        // gosteriyor. s = focal icin:
        //     focal*(1 - 1/zoom) + focal/zoom = focal
        // Yani focal point'teki nokta ekranda TAM OLARAK focal point'e dusuyor.
        // Buyutmenin sabit noktasi imlec. Gercek imlec altindaki buyutulmus
        // icerige birebir oturuyor, koordinat donusumu yapmaya gerek yok —
        // mevcut click-through zaten dogru yere gidiyor.
        //
        // CLAMP GEREKMIYOR: formul her focal konumunda sinir icinde kaliyor.
        //   focal = 0     -> origin = 0
        //   focal = monW  -> origin = monW*(1-1/zoom),
        //                    sag kenar = origin + monW/zoom = monW
        // Yani ekranin her yerine erisilebiliyor ve tasma olmuyor.
        //
        // Python analojisi: PIL'de resize degil, sabit bir noktayi koruyarak
        // affine transform uygulamak — cv2.getRotationMatrix2D'nin center
        // parametresi gibi.
        const double invZoom = 1.0 / static_cast<double>(zoom);

        RECT srcRect{};
        srcRect.left = static_cast<long>(static_cast<double>(focalX) * (1.0 - invZoom));
        srcRect.top  = static_cast<long>(static_cast<double>(focalY) * (1.0 - invZoom));

        // Yuvarlama ve focal point'in monitor disina tasabildigi durumlar
        // (fare baska monitorde, freeze aktif) icin guvenlik agi.
        srcRect.left = std::clamp(srcRect.left, 0L, (std::max)(0L, monW - srcW));
        srcRect.top  = std::clamp(srcRect.top,  0L, (std::max)(0L, monH - srcH));

        srcRect.right  = srcRect.left + srcW;
        srcRect.bottom = srcRect.top  + srcH;

        // ── Degisen bir sey yoksa hic cizme ──
        // Ne yeni frame geldi ne de capa oynadi: ekranda gosterilecek yeni
        // bir sey yok. Present cagirmak sadece vSync'te bloklayip GPU
        // yakmak olurdu.
        const size_t rectSlot = (monitorIndex < StatusSnapshot::kMaxMonitors)
                              ? monitorIndex : StatusSnapshot::kMaxMonitors - 1;
        const RECT& lastRect = m_lastSrcRect[rectSlot];

        const bool rectSame = (lastRect.left   == srcRect.left)
                           && (lastRect.top    == srcRect.top)
                           && (lastRect.right  == srcRect.right)
                           && (lastRect.bottom == srcRect.bottom);

        if (!frame.isNewFrame && rectSame)
        {
            capture.ReleaseFrame();
            return;
        }

        m_lastSrcRect[rectSlot] = srcRect;

        // Yeni frame varsa onu ver; yoksa nullptr = "son frame'i tekrar kullan".
        ID3D11Texture2D* newFrame = (frame.isNewFrame && frame.texture)
                                  ? frame.texture.Get()
                                  : nullptr;

        if (!m_renderer.RenderFrame(newFrame, monitorIndex, srcRect))
        {
            // Henuz hic frame gelmemis olabilir — bir sonraki turda tekrar denenir.
            capture.ReleaseFrame();
            return;
        }

        // ── vSync SADECE flip modda ──
        // Layered pencerede Present, DWM'in layered surface'ini guncellemesini
        // gerektiriyor; 2560x1440'ta bu pahali. Ustune vblank beklemesi
        // eklenince render thread yuz milisaniyelerce bloklanip mesaj
        // pompalamayi birakiyor — WM_HOTKEY islenmiyor, uygulama donuyor.
        // Gozlenen davranis buydu: tuslar bir sure calisti, sonra tamamen sustu.
        //
        // Layered modda vSync KAPALI. Tearing riski var ama donan bir
        // uygulamadan iyidir. Frame hizini "degisen yok -> cizme" mantigi ve
        // asagidaki Sleep zaten sinirliyor.
        m_renderer.Present(monitorIndex, UseFlipOverlay());
        m_presentedThisTick = true;

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
// AssertOverlaysTopmost — menu/popup'larin uzerinde kal
// =============================================================================
// Iki kaynaktan cagriliyor:
//   1. WM_APP_ASSERT_TOPMOST — input thread yeni bir popup/menu dogdugunu
//      gorunce (EVENT_SYSTEM_MENUPOPUPSTART / EVENT_OBJECT_SHOW). ASIL YOL:
//      dropdown'lar saniyenin altinda acilip kullaniliyor, yoklama yetismiyor.
//   2. Update() her turda — kacan durumlar icin yedek ag.
//
// RATE LIMIT: EVENT_OBJECT_SHOW cok sik tetikleniyor. 40 ms alt sinir, iki
// ardisik frame'den kisa; kullanici farki gormez ama SetWindowPos firtinasi
// ve z-order gurultusu olusmaz.
// =============================================================================
void App::AssertOverlaysTopmost()
{
    // BM_NO_TOPMOST_FIGHT=1 ile kapatilabilir — popup canli kalir ama
    // buyutulmez ve cift gorunur (bkz. pch.h FightPopupZOrder).
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

    // Hicbir overlay gorunmuyorsa zaman damgasini guncellemiyoruz — zoom
    // acildiginda ilk popup icin rate limit bosa harcanmasin.
    if (any)
        m_lastTopmostAssert = now;
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
            // Kullanimdaki seviyeyi toggle'DAN ONCE oku: ToggleZoom kapatirken
            // zoomLevel'i kMinZoom'a sifirliyor, sonra okursak 1.0 goruruz.
            const float levelInUse = monitors[i].zoom.zoomLevel;
            const bool  wasActive  = monitors[i].zoom.isActive;

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

                // GUVENLIK AGI: 1.0x'te acmak "zoom calismiyor" demek.
                // Bozuk/eski bir settings.ini (LastZoom=1) bu duruma yol
                // aciyordu; kullanici dosyayi silmeden de duzelsin diye
                // burada tabana basiyoruz.
                if (startZoom <= ms.minZoom)
                    startZoom = std::clamp(ms.minZoom * 2.0f, ms.minZoom, ms.maxZoom);

                m_monitorManager.SetZoom(i, startZoom);
                m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: Aktif");
            }
            else
            {
                // Kapaniyor — kullanilan seviyeyi SIMDI sakla.
                // Shutdown'a birakmak calismiyordu: orada zoomLevel coktan
                // 1.0'a sifirlanmis oluyor ve LastZoom=1 diske yaziliyordu.
                if (wasActive
                    && m_settings.General().rememberZoomLevel
                    && levelInUse > ms.minZoom)
                {
                    auto updated = ms;
                    updated.lastZoom = std::clamp(levelInUse, ms.minZoom, ms.maxZoom);
                    m_settings.SetMonitor(mon->deviceName, updated);
                }

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

// =============================================================================
// OnZoomStep — zoom'u bir adim degistir
// =============================================================================
// Kaynaklari: Ctrl+Alt+tekerlek, Win+arti, Win+eksi.
//
// Windows Magnifier davranisini taklit ediyor:
//   Zoom KAPALI + yon(+)  -> ac (baslangic seviyesinde)
//   Zoom ACIK  + yon(+)   -> bir adim buyut
//   Zoom ACIK  + yon(-)   -> bir adim kucult; minZoom'a inince KAPAT
//   Zoom KAPALI + yon(-)  -> hicbir sey (kapali olani daha fazla kapatamayiz)
//
// Fare tekerlegi icin eski davranistan farki: eskiden sadece zoom AKTIFKEN
// tepki veriyordu cunku duz tekerlek yutulmuyordu ve normal kaydirmayi
// bozmamak gerekiyordu. Artik Ctrl+Alt+tekerlek yutuluyor, yani kombinasyon
// bize ait — zoom'u acmasi da mesru.
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
            m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: Aktif");

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
            m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: Pasif");
            LOG_INFO("Monitor {} zoom kapandi (minZoom'a inildi)", i);
        }

        return;
    }
}

// =============================================================================
// OnFocusChanged — klavye odagi degisti, focal point'i oraya kaydir
// =============================================================================
// Odaklanan pencerenin MERKEZINI focal point yapiyoruz. Daha isabetli olan
// caret pozisyonu UI Automation gerektiriyor ve uygulama bazinda tutarsiz
// calisiyor — kapsam disi.
//
// Freeze aktifse dokunmuyoruz: kullanici bilincli olarak sabitlemis.
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

    // Sadece zoom AKTIF ve frozen DEGILSE kaydir
    if (!target->zoom.isActive || target->zoom.isFrozen)
        return;

    // ── SetCursorPos DENENDI VE GERI ALINDI ──
    //
    // Fikir su idi: "capa == imlec" degismez kuralini korumak icin capa'yi
    // degil IMLECI tasimak. Boylece odak takibi ile tiklama hizalamasi
    // birbirini bozmayacakti.
    //
    // Pratikte yikici cikti. Imleci tasimak, yeni konumdaki pencerede
    // hover/odak tetikliyor -> yeni EVENT_OBJECT_FOCUS -> yeni tasima ->
    // GERI BESLEME DONGUSU. Context menusunde asagi inerken imlec yukari
    // firliyor, ekran disina cikiyor.
    //
    // "Imlec odaklanan pencerenin icindeyse dokunma" korumasi da yetmedi:
    // menu acikken odak menuye degil SAHIBI pencereye gidebiliyor, o zaman
    // imlec o pencerenin disinda kaliyor ve merkeze firlatiliyor.
    //
    // Ders: imleci kullanicidan habersiz tasimak, konuma tepki veren her UI
    // ile yaris haline giriyor. Bir magnifier bunu yapmamali.
    //
    // Simdi sadece capa'yi tasiyoruz — eski, ongorulebilir davranis.
    //
    // KABUL EDILEN BEDEL: bu mod acikken capa imlecten kopuyor, dolayisiyla
    // tiklama gordugun yere gitmiyor. Ikisi ayni anda mumkun degil. Bu yuzden
    // varsayilan FollowMode::Mouse ve acilista uyari basiyoruz.
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
// ApplySettings — GUI ayarlari degistirdi, motora uygula
// =============================================================================
// GUI once SettingsStore'u yazdi SONRA bu mesaji postaladi, yani buradaki
// okuma guvenli — yaris yok.
// =============================================================================
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

    case WM_APP_ZOOM_STEP:
        if (s_instance)
        {
            // wParam kZoomIn (1) veya kZoomOut ((WPARAM)-1).
            // WPARAM isaretsiz — isaretli okumak icin intptr_t'den geciyoruz.
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
            s_instance->OnToggleZoom();
        return 0;

    case WM_APP_TOGGLE_FREEZE:
        if (s_instance)
            s_instance->OnFreeze();
        return 0;

    case WM_APP_ASSERT_TOPMOST:
        if (s_instance)
            s_instance->AssertOverlaysTopmost();
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

    // 1. Input thread'i once durdur — hook'lar kalkmadan mesaj penceresini
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

    // 6. Mesaj penceresi
    if (m_messageHwnd)
    {
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

            // SADECE zoom acik olan monitorler icin yaz.
            //
            // Eskiden kosulsuz yaziliyordu; zoom kapali bir monitorde
            // zoomLevel 1.0 oldugu icin LastZoom=1 kaydediliyordu ve bir
            // sonraki acilista zoom 1.0x'te "aciliyordu" — yani hic
            // buyutmuyordu. Kapatma anindaki seviye zaten OnToggleZoom'da
            // saklaniyor.
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
