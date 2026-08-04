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
bool InputThread::Start(HWND targetHwnd, FollowMode initialMode, bool hijackWinZ)
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
    m_hijackWinZ.store(hijackWinZ, std::memory_order_relaxed);
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
    // Her zaman kuruyoruz ama sadece hijackWinZ acikken olay yutuyoruz.
    // Kur/kaldir yapmaktansa atomic bayrak okumak hem ucuz hem yaris kosulsuz.
    // Basarisiz olursa kritik degil — sadece Win+Z ozelligi calismaz.
    m_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        GetModuleHandleW(nullptr),
        0);

    if (!m_keyboardHook)
        LOG_WARN("WH_KEYBOARD_LL kurulamadi: {} — Win+Z ele gecirme devre disi",
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

void InputThread::SetHijackWinZ(bool enable)
{
    const bool prev = m_hijackWinZ.exchange(enable, std::memory_order_relaxed);

    if (prev != enable)
    {
        LOG_INFO("Win+Z ele gecirme {}{}",
            enable ? "ACIK" : "KAPALI",
            enable ? " — Windows Snap Layouts devre disi" : "");
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
    if (nCode == HC_ACTION && s_instance && s_instance->m_target && wParam == WM_MOUSEWHEEL)
    {
        auto* data = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (data)
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(data->mouseData);

            // Konumu gondermiyoruz — render thread GetCursorPos() ile kendisi
            // okuyor. Olay ile isleme arasi birkac ms, fare ayni monitorde kalir.
            PostMessageW(s_instance->m_target, WM_APP_SCROLL_ZOOM,
                         static_cast<WPARAM>(delta), 0);
        }
    }

    // Chain'i MUTLAKA devam ettir — yoksa diger uygulamalar fare olaylarini
    // alamaz. return 1 sadece olayi YUTMAK istedigimizde (klavyede Win+Z).
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// =============================================================================
// LowLevelKeyboardProc — Win+Z'yi yut
// =============================================================================
//
// return 1 = olayi YUT (chain'e gitmez, Windows gormez).
// RegisterHotKey'in yapamadigi seyi yapmanin tek yolu bu: Win+Z Windows 11'de
// Snap Layouts'a rezerve, RegisterHotKey basarisiz doner.
//
// NE YUTUYORUZ, NE YUTMUYORUZ:
//   Sadece Win basiliyken Z'nin KeyDown'unu yutuyoruz. Win tusunun KENDISINI
//   yutmuyoruz — yutsak Start menusu, Win+D, Win+E hepsi bozulur.
//
// KAYBEDILEN: hijack acikken Windows 11 Snap Layouts calismaz. Varsayilan
// KAPALI olmasinin sebebi bu.
//
// YUTAMADIKLARIMIZ (kernel/Winlogon korumali, kod ile engellenemez):
//   Ctrl+Alt+Del, Win+L. Bunlar mimari olarak erisimimizin disinda.
//
// ADMIN NOTU: yuksek integrity'li pencere odaktayken (Task Manager, UAC) hook
// devreye girmez. DXGI Desktop Duplication da secure desktop'ta calismadigi
// icin sinir zaten orada — ek kisit getirmiyor.
// =============================================================================
LRESULT CALLBACK InputThread::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance &&
        s_instance->m_hijackWinZ.load(std::memory_order_relaxed))
    {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (kb && kb->vkCode == 'Z' &&
            (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
        {
            // Win tusu basili mi? 0x8000 biti = su an basili.
            const bool winDown =
                (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

            if (winDown && s_instance->m_target)
            {
                PostMessageW(s_instance->m_target, WM_APP_TOGGLE_ZOOM, kFocusedMonitor, 0);

                // YUT — Snap Layouts bu tusu gormesin.
                return 1;
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
