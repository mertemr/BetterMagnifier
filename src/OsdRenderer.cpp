// =============================================================================
// OsdRenderer — GDI text into a premultiplied BGRA texture
// =============================================================================
//
// The one subtlety in here is the alpha channel, and it is worth stating up
// front because getting it wrong produces black text on a black box that looks
// like the OSD never rendered.
//
// DrawTextW writes RGB and leaves alpha at whatever the DIB already held. A
// 32-bit DIB comes back zeroed, so text drawn straight onto it is fully
// transparent — invisible, with no error anywhere. The fix is to own the alpha
// ourselves: the pill's coverage is computed geometrically, the glyph coverage
// is recovered from the luminance of white-on-black text, and the two are
// composited by hand.
// =============================================================================

#include "pch.h"
#include "OsdRenderer.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace BetterMagnifier {

namespace {

// Layout, in fractions of the font height, so the readout scales with the
// display it is drawn on rather than being pinned to one DPI.
constexpr double kPadX        = 0.62;
constexpr double kPadY        = 0.30;
constexpr double kCornerRatio = 0.34;   // of the pill's height

// Deliberately not pure black: a fully black pill on dark content reads as a
// hole punched in the screen, while a very dark grey reads as a panel over it.
constexpr double kPillR = 24.0, kPillG = 24.0, kPillB = 26.0;
constexpr double kPillAlpha = 0.82;

constexpr double kTextR = 244.0, kTextG = 246.0, kTextB = 250.0;

// Rounded-rectangle coverage at a point, antialiased over one pixel. Written
// out rather than reached for through GDI's RoundRect because that path draws
// into the colour channels and the alpha is exactly what has to be controlled
// here.
double PillCoverage(double x, double y, double w, double h, double r)
{
    r = (std::min)(r, (std::min)(w, h) * 0.5);

    // Distance outside the rounded rectangle, negative inside.
    const double dx = (std::max)((std::max)(r - x, x - (w - r)), 0.0);
    const double dy = (std::max)((std::max)(r - y, y - (h - r)), 0.0);

    double outside;
    if (dx > 0.0 && dy > 0.0)
        outside = std::sqrt(dx * dx + dy * dy) - r;   // in a corner
    else if (x < 0.0 || x > w || y < 0.0 || y > h)
        outside = (std::max)((std::max)(-x, x - w), (std::max)(-y, y - h));
    else
        outside = -1.0;                               // comfortably inside

    // One pixel of feather, centred on the edge.
    return std::clamp(0.5 - outside, 0.0, 1.0);
}

} // anonymous namespace

