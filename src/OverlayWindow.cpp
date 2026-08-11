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
// RegisterWindowClass — window class for the overlay
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
// Create — the per-monitor overlay window
// =============================================================================
//
// WS_EX_LAYERED is mandatory, and this was got wrong once. It was dropped so a
// flip-model swap chain could be used, justified as "a fullscreen magnifier
// does not need transparency". Visually true, catastrophic for input: the
// overlay swallowed every click and Windows became unusable while zoomed.
//
// WS_EX_TRANSPARENT alone is not enough, and neither is returning HTTRANSPARENT
// from WM_NCHITTEST. LAYERED | TRANSPARENT together is the recipe that works.
//
// The cost is real: flip model refuses layered windows, so the swap chain falls
// back to blt (see D3DRenderer::CreateSwapChainForWindow). Slightly more
// latency, in exchange for input that works at all.
//
// One subtlety worth keeping: a window made layered via
// SetLayeredWindowAttributes keeps its normal redirection surface, so D3D
// rendering into it works. The UpdateLayeredWindow route would not.
//
//   WS_EX_LAYERED      with TRANSPARENT, gives real click-through
//   WS_EX_TRANSPARENT  mouse events pass through to what is underneath
//   WS_EX_TOPMOST      above ordinary windows
//   WS_EX_NOACTIVATE   never takes focus from the app being magnified
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

    // Layered by default; BM_OVERLAY_FLIP=1 restores the flip-model behaviour,
    // which has lower latency and no click-through. Kept for comparison only.
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

    // Fully opaque. The window is layered for input transparency, not visual
    // transparency — and without this call a layered window is never drawn at
    // all.
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
    // WDA_EXCLUDEFROMCAPTURE (Windows 10 2004+): visible on screen, invisible
    // to capture APIs — exactly what is needed here. Without it Desktop
    // Duplication would capture our own output and feed it back.
    if (SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE))
    {
        m_excludedFromCapture = true;
    }
    else
    {
        m_excludedFromCapture = false;
        LOG_WARN("SetWindowDisplayAffinity failed ({}), needs Windows 10 2004+ — "
                 "capture feedback is possible", GetLastError());
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

    // Z-order only: NOMOVE and NOSIZE keep the geometry, NOACTIVATE keeps the
    // focus where the user put it.
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
