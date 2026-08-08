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
//   SettingsStore   → INI'den gelen kullanici tercihleri
//   MonitorManager  → hangi monitorler var, her birinin zoom state'i
//   D3DRenderer     → GPU device + her monitor icin swap chain
//   DXGICapture     → her monitor icin desktop duplication session
//   OverlayWindow   → her monitor icin tam ekran click-through pencere
//   HotkeyManager   → RegisterHotKey (toggle / freeze)
//   InputThread     → low-level fare hook'u, AYRI thread'de
//   TrayIcon        → sag tik menusu, cift tik toggle
//
// INIT SIRASI KRITIK:
//   SettingsStore en once (digerleri ayarlara bagli)
//   MonitorManager sonra (kac monitor var bilmeliyiz)
//   D3DRenderer sonra (device lazim)
//   Overlay + SwapChain + Capture per-monitor (device'a bagimli)
//   Hotkey + Tray + InputThread en son (message window'a bagimli)
//
// PER-MONITOR DIZI HIZASI (kirilirsa sessizce yanlis monitore render edilir):
//   m_overlays[i], m_captures[i] ve MonitorManager'daki i AYNI monitoru
//   gosterir. Bu yuzden bir monitor icin kurulum basarisiz olsa bile diziye
//   BOS bir eleman ekleniyor — bkz. BuildPerMonitorResources.
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
    // Dosya yoksa varsayilanlarla devam eder — hata degil.
    m_settings.Load();

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
    BuildPerMonitorResources();

    const bool anyOverlay = std::any_of(m_overlays.begin(), m_overlays.end(),
        [](const OverlayWindow& o) { return o.GetHwnd() != nullptr; });

    if (!anyOverlay)
    {
        LOG_ERROR("Hicbir monitor icin overlay olusturulamadi");
        return false;
    }

    // ── 4. Hotkey + Tray (message window'a bagli) ──
    m_hotkeyManager.Initialize(m_messageHwnd, m_settings.General());
    m_trayIcon.Create(m_messageHwnd, m_hInstance);

    // ── 5. Input thread ──
    // Hook'lar burada, render thread'de DEGIL (bkz. InputThread.h).
    // Basarisiz olursa scroll zoom calismaz ama uygulama ayakta kalir.
    if (!m_inputThread.Start(m_messageHwnd))
        LOG_WARN("InputThread baslatilamadi — mouse wheel zoom devre disi");

    return true;
}

// =============================================================================
// BuildPerMonitorResources — overlay + swap chain + capture zincirini kur
// =============================================================================
//
// DIZI HIZASI (bu fonksiyonun en kritik ozelligi):
//   Eskiden kurulum basarisiz olan monitor `continue` ile ATLANIYORDU, yani
//   diziye hic eklenmiyordu. Sonucu: monitor 0 basarisiz olursa monitor 1'in
//   overlay'i m_overlays[0]'a dusuyor, ama swap chain'i index 1 ile
//   olusturulmus oluyordu ve Update() m_overlays[i] ile
//   MonitorManager.GetMonitor(i)'yi ayni monitor sanmaya devam ediyordu.
//   Yani bir monitorun goruntusu bir baskasinin overlay'ine gidebiliyordu.
//
//   Cozum: basarisizlikta bile bos bir eleman ekliyoruz. Bos overlay'in HWND'si
//   null, bos capture initialize degil — ikisi de kendi metodlarinda zaten
//   guvenli sekilde no-op.
// =============================================================================
void App::BuildPerMonitorResources()
{
    DestroyPerMonitorResources();

    const size_t monitorCount = m_monitorManager.GetMonitorCount();

    // reserve ONEMLI: vector buyurken move ediyor, HWND/COM pointer'lar tasiniyor.
    // reserve olmadan realloc sirasinda gereksiz move + destroy zinciri olusur.
    m_overlays.reserve(monitorCount);
    m_captures.reserve(monitorCount);

    for (size_t i = 0; i < monitorCount; ++i)
    {
        OverlayWindow overlay;
        DXGICapture   capture;

        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (mon)
        {
            // Overlay pencere
            if (!overlay.Create(m_hInstance, *mon, i))
            {
                LOG_ERROR("Monitor {} icin overlay olusturulamadi", i);
            }
            // Swap chain (overlay'in HWND'sine bagli)
            else if (!m_renderer.CreateSwapChainForWindow(
                        overlay.GetHwnd(),
                        static_cast<UINT>(mon->Width()),
                        static_cast<UINT>(mon->Height()),
                        i))
            {
                LOG_ERROR("Monitor {} icin swap chain olusturulamadi", i);
                // Overlay'i yine tutuyoruz — hotkey/tray calismaya devam etsin
            }

            // Desktop Duplication
            if (mon->dxgiOutput)
            {
                if (!capture.Initialize(m_renderer.GetDevice(), mon->dxgiOutput.Get()))
                    LOG_ERROR("Monitor {} icin capture baslatilamadi", i);
            }
            else
            {
                LOG_WARN("Monitor {} icin DXGI output yok — capture edilemez", i);
            }
        }

        m_overlays.push_back(std::move(overlay));
        m_captures.push_back(std::move(capture));
    }

    m_status.monitorCount.store(m_overlays.size(), std::memory_order_relaxed);
}

