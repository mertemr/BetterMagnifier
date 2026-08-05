// =============================================================================
// OverlayWindow.cpp — Per-Monitor Transparent Overlay Implementation
// =============================================================================

#include "pch.h"
#include "OverlayWindow.h"
#include "MonitorManager.h"
#include "Logger.h"

namespace BetterMagnifier {

bool OverlayWindow::s_classRegistered = false;

// =============================================================================
// Destructor
// =============================================================================
OverlayWindow::~OverlayWindow()
{
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

// =============================================================================
// Move Semantics
// =============================================================================
OverlayWindow::OverlayWindow(OverlayWindow&& other) noexcept
    : m_hwnd(other.m_hwnd)
    , m_monitorIndex(other.m_monitorIndex)
    , m_visible(other.m_visible)
    , m_excludedFromCapture(other.m_excludedFromCapture)
{
    other.m_hwnd = nullptr;
    other.m_visible = false;
    other.m_excludedFromCapture = false;
}

OverlayWindow& OverlayWindow::operator=(OverlayWindow&& other) noexcept
{
    if (this != &other)
    {
        if (m_hwnd) DestroyWindow(m_hwnd);
        m_hwnd                = other.m_hwnd;
        m_monitorIndex        = other.m_monitorIndex;
        m_visible             = other.m_visible;
        m_excludedFromCapture = other.m_excludedFromCapture;
        other.m_hwnd    = nullptr;
        other.m_visible = false;
        other.m_excludedFromCapture = false;
    }
    return *this;
}

// =============================================================================
// RegisterWindowClass — Overlay icin window class kaydi
// =============================================================================
bool OverlayWindow::RegisterWindowClass(HINSTANCE hInstance)
{
    if (s_classRegistered)
        return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;           // Arka plan yok (seffaf)
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc))
    {
        LOG_ERROR("Overlay window class kaydi basarisiz: {}", GetLastError());
        return false;
    }

    s_classRegistered = true;
    LOG_DEBUG("Overlay window class kaydedildi");
    return true;
}

// =============================================================================
// Create — Overlay penceresi olustur
// =============================================================================
//
// NEDEN WS_EX_LAYERED SART (onemli mimari karar, bir kez YANLIS yapildi):
//   Bir ara flip-model swap chain'i calistirmak icin WS_EX_LAYERED kaldirildi
//   ve "tam ekran magnifier'a seffaflik gerekmez" diye gerekcelendirildi.
//   Gorsel olarak dogruydu, GIRDI olarak yikiciydi: overlay butun tiklamalari
//   yutmaya basladi, zoom acikken Windows kullanilamaz hale geldi.
//
//   Sebep: WS_EX_TRANSPARENT tek basina ve WM_NCHITTEST'ten HTTRANSPARENT
//   dondurmek click-through icin YETMIYOR. Guvenilir recete LAYERED |
//   TRANSPARENT ikilisi.
//
//   Bedeli: flip-model layered pencereyi reddediyor, blt modeline dusuyoruz
//   (bkz. D3DRenderer::CreateSwapChainForWindow). Latency biraz artiyor ama
//   calisan bir girdi, daha hizli bir kullanilamazliktan iyidir.
//
//   SetLayeredWindowAttributes ile layered yapilan pencere normal redirection
//   surface'ini KORUR, yani D3D cizimi calisir. UpdateLayeredWindow yolu
//   olsaydi calismazdi — aradaki fark kritik.
//
// Window Styles Aciklamasi:
//   WS_EX_LAYERED      — TRANSPARENT ile birlikte gercek click-through saglar
//   WS_EX_TRANSPARENT  — Mouse olaylari bu pencereden gecer (click-through)
//   WS_EX_TOPMOST      — Her zaman ustte (diger pencerelerin uzerinde)
//   WS_EX_NOACTIVATE   — Tiklayinca focus almasin (calisan uygulama focus'unu kaybetmesin)
//   WS_EX_TOOLWINDOW   — Taskbar'da gorunmesin
//   WS_POPUP           — Title bar, kenar cizgisi yok (tam seffaf)
//
// Python analojisi:
//   tkinter: root.attributes("-alpha", 0.0, "-topmost", True)
//   PyQt: Qt.WindowStaysOnTopHint | Qt.FramelessWindowHint | Qt.WindowTransparentForInput
//
// =============================================================================
bool OverlayWindow::Create(HINSTANCE hInstance, const MonitorInfo& monitorInfo, size_t monitorIndex)
{
    if (!RegisterWindowClass(hInstance))
        return false;

    m_monitorIndex = monitorIndex;

    DWORD exStyle = WS_EX_TRANSPARENT
                  | WS_EX_TOPMOST
                  | WS_EX_NOACTIVATE
                  | WS_EX_TOOLWINDOW;

    // Layered mod (varsayilan): click-through icin sart.
    // BM_OVERLAY_FLIP=1 ile eski flip-model davranisina donulur — girdi
    // gecmez, sadece karsilastirma icin.
    if (!UseFlipOverlay())
        exStyle |= WS_EX_LAYERED;

    DWORD style = WS_POPUP;

    int x = monitorInfo.bounds.left;
    int y = monitorInfo.bounds.top;
    int w = monitorInfo.Width();
    int h = monitorInfo.Height();

    m_hwnd = CreateWindowExW(
        exStyle,
        kClassName,
        L"BetterMagnifier Overlay",
        style,
        x, y, w, h,
        nullptr,        // Parent window yok
        nullptr,        // Menu yok
        hInstance,
        this            // WndProc'a "this" gec (CREATESTRUCT.lpCreateParams)
    );

    if (!m_hwnd)
    {
        LOG_ERROR("Overlay window olusturulamadi (monitor {}): {}", monitorIndex, GetLastError());
        return false;
    }

    // ── Layered pencereyi tam opak yap ──
    // Layered olmasinin sebebi girdi seffafligi, GORSEL seffaflik degil —
    // alpha 255 (tam opak). Bu cagri olmadan layered pencere HIC cizilmez.
    if (!UseFlipOverlay())
    {
        if (!SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA))
        {
            LOG_ERROR("SetLayeredWindowAttributes basarisiz: {} — overlay gorunmeyebilir",
                GetLastError());
        }
    }

    // ── FEEDBACK LOOP ONLEME (kritik!) ──
    // Desktop Duplication tum masaustunu yakaliyor — overlay de masaustunun
    // parcasi. Onlem alinmazsa overlay kendi icerigini yakalar → sonsuz ayna
    // (kamerayi kendi ekranina tutmak gibi).
    //
    // WDA_EXCLUDEFROMCAPTURE (Windows 10 2004+): pencere kullaniciya normal
    // gorunur ama ekran yakalama API'lerine gorunmez. Tam istedigimiz sey.
    // Eski Windows'ta basarisiz olur — o zaman feedback loop olusur, uyari veriyoruz.
    if (SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE))
    {
        m_excludedFromCapture = true;
    }
    else
    {
        m_excludedFromCapture = false;
        LOG_WARN("SetWindowDisplayAffinity basarisiz ({}) — Windows 10 2004+ gerekiyor, "
                 "feedback loop olusabilir", GetLastError());
    }

    LOG_INFO("Overlay window olusturuldu: monitor={}, pos=({},{}), size={}x{}, HWND=0x{:X}",
        monitorIndex, x, y, w, h, reinterpret_cast<uintptr_t>(m_hwnd));

    return true;
}