// =============================================================================
// RenderOsdText
// =============================================================================
bool RenderOsdText(const std::wstring& text, int fontHeightPx, OsdBitmap& out,
                   float opacity)
{
    out.pixels.clear();
    out.width  = 0;
    out.height = 0;

    if (text.empty())
        return false;

    fontHeightPx = std::clamp(fontHeightPx, 10, 400);

    const double fade = std::clamp(static_cast<double>(opacity), 0.0, 1.0);
    if (fade <= 0.0)
        return false;

    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;

    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc)
        return false;

    // Segoe UI Semibold. A magnifier's own UI has no business being the thinnest
    // thing on the screen.
    HFONT font = CreateFontW(
        -fontHeightPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

    if (!font)
    {
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ oldFont = SelectObject(dc, font);

    SIZE textSize{};
    if (!GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &textSize))
    {
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }

    const int padX = static_cast<int>(std::lround(fontHeightPx * kPadX));
    const int padY = static_cast<int>(std::lround(fontHeightPx * kPadY));

    const int w = textSize.cx + padX * 2;
    const int h = textSize.cy + padY * 2;

    if (w <= 0 || h <= 0 || w > 4096 || h > 1024)
    {
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }

    // Top-down (negative height) so the rows come out in the order the texture
    // upload wants and nothing has to be flipped afterwards.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);

    if (!dib || !bits)
    {
        if (dib) DeleteObject(dib);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ oldBmp = SelectObject(dc, dib);

    // White on black, so the glyph coverage can be read straight back out of any
    // colour channel below. Antialiasing and ClearType both land in here as
    // intermediate greys, which is exactly the coverage we want.
    RECT rc{ padX, padY, padX + textSize.cx, padY + textSize.cy };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    GdiFlush();

    out.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    out.width  = w;
    out.height = h;

    const auto* src = static_cast<const uint32_t*>(bits);
    const double radius = h * kCornerRatio;

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const uint32_t p = src[static_cast<size_t>(y) * w + x];

            // Green channel as the glyph's coverage. ClearType writes the three
            // channels differently for subpixel positioning, and picking one is
            // what turns that back into plain greyscale antialiasing — which is
            // correct here, because this bitmap gets scaled and composited, and
            // subpixel geometry does not survive either.
            const double glyph = ((p >> 8) & 0xFFu) / 255.0;

            const double pill = PillCoverage(x + 0.5, y + 0.5, w, h, radius) * kPillAlpha;

            // Text over pill, both over nothing.
            const double a = glyph + pill * (1.0 - glyph);

            if (a <= 0.002)
            {
                out.pixels[static_cast<size_t>(y) * w + x] = 0;
                continue;
            }

            const double r = kTextR * glyph + kPillR * pill * (1.0 - glyph);
            const double g = kTextG * glyph + kPillG * pill * (1.0 - glyph);
            const double b = kTextB * glyph + kPillB * pill * (1.0 - glyph);

            // Premultiplied: the colour terms above are already weighted by
            // their own coverage, so they are the premultiplied values as they
            // stand and must NOT be divided by a and multiplied back.
            const auto clamp8 = [](double v) -> uint32_t {
                return static_cast<uint32_t>(std::clamp(std::lround(v), 0L, 255L));
            };

            // Premultiplied, so one factor across all four channels is exactly a
            // change of opacity — no unpremultiply-and-back needed.
            out.pixels[static_cast<size_t>(y) * w + x] =
                  (clamp8(a * 255.0 * fade) << 24)
                | (clamp8(r * fade) << 16)
                | (clamp8(g * fade) << 8)
                |  clamp8(b * fade);
        }
    }

    SelectObject(dc, oldBmp);
    SelectObject(dc, oldFont);
    DeleteObject(dib);
    DeleteObject(font);
    DeleteDC(dc);
    return true;
}

// =============================================================================
// OsdCache
// =============================================================================
bool OsdCache::Acquire(const std::wstring& text, int fontHeightPx, float opacity, Label& out)
{
    if (!m_device || text.empty())
        return false;

    // Quantised to a byte before it becomes a key, so a float that wanders in
    // the last bits cannot mint a fresh entry — and a fresh entry every frame is
    // a texture upload every frame.
    const int alpha = std::clamp(static_cast<int>(std::lround(opacity * 255.0f)), 0, 255);
    if (alpha == 0)
        return false;

    // The height is part of the key: the same string on a 4K display and on a
    // 1080p one is a different bitmap, and keying on the text alone would show
    // whichever size happened to be rendered first on both.
    const std::wstring key = text + L'\x1' + std::to_wstring(fontHeightPx)
                                  + L'\x1' + std::to_wstring(alpha);

    if (auto it = m_entries.find(key); it != m_entries.end())
    {
        out.srv    = it->second.srv.Get();
        out.width  = it->second.width;
        out.height = it->second.height;
        return true;
    }

    OsdBitmap bmp;
    if (!RenderOsdText(text, fontHeightPx, bmp, alpha / 255.0f) || bmp.pixels.empty())
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = static_cast<UINT>(bmp.width);
    desc.Height           = static_cast<UINT>(bmp.height);
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem     = bmp.pixels.data();
    init.SysMemPitch = static_cast<UINT>(bmp.width) * 4u;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(m_device->CreateTexture2D(&desc, &init, &tex)))
        return false;

    Entry entry;
    if (FAILED(m_device->CreateShaderResourceView(tex.Get(), nullptr, &entry.srv)))
        return false;

    entry.width  = bmp.width;
    entry.height = bmp.height;

    if (m_entries.size() >= kMaxEntries)
        m_entries.clear();

    const Entry& stored = (m_entries[key] = std::move(entry));

    out.srv    = stored.srv.Get();
    out.width  = stored.width;
    out.height = stored.height;
    return true;
}

void OsdCache::Clear()
{
    m_entries.clear();
}

} // namespace BetterMagnifier
