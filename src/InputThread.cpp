// =============================================================================
// InputThread.cpp
// =============================================================================

#include "pch.h"
#include "InputThread.h"
#include "AppMessages.h"
#include "Logger.h"

#include <future>

namespace BetterMagnifier {

InputThread* InputThread::s_instance = nullptr;

InputThread::~InputThread()
{
    Stop();
}

// =============================================================================
// Start — thread'i baslat, hook kurulumunu bekle
// =============================================================================
//
// Neden std::promise ile bekliyoruz?
//   Hook'lar thread ICINDE kurulmali (o thread'in kuyruguna baglanacaklar).
//   Ama Start()'in cagirana "hook'lar hazir" veya "kurulamadi" demesi lazim.
//   promise/future tam bu is icin: thread sonucu yaziyor, Start okuyor.
//
// Python analojisi: concurrent.futures.Future, ya da threading.Event +
// bir sonuc degiskeni.
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
    LOG_INFO("  Mouse hook aktif (input thread'de)");

    // ── Klavye hook'u (Win+Z ele gecirme) ──
    // Her zaman kuruyoruz ama sadece hijackMagnifierKeys acikken olay yutuyoruz.
    // Kur/kaldir yapmaktansa atomic bayrak okumak hem ucuz hem yaris kosulsuz.
    // Basarisiz olursa kritik degil — sadece Win+arti/eksi devralinmaz.
    m_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        GetModuleHandleW(nullptr),
        0);

    if (!m_keyboardHook)
        LOG_WARN("WH_KEYBOARD_LL kurulamadi: {} — Win+arti/eksi devralma devre disi",
            GetLastError());
    else
        LOG_INFO("  Klavye hook'u aktif");

    // ── Klavye odagi hook'u ──
    // SetWindowEventHook(EVENT_OBJECT_FOCUS): sistemde odak degisince haber verir.
    // WINEVENT_OUTOFCONTEXT: callback BIZIM thread'imizde cagrilir (DLL
    // enjeksiyonu yok) — bu yuzden bu thread'in mesaj loop'u olmak zorunda.
    // WINEVENT_SKIPOWNPROCESS: kendi pencerelerimiz (overlay, panel) tetiklemesin.
    m_focusHook = SetWinEventHook(
        EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS,
        nullptr,
        WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!m_focusHook)
        LOG_WARN("EVENT_OBJECT_FOCUS hook kurulamadi: {} — klavye odagi takibi devre disi",
            GetLastError());
    else
        LOG_INFO("  Klavye odagi hook'u aktif");

    return true;
}

void InputThread::RemoveHooks()
{
    if (m_focusHook)
    {
        UnhookWinEvent(m_focusHook);
        m_focusHook = nullptr;
        LOG_DEBUG("Klavye odagi hook'u kaldirildi");
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
// ThreadMain — hook'larin yasamasi icin gereken mesaj loop'u
// =============================================================================
// Bu loop hicbir pencereye ait degil (thread-only mesajlar). Tek isi hook
// callback'lerinin cagrilabilmesi icin thread'i "mesaj pompalayan" halde tutmak.
//
// GetMessage kullaniyoruz, PeekMessage DEGIL: burada render yapmiyoruz,
// bloklamak dogrusu — CPU %0.
// =============================================================================
void InputThread::ThreadMain()
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
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
        LOG_INFO("Magnifier kisayol devralma {}{}",
            enable ? "ACIK" : "KAPALI",
            enable ? " — Win+arti/eksi, Ctrl+Alt+tekerlek, Win+orta tik bize geliyor"
                   : "");
    }
}

