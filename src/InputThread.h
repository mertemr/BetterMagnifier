#pragma once

// =============================================================================
// InputThread.h — Low-level hook'lar icin ayri thread
// =============================================================================
//
// NEDEN AYRI THREAD (bu dosyanin butun varlik sebebi):
//   Low-level hook'lar (WH_MOUSE_LL, WH_KEYBOARD_LL) onlari KURAN thread'in
//   mesaj kuyrugunda cagrilir. Render thread'imiz Present(vSync) ile bir
//   frame boyunca blokluyor. Hook orada olursa sistemdeki HER fare/tus olayi
//   bizim frame'imizin arkasinda bekler — makinede her yerde girdi gecikmesi.
//
//   Ustune Windows'un LowLevelHooksTimeout'u (varsayilan 300 ms) asilirsa
//   hook'u sessizce devre disi birakiyor.
//
//   Bu thread hicbir agir is yapmaz: hook callback'i sadece PostMessage eder
//   ve doner. Gercek isi render thread yapar.
//
// Python analojisi: pynput listener'ini ayri bir thread'de calistirmak.
// Fark: Win32'de hook'un yasadigi thread'in GetMessage loop'u olmak ZORUNDA,
// yoksa callback hic cagrilmaz.
// =============================================================================

#ifndef BETTER_MAGNIFIER_INPUT_THREAD_H
#define BETTER_MAGNIFIER_INPUT_THREAD_H

#include "SettingsStore.h"   // FollowMode

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
    //
    // targetHwnd  : olaylarin PostMessage ile gonderilecegi pencere (mesaj penceresi)
    // initialMode : klavye odagi takibi aktif mi
    // hijackWinZ  : Win+Z'yi yut ve kendi toggle'imiza cevir
    //
    // Hook'lar thread ICINDE kurulur; Start() donmeden once kurulum
    // tamamlanmis olur (promise/future ile senkron bekleme).
    bool Start(HWND targetHwnd, FollowMode initialMode, bool hijackWinZ);

    // Thread'e WM_QUIT postala, hook'lari kaldir, join et. Idempotent.
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

    // ── Canli ayar degisimi (thread-safe, atomic) ──
    // Hook'lari kur/kaldir yapmaktansa atomic bayrak okumak hem ucuz
    // hem yaris kosulsuz.
    void SetFollowMode(FollowMode mode);
    void SetHijackWinZ(bool enable);

private:
    void ThreadMain();
    bool InstallHooks();
    void RemoveHooks();

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild,
                                      DWORD idEventThread, DWORD dwmsEventTime);

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};

    // Thread'e mesaj postalamak icin — Stop() bunu kullanir
    std::atomic<DWORD> m_threadId{0};

    std::atomic<FollowMode> m_followMode{FollowMode::MouseAndFocus};
    std::atomic<bool>       m_hijackWinZ{false};

    HWND          m_target       = nullptr;
    HHOOK         m_mouseHook    = nullptr;
    HHOOK         m_keyboardHook = nullptr;
    HWINEVENTHOOK m_focusHook    = nullptr;

    // Win32 hook callback'leri static olmak zorunda (calling convention).
    // Bu yuzden global instance pointer'i tutuyoruz. Uygulamada tek
    // InputThread var ve olmasi da gerekmiyor.
    static InputThread* s_instance;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_INPUT_THREAD_H