// =============================================================================
// DestroyPerMonitorResources — zinciri DOGRU SIRADA yik
// =============================================================================
// Sira: capture (duplication session) → swap chain → overlay pencere.
// Ters sira yaparsak swap chain yok olmus bir HWND'ye referans tutar.
// =============================================================================
void App::DestroyPerMonitorResources()
{
    m_captures.clear();

    for (size_t i = 0; i < m_overlays.size(); ++i)
        m_renderer.RemoveRenderTarget(i);

    m_overlays.clear();

    m_lastSrcRect.fill(RECT{});
    m_lastFrameTime.fill(std::chrono::steady_clock::time_point{});
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

    // Scroll artik callback ile degil, input thread'den gelen
    // WM_APP_SCROLL_ZOOM mesajiyla geliyor (bkz. MessageWndProc).

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
    // ── Mouse pozisyonunu takip et (magnifier fareyi izler) ──
    POINT cursor{};
    GetCursorPos(&cursor);

    bool anyActive   = false;
    bool anyRendered = false;
    const size_t count = m_overlays.size();

    m_status.monitorCount.store(count, std::memory_order_relaxed);

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
            m_lastFrameTime[StatusSnapshot::ClampIndex(i)] = {};
            m_lastSrcRect[StatusSnapshot::ClampIndex(i)]   = RECT{};
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
            // Basarisiz olduysa sorun degil — DXGICapture kendi araligiyla
            // tekrar deneyecek (her frame degil).
        }

        // vSync hakki frame basina TEK monitore veriliyor. Yoksa uc aktif
        // monitorde uc ardisik Present(vSync) bekliyoruz ve efektif frame
        // hizi refresh/3'e dusuyor — ustelik bu bekleme boyunca mesaj
        // kuyrugumuz da islenmiyor.
        if (RenderMonitor(i, !anyRendered))
            anyRendered = true;
    }

    // ── Bosa CPU yakma ──
    // Sleep yerine MsgWaitForMultipleObjectsEx: bir mesaj (hotkey, tray,
    // scroll, display change) gelir gelmez uyaniyoruz, sabit uyku suresi
    // kadar gecikmiyoruz.
    if (!anyActive)
    {
        // Hicbir monitorde zoom yok — uzun uyu.
        WaitForInput(kIdleWaitMs);
    }
    else if (!anyRendered)
    {
        // Zoom acik ama ne masaustu ne de zoom bolgesi degisti. Present
        // yapmadigimiz icin vSync bizi yavaslatmiyor; kisa bir bekleme ile
        // dongunun bos donmesini engelliyoruz.
        WaitForInput(kActiveIdleWaitMs);
    }
}

