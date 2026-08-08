#pragma once

// =============================================================================
// App.h — Ana Uygulama Sinifi (Orkestrator)
// =============================================================================
// Tum component'lari (MonitorManager, DXGICapture, D3DRenderer, Overlay,
// HotkeyManager, TrayIcon, InputThread) init eden ve yoneten RAII sinifi.
//
// THREAD: Bu sinif RENDER thread'inde yasar. Input thread ve (ileride) GUI
// thread ona sadece mesaj penceresine PostMessage ederek ulasir; motordan
// disari veri akisi ise StatusSnapshot uzerinden lock-free gider.
// =============================================================================

#ifndef BETTER_MAGNIFIER_APP_H
#define BETTER_MAGNIFIER_APP_H

#include "MonitorManager.h"
#include "DXGICapture.h"
#include "D3DRenderer.h"
#include "OverlayWindow.h"
#include "HotkeyManager.h"
#include "TrayIcon.h"
#include "SettingsStore.h"
#include "InputThread.h"
#include "StatusSnapshot.h"
#include "AppMessages.h"

#include <windows.h>
#include <vector>
#include <array>
#include <chrono>
#include <memory>

namespace BetterMagnifier {

class App
{
public:
    App() = default;
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ── Lifecycle ──
    bool Initialize(HINSTANCE hInstance);
    int  Run();
    void Shutdown();

    // GUI thread bu pointer'lari okur. App yasadigi surece gecerli.
    StatusSnapshot* Status()   { return &m_status; }
    SettingsStore*  Settings() { return &m_settings; }

private:
    // ── Internal Setup ──
    bool CreateMessageWindow();
    bool InitializeComponents();
    void SetupCallbacks();

    // Per-monitor kaynak zinciri (overlay + swap chain + capture).
    // Hem ilk kurulumda hem WM_DISPLAYCHANGE sonrasinda ayni kod calisir.
    void BuildPerMonitorResources();
    void DestroyPerMonitorResources();

    // ── Per-Frame ──
    void Update();

    // Tek monitoru render et. true doner = bu frame'de gercekten Present edildi.
    // allowVSync: bu frame'de vSync'e girme hakki (bkz. Update icindeki aciklama).
    bool RenderMonitor(size_t monitorIndex, bool allowVSync);

    // Mesaj bekleyerek uyu. Sleep yerine bunu kullaniyoruz: mesaj gelir gelmez
    // uyaniyoruz, bos beklemede CPU yakmiyoruz.
    static void WaitForInput(DWORD timeoutMs);

    // ── Event Handlers ──
    void OnToggleZoom();
    void OnFreeze();
    void OnScroll(int delta, POINT mousePos);
    void OnDisplayChange();

    // Ayarlar degistiginde uygula (WM_APP_SETTINGS_CHANGED)
    void ApplySettings();

    // Mesaj wParam'ini monitor indeksine cevir (kFocusedMonitor destekli)
    bool ResolveMonitorIndex(WPARAM wparam, size_t& outIndex) const;

    // ── Message Window WndProc ──
    static LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ── Components ──
    HINSTANCE                   m_hInstance = nullptr;
    HWND                        m_messageHwnd = nullptr;

    SettingsStore               m_settings;
    MonitorManager              m_monitorManager;
    D3DRenderer                 m_renderer;
    HotkeyManager               m_hotkeyManager;
    TrayIcon                    m_trayIcon;
    InputThread                 m_inputThread;
    std::vector<DXGICapture>    m_captures;
    std::vector<OverlayWindow>  m_overlays;

    // ── GUI'ye acilan canli durum ──
    StatusSnapshot              m_status;

    // FPS olcumu — monitor basina son frame zamani
    std::array<std::chrono::steady_clock::time_point, StatusSnapshot::kMaxMonitors> m_lastFrameTime{};

    // Son render edilen kirpma bolgesi. Yeni frame YOKSA bile bu bolge
    // degistiyse yeniden render etmemiz gerekiyor (fare hareket etti, masaustu
    // sabit). Sifir RECT = "bu monitorde henuz render yapilmadi".
    std::array<RECT, StatusSnapshot::kMaxMonitors> m_lastSrcRect{};

    bool m_running     = false;
    bool m_initialized = false;

    // Static instance for WndProc
    static App* s_instance;

    static constexpr wchar_t kMsgWindowClass[] = L"BetterMagnifierMsg";

    // Aktif monitor yokken mesaj bekleme suresi (ms). Uzun tutuyoruz —
    // gercek bir olay gelirse zaten aninda uyaniyoruz.
    static constexpr DWORD kIdleWaitMs = 100;

    // Zoom acik ama hicbir sey degismediginde bekleme suresi (ms).
    // Kisa: fare hareketine gecikmesiz cevap vermeliyiz.
    static constexpr DWORD kActiveIdleWaitMs = 4;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_APP_H
