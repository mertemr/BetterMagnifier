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
//   Ama Start()'in cagiran tarafa "hook'lar hazir" veya "kurulamadi" demesi
//   lazim. promise/future tam bu is icin: thread sonucu yaziyor, Start okuyor.
//
// Python analojisi: threading.Event() + bir sonuc degiskeni, ya da
// concurrent.futures.Future.
// =============================================================================
bool InputThread::Start(HWND targetHwnd)
{
    if (m_running.load(std::memory_order_acquire))
        return true;

    if (!targetHwnd)
    {
        LOG_ERROR("InputThread::Start — targetHwnd null!");
        return false;
    }

    m_target   = targetHwnd;
    s_instance = this;

    std::promise<bool> ready;
    std::future<bool> readyFuture = ready.get_future();

    m_thread = std::thread([this, p = std::move(ready)]() mutable {
        m_threadId.store(GetCurrentThreadId(), std::memory_order_release);

        const bool ok = InstallHooks();

        // Sonucu yazmadan ONCE running'i isaretle: Start() donduğu anda
        // IsRunning() dogru cevap versin.
        if (ok)
            m_running.store(true, std::memory_order_release);

        p.set_value(ok);

        if (!ok)
            return;

        ThreadMain();

        m_running.store(false, std::memory_order_release);
        RemoveHooks();
    });

    const bool ok = readyFuture.get();
    if (!ok)
    {
        LOG_ERROR("InputThread hook kurulumu basarisiz");
        if (m_thread.joinable())
            m_thread.join();
        m_threadId.store(0, std::memory_order_release);
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
    return true;
}

void InputThread::RemoveHooks()
{
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
//
// Bu loop hicbir pencereye ait degil (thread-only mesajlar). Tek isi
// hook callback'lerinin cagrilabilmesi icin thread'i "mesaj pompalayan"
// halde tutmak. GetMessage bloklar — CPU %0.
//
// PeekMessage KULLANMIYORUZ: burada render yapmiyoruz, bloklamak dogrusu.
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
    m_target   = nullptr;
    s_instance = nullptr;
}

// =============================================================================
// LowLevelMouseProc — HIZLI DONMELI
// =============================================================================
//
// Bu callback sistemdeki her fare olayinda cagriliyor. Icinde is yapmak
// yasak: sadece ilgilendigimiz olayi PostMessage ile render thread'e atip
// hemen donuyoruz.
//
// PostMessage (SendMessage DEGIL) kullanmak kritik: SendMessage hedef
// thread'in mesaji islemesini BEKLER — render thread Present'te blokluysa
// bu hook'u kilitler ve LowLevelHooksTimeout'a takilir.
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
    // alamaz. return 1 sadece olayi YUTMAK istedigimizde (Win+Z ele gecirme).
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace BetterMagnifier
