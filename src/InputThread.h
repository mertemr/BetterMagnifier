#pragma once

// =============================================================================
// InputThread.h — Low-level hook'lar icin ayri thread
// =============================================================================
//
// NEDEN AYRI THREAD (bu dosyanin butun varlik sebebi):
//   Low-level hook'lar (WH_MOUSE_LL, WH_KEYBOARD_LL) onlari KURAN thread'in
//   mesaj kuyrugunda cagrilir. Render thread'imiz Present(vSync) ile bir frame
//   boyunca blokluyor. Hook orada olursa sistemdeki HER fare/tus olayi bizim
//   frame'imizin arkasinda bekler — makinede her yerde girdi gecikmesi.
//
//   Ustune Windows'un LowLevelHooksTimeout'u (varsayilan 300 ms) asilirsa
//   hook'u sessizce devre disi birakiyor.
//
//   Bu thread hicbir agir is yapmaz: hook callback'i sadece PostMessage eder
//   ve doner. Gercek isi render thread yapar.
//
// Python analojisi: pynput'un listener'ini ayri bir thread'de calistirmak.
// Fark: Win32'de hook'un yasadigi thread'in GetMessage loop'u olmak ZORUNDA,
// yoksa callback hic cagrilmaz.
// =============================================================================

#ifndef BETTER_MAGNIFIER_INPUT_THREAD_H
#define BETTER_MAGNIFIER_INPUT_THREAD_H

#include <windows.h>
#include <thread>
#include <atomic>

namespace BetterMagnifier {

class InputThread
{
public:
    InputThread() = default;
    ~InputThread();

    InputThread(const InputThread&) = delete;
    InputThread& operator=(const InputThread&) = delete;

    // Thread'i baslat ve hook'lari kur.
    // targetHwnd: olaylarin PostMessage ile gonderilecegi pencere (mesaj penceresi).
    // Hook'lar thread ICINDE kurulur — Start() donmeden once kurulum
    // tamamlanmis olur (senkron bekleme var).
    bool Start(HWND targetHwnd);

    // Thread'e WM_QUIT postala, hook'lari kaldir, join et.
    // Idempotent — iki kez cagirmak guvenli.
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    void ThreadMain();
    bool InstallHooks();
    void RemoveHooks();

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};

    // Thread'e mesaj postalamak icin — Stop() bunu kullanir
    std::atomic<DWORD> m_threadId{0};

    HWND   m_target     = nullptr;
    HHOOK  m_mouseHook  = nullptr;

    // Win32 hook callback'leri static olmak zorunda (calling convention).
    // Bu yuzden global instance pointer'i tutuyoruz.
    // Tek InputThread varsayimi — birden fazla olursa bu kirilir, ama
    // uygulamada tek tane var ve olmasi da gerekmiyor.
    static InputThread* s_instance;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_INPUT_THREAD_H
