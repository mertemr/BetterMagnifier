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
{
    other.m_hwnd = nullptr;
    other.m_visible = false;
}

OverlayWindow& OverlayWindow::operator=(OverlayWindow&& other) noexcept
{
    if (this != &other)
    {
        if (m_hwnd) DestroyWindow(m_hwnd);
        m_hwnd         = other.m_hwnd;
        m_monitorIndex = other.m_monitorIndex;
        m_visible      = other.m_visible;
        other.m_hwnd    = nullptr;
        other.m_visible = false;
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
// Window Styles Aciklamasi:
//   WS_EX_LAYERED      — Seffaflik destegi (per-pixel alpha)
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

    DWORD exStyle = WS_EX_LAYERED
                  | WS_EX_TRANSPARENT
                  | WS_EX_TOPMOST
                  | WS_EX_NOACTIVATE
                  | WS_EX_TOOLWINDOW;

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

    // Pencereyi tamamen seffaf yap
    // SetLayeredWindowAttributes ile alpha = 0 → tamamen gorunmez
    // Render basladiginda bu deger render pipeline tarafindan ayarlanacak
    SetLayeredWindowAttributes(m_hwnd, 0, 0, LWA_ALPHA);

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
        // Alpha'yi 255 yap (tam opak — render pipeline icerigi belirler)
        SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
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