// =============================================================================
// WaitForInput — mesaj gelene kadar (veya timeout) uyu
// =============================================================================
// MWMO_INPUTAVAILABLE: kuyrukta ZATEN bekleyen bir mesaj varsa hemen doner.
// Bu bayrak olmadan, PeekMessage ile bakip almadigimiz bir mesaj varken
// bekleyip takilabilirdik.
//
// Python analojisi: time.sleep(0.004) yerine
// queue.get(timeout=0.004) — olay gelirse aninda uyaniyorsun.
// =============================================================================
void App::WaitForInput(DWORD timeoutMs)
{
    MsgWaitForMultipleObjectsEx(0, nullptr, timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
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
//
// NE ZAMAN RENDER EDIYORUZ:
//   (a) masaustu icerigi degistiyse, VEYA
//   (b) zoom bolgesi degistiyse (fare hareket etti / zoom seviyesi degisti)
//
//   Eskiden sadece (a) vardi: "yeni frame yoksa hicbir sey yapma". Masaustu
//   sabitken (video yok, animasyon yok) fareyi gezdirdiginizde buyutulmus
//   goruntu DONUYORDU — zoom bolgesi kayiyor ama ekrana yansimiyordu.
//   Artik DXGICapture son goruntuyu onbellekte tuttugu icin (b) mumkun.
//
//   Ikisi de degismediyse Present'i tamamen atliyoruz: bos frame gondermek
//   GPU'yu ve vSync beklemesini bosa harciyordu.
// =============================================================================
bool App::RenderMonitor(size_t monitorIndex, bool allowVSync)
{
    if (monitorIndex >= m_captures.size())
        return false;

    MonitorInfo* mon = m_monitorManager.GetMonitor(monitorIndex);
    if (!mon)
        return false;

    auto& capture = m_captures[monitorIndex];
    if (!capture.IsInitialized())
        return false;

    // ── Frame yakala ──
    // timeout 0 = bloklamadan sor. Yeni frame yoksa onbellekteki goruntu
    // doner (isNewFrame = false), yani elimiz hicbir zaman bos kalmaz.
    const CapturedFrame frame = capture.AcquireFrame(0);
    if (!frame.texture)
        return false;   // Henuz hic goruntu yakalanmadi

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

    // ── Degisiklik yoksa cikma ──
    RECT& lastRect = m_lastSrcRect[StatusSnapshot::ClampIndex(monitorIndex)];
    if (!frame.isNewFrame && EqualRect(&lastRect, &srcRect))
        return false;

    m_renderer.RenderFrame(frame.texture.Get(), monitorIndex, srcRect);
    m_renderer.Present(monitorIndex, allowVSync);
    lastRect = srcRect;

    // ── FPS olcumu ──
    // Present'ten sonra olcuyoruz, cunku vSync bekleyisi de frame suresinin
    // parcasi. Iki render arasi sureyi 1/dt ile FPS'e ceviriyoruz.
    // Python analojisi: time.perf_counter() farki, ama steady_clock
    // monotonic garantisi veriyor (sistem saati geri alinsa bile bozulmaz).
    const auto now = std::chrono::steady_clock::now();
    auto& lastTime = m_lastFrameTime[StatusSnapshot::ClampIndex(monitorIndex)];

    if (lastTime.time_since_epoch().count() != 0)
    {
        const auto dt = std::chrono::duration<float>(now - lastTime).count();
        if (dt > 0.0f)
        {
            // Ustel yumusatma — ham 1/dt cok zipliyor, gostergede okunmaz.
            const float instant = 1.0f / dt;
            auto& fpsSlot = m_status.Monitor(monitorIndex).fps;
            const float prev = fpsSlot.load(std::memory_order_relaxed);
            const float smoothed = (prev <= 0.0f) ? instant : (prev * 0.9f + instant * 0.1f);
            fpsSlot.store(smoothed, std::memory_order_relaxed);
        }
    }
    lastTime = now;

    return true;
}

// =============================================================================
// Event Handlers
// =============================================================================

// =============================================================================
// ResolveMonitorIndex — mesaj wParam'ini monitor indeksine cevir
// =============================================================================
// kFocusedMonitor sentinel'i = "farenin uzerinde oldugu monitor".
// Diger degerler dogrudan indeks. Sinir disi indeks false doner.
// =============================================================================
bool App::ResolveMonitorIndex(WPARAM wparam, size_t& outIndex) const
{
    if (wparam == kFocusedMonitor)
    {
        POINT cursor{};
        GetCursorPos(&cursor);

        const auto index = m_monitorManager.FindIndexByPoint(cursor);
        if (!index)
            return false;

        outIndex = *index;
        return true;
    }

    if (wparam < m_monitorManager.GetMonitorCount())
    {
        outIndex = static_cast<size_t>(wparam);
        return true;
    }

    return false;
}

// Hotkey veya tray cift tik / menu → farenin uzerinde oldugu monitorde toggle.
// Windows Magnifier TUM ekranlari birlikte buyutur; bizim farkimiz bu:
// her monitorde BAGIMSIZ zoom.
void App::OnToggleZoom()
{
    POINT cursor{};
    GetCursorPos(&cursor);

    const auto index = m_monitorManager.FindIndexByPoint(cursor);
    if (!index)
    {
        LOG_WARN("Fare hicbir monitorde bulunamadi, toggle atlandi");
        return;
    }

    const size_t i = *index;
    m_monitorManager.ToggleZoom(i);

    const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
    if (mon && mon->zoom.isActive)
    {
        // Zoom acilinca hangi seviyeden baslasin?
        // rememberZoomLevel aciksa son kullanilan seviye, degilse minZoom'un
        // iki kati (1.0x'te acmak anlamsiz).
        const auto ms = m_settings.Monitor(mon->deviceName);
        const float startZoom = m_settings.General().rememberZoomLevel
            ? std::clamp(ms.lastZoom, ms.minZoom, ms.maxZoom)
            : std::clamp(ms.minZoom * 2.0f, ms.minZoom, ms.maxZoom);

        m_monitorManager.SetZoom(i, startZoom);
        m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: Aktif");
    }
    else
    {
        m_trayIcon.UpdateTooltip(L"BetterMagnifier - Zoom: Pasif");
    }
}

void App::OnFreeze()
{
    POINT cursor{};
    GetCursorPos(&cursor);

    const auto index = m_monitorManager.FindIndexByPoint(cursor);
    if (index)
        m_monitorManager.ToggleFreezeOnMonitor(*index);
}

// Mouse wheel → zoom.
//
// DIKKAT: Low-level mouse hook scroll'u ENGELLEMEZ, altindaki uygulamaya da
// gider. Bu yuzden scroll'u sadece o monitorde zoom AKTIFKEN yakaliyoruz.
// Boylece normal scroll davranisini hicbir zaman bozmuyoruz — zoom kapaliysa
// hicbir sey olmuyor.
void App::OnScroll(int delta, POINT mousePos)
{
    const auto index = m_monitorManager.FindIndexByPoint(mousePos);
    if (!index)
        return;

    const MonitorInfo* mon = m_monitorManager.GetMonitor(*index);
    if (!mon || !mon->zoom.isActive)
        return;

    const auto ms = m_settings.Monitor(mon->deviceName);
    const float step = (delta > 0) ? ms.zoomStep : -ms.zoomStep;
    m_monitorManager.AdjustZoom(*index, step);
}

// =============================================================================
// ApplySettings — ayarlar degisti, motora uygula
// =============================================================================
// GUI once SettingsStore'u yazdi SONRA bu mesaji postaladi, yani buradaki
// okuma guvenli — yaris yok.
// =============================================================================
void App::ApplySettings()
{
    LOG_INFO("Ayarlar uygulaniyor...");

    const auto& g = m_settings.General();

    // Hotkey'leri yeniden kaydet
    m_hotkeyManager.Reregister(g);

    // Per-monitor zoom sinirlarini uygula
    for (size_t i = 0; i < m_monitorManager.GetMonitorCount(); ++i)
    {
        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        const auto ms = m_settings.Monitor(mon->deviceName);

        // Mevcut zoom yeni sinirlarin disinda kaldiysa iceri cek
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

    m_monitorManager.Refresh();

    // Yikma + yeniden kurma tek yerde: ilk kurulumla ayni kod yolu.
    // Eskiden bu fonksiyon kurulumun kopyasini tasiyordu ve kopya, orijinaldeki
    // duzeltmelerden (dizi hizasi, hata loglari) habersiz kaliyordu.
    BuildPerMonitorResources();

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

    // ── Input thread'den gelen olaylar ──
    case WM_APP_SCROLL_ZOOM:
        if (s_instance)
        {
            // Konumu simdi okuyoruz — olaydan birkac ms sonra, ayni monitorde.
            // wParam'a negatif delta konulmus olabilir: WPARAM unsigned oldugu
            // icin once intptr_t'ye cevirip isareti geri kazaniyoruz.
            POINT pt{};
            GetCursorPos(&pt);
            s_instance->OnScroll(static_cast<int>(static_cast<intptr_t>(wParam)), pt);
        }
        return 0;

    // ── (Ileride) GUI thread'den gelecek olaylar ──
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

    // 2. Girdi kaynaklarini kes (hotkey) — artik callback gelmesin
    m_hotkeyManager.Shutdown();

    // 3. Tray icon'u kaldir
    m_trayIcon.Destroy();

    // 4. Ayarlari kaydet — son zoom seviyeleri dahil.
    // Monitorler hala ayakta oldugu icin overlay/capture yikimindan ONCE.
    if (m_settings.General().rememberZoomLevel)
    {
        for (size_t i = 0; i < m_monitorManager.GetMonitorCount(); ++i)
        {
            const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
            if (!mon)
                continue;

            auto ms = m_settings.Monitor(mon->deviceName);

            // Zoom kapaliyken zoomLevel 1.0'a dusuyor (ToggleZoom sifirliyor);
            // o degeri kaydetmek "son seviyeyi hatirla"yi anlamsiz kilardi.
            if (mon->zoom.isActive && mon->zoom.zoomLevel > ms.minZoom)
            {
                ms.lastZoom = mon->zoom.zoomLevel;
                m_settings.SetMonitor(mon->deviceName, ms);
            }
        }
    }
    m_settings.Save();

    // 5. Per-monitor zincir: capture → swap chain → overlay pencere
    DestroyPerMonitorResources();

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