// =============================================================================
// LowLevelMouseProc — HIZLI DONMELI
// =============================================================================
//
// Bu callback sistemdeki her fare olayinda cagriliyor. Icinde is yapmak yasak:
// sadece ilgilendigimiz olayi PostMessage ile render thread'e atip donuyoruz.
//
// PostMessage (SendMessage DEGIL) kritik: SendMessage hedef thread'in mesaji
// ISLEMESINI bekler — render thread Present'te blokluysa bu hook'u kilitler
// ve LowLevelHooksTimeout'a takilir.
// =============================================================================
LRESULT CALLBACK InputThread::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && s_instance->m_target &&
        s_instance->m_hijackMagnifierKeys.load(std::memory_order_relaxed))
    {
        auto* data = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // ── Ctrl+Alt+tekerlek = zoom adimi ──
        //
        // Windows Magnifier'in kendi kombinasyonu, ondan deviraliyoruz.
        //
        // NEDEN DUZ TEKERLEK DEGIL: hook olayi yutmadigi surece alttaki
        // uygulamaya da gidiyor. Duz tekerlekle zoom yapinca sayfa hem
        // zoom'laniyor hem kayiyordu. Ctrl+Alt+tekerlegi YUTARAK aliyoruz,
        // boylece cift etki bitiyor ve duz tekerlek normal kaydirmaya donuyor.
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
        // Elin zaten farede, klavyeye gitmiyorsun. MBUTTONUP'i da yutuyoruz,
        // yoksa alttaki uygulama yarim bir orta tiklama gorur (bazi
        // tarayicilarda bu yeni sekme acar).
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

    // Chain'i MUTLAKA devam ettir — yoksa diger uygulamalar fare olaylarini
    // alamaz. return 1 sadece yukarida, olayi bilincli yuttugumuz yerlerde.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// =============================================================================
// LowLevelKeyboardProc — Windows Magnifier kisayollarini devral
// =============================================================================
//
// return 1 = olayi YUT (chain'e gitmez, Windows gormez).
//
// NEDEN HOOK, RegisterHotKey DEGIL:
//   Win+arti / Win+eksi Windows Magnifier'a rezerve. RegisterHotKey bunlari
//   ALAMAZ, basarisiz doner. Sistem kisayolunu gercekten devralmanin tek yolu
//   low-level hook'ta olayi yutmak.
//
// DEVRALINANLAR (Win basiliyken):
//   VK_OEM_PLUS  / VK_ADD      -> zoom adim +  (zoom kapaliysa ACAR)
//   VK_OEM_MINUS / VK_SUBTRACT -> zoom adim -  (minZoom'a inince KAPATIR)
//
//   Hem ana klavye sirasi (OEM_*) hem numpad (ADD/SUBTRACT): Windows
//   Magnifier ikisini de kabul ediyor, biz de edelim.
//
// NE YUTMUYORUZ:
//   Win tusunun KENDISINI. Yutsak Start menusu, Win+D, Win+E hepsi bozulur.
//   Sadece Win basiliyken ilgili tusun KeyDown'unu yutuyoruz.
//
// KAYBEDILEN: hijack acikken Windows'un kendi Magnifier'i bu tuslarla
// acilmaz. Istenen davranis tam olarak bu.
//
// YUTAMADIKLARIMIZ (kernel/Winlogon korumali, kod ile engellenemez):
//   Ctrl+Alt+Del, Win+L. Mimari olarak erisimimizin disinda.
//
// ADMIN NOTU: yuksek integrity'li pencere odaktayken (Task Manager, UAC) hook
// devreye girmez. DXGI Desktop Duplication da secure desktop'ta calismadigi
// icin sinir zaten orada — ek kisit getirmiyor.
// =============================================================================
LRESULT CALLBACK InputThread::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && s_instance->m_target &&
        s_instance->m_hijackMagnifierKeys.load(std::memory_order_relaxed) &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (kb)
        {
            // Win tusu basili mi? 0x8000 biti = su an basili.
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

            // ── PANIK CIKISI: Ctrl+Alt+Shift+Q ──
            //
            // Neden gerekli: overlay tam ekran, topmost ve opak. Render thread
            // herhangi bir sebeple bloklanirsa mesaj pompalamayi birakir,
            // WM_HOTKEY islenmez ve kullanici ekranin arkasinda mahsur kalir —
            // gorev yoneticisinden kapatmak zorunda kalindi, bir kez oldu.
            //
            // Bu tus INPUT thread'de isleniyor; o thread hicbir zaman
            // bloklanmiyor (hook callback'leri sadece PostMessage edip donuyor).
            // Yani render thread olmus olsa bile bu yol calisir.
            //
            // NEDEN ShowWindow ILE OVERLAY'LERI GIZLEMIYORUZ: pencereler render
            // thread'e ait. Baska thread'den gizlemek o thread'in isbirligini
            // gerektiriyor — wedge durumunda tam da olmayan sey bu.
            // Kesin calisan tek sey process'i bitirmek.
            if (kb->vkCode == 'Q'
                && (GetAsyncKeyState(VK_CONTROL) & 0x8000)
                && (GetAsyncKeyState(VK_MENU)    & 0x8000)
                && (GetAsyncKeyState(VK_SHIFT)   & 0x8000))
            {
                // Iki kez tetiklenmesin (tus tekrari)
                static std::atomic<bool> panicStarted{false};
                if (!panicStarted.exchange(true, std::memory_order_relaxed))
                {
                    LOG_WARN("PANIK CIKISI (Ctrl+Alt+Shift+Q) — nazik kapatma deneniyor");

                    // 1. Nazik yol: mesaj penceresine WM_CLOSE.
                    PostMessageW(s_instance->m_target, WM_CLOSE, 0, 0);

                    // 2. Hook icinde BEKLEMEK YASAK (LowLevelHooksTimeout).
                    //    Ayri, detached bir thread bekleyip gerekirse zorlar.
                    std::thread([]{
                        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                        LOG_ERROR("Nazik kapatma 1.5 sn'de tamamlanmadi — TerminateProcess");
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
    if (event != EVENT_OBJECT_FOCUS || !hwnd)
        return;

    // idObject == OBJID_CLIENT: gercek bir kontrol odaklandi.
    // Menu/scrollbar/caret gibi alt nesneleri yoksayiyoruz — zoom bolgesini
    // her scrollbar tiklamasinda ziplatmak istemiyoruz.
    if (idObject != OBJID_CLIENT)
        return;

    if (!s_instance || !s_instance->m_target)
        return;

    if (s_instance->m_followMode.load(std::memory_order_relaxed) != FollowMode::MouseAndFocus)
        return;

    PostMessageW(s_instance->m_target, WM_APP_FOCUS_CHANGED, 0,
                 reinterpret_cast<LPARAM>(hwnd));
}

} // namespace BetterMagnifier