// =============================================================================
// Show/Hide
// =============================================================================
void OverlayWindow::Show()
{
    if (m_hwnd && !m_visible)
    {
        // SW_SHOWNOACTIVATE: goster ama focus'u CALMA — altta calisan
        // uygulamanin klavye odagi bozulmasin.
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        m_visible = true;
        LOG_DEBUG("Overlay gorunur: monitor={}", m_monitorIndex);
    }
}

void OverlayWindow::Hide()
{
    if (m_hwnd && m_visible)
    {
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
        LOG_DEBUG("Overlay gizlendi: monitor={}", m_monitorIndex);
    }
}

bool OverlayWindow::IsVisible() const
{
    return m_visible;
}

// =============================================================================
// EnsureTopmost — menulerin uzerinde kal
// =============================================================================
void OverlayWindow::EnsureTopmost()
{
    if (!m_hwnd || !m_visible)
        return;

    // SWP_NOACTIVATE: focus calmayacagiz.
    // SWP_NOMOVE | SWP_NOSIZE: sadece z-order degisiyor, geometri sabit.
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// =============================================================================
// Reposition — DPI veya resolution degistiginde
// =============================================================================
void OverlayWindow::Reposition(const RECT& bounds)
{
    if (m_hwnd)
    {
        int w = bounds.right - bounds.left;
        int h = bounds.bottom - bounds.top;
        SetWindowPos(m_hwnd, HWND_TOPMOST,
            bounds.left, bounds.top, w, h,
            SWP_NOACTIVATE | SWP_NOZORDER);

        LOG_DEBUG("Overlay repositioned: monitor={}, {}x{} @ ({},{})",
            m_monitorIndex, w, h, bounds.left, bounds.top);
    }
}

// =============================================================================
// WndProc — Overlay penceresi mesaj isleyicisi
// =============================================================================
//
// HTTRANSPARENT: Mouse olaylarini bu pencereye DEG1L, altindaki pencereye ilet.
// Bu olmadan overlay tum mouse tiklamalarini yutar!
//
// WM_DPICHANGED: Monitor DPI'i degistiginde Windows bunu gonderir.
// Overlay'in boyutunu ve pozisyonunu guncelleriz.
//
// =============================================================================
LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_NCHITTEST:
        // KRITIK: Mouse olaylari altindaki pencereye gider.
        // Bu olmadan zoom aktifken masaustune tiklanamaz!
        return HTTRANSPARENT;

    case WM_DPICHANGED:
    {
        // Yeni boyutu al ve pencereyi guncelle
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested)
        {
            SetWindowPos(hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_DESTROY:
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace BetterMagnifier
