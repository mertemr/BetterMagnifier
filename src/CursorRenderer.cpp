#include "pch.h"
#include "CursorRenderer.h"
#include "Logger.h"

#include <cstdio>

namespace BetterMagnifier {
namespace {

// Reads a bitmap into a top-down 32-bit BGRA buffer.
bool ReadBitmapBits(HBITMAP bmp, int width, int height, std::vector<uint32_t>& out)
{
    if (!bmp || width <= 0 || height <= 0)
        return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -height;    // negative = top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    out.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);

    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;

    const int copied = GetDIBits(screen, bmp, 0, static_cast<UINT>(height),
                                 out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    return copied == height;
}

// A 32-bit cursor bitmap may carry a real alpha channel or may be opaque with
// the AND mask doing the work. Telling them apart needs an actual look: a
// fully-zero alpha channel means "no alpha here", not "invisible cursor".
bool HasRealAlpha(const std::vector<uint32_t>& px)
{
    for (uint32_t p : px)
        if ((p & 0xFF000000u) != 0u)
            return true;
    return false;
}

void PremultiplyInPlace(std::vector<uint32_t>& px)
{
    for (uint32_t& p : px)
    {
        const uint32_t a = (p >> 24) & 0xFFu;
        if (a == 0xFFu)
            continue;
        if (a == 0u) { p = 0u; continue; }

        const uint32_t b = ((p        & 0xFFu) * a) / 255u;
        const uint32_t g = (((p >> 8) & 0xFFu) * a) / 255u;
        const uint32_t r = (((p >> 16)& 0xFFu) * a) / 255u;
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

} // namespace

bool DecodeCursor(HCURSOR cursor, CursorBitmap& out)
{
    if (!cursor)
        return false;

    ICONINFO info{};
    if (!GetIconInfo(cursor, &info))
        return false;

    // GetIconInfo hands over two bitmaps and expects the caller to free them,
    // on every path including the failures below.
    struct Cleanup {
        HBITMAP color, mask;
        ~Cleanup() { if (color) DeleteObject(color); if (mask) DeleteObject(mask); }
    } cleanup{ info.hbmColor, info.hbmMask };

    BITMAP maskBm{};
    if (!GetObjectW(info.hbmMask, sizeof(maskBm), &maskBm))
        return false;

    out.hotspotX = static_cast<int>(info.xHotspot);
    out.hotspotY = static_cast<int>(info.yHotspot);

    if (info.hbmColor)
    {
        BITMAP colorBm{};
        if (!GetObjectW(info.hbmColor, sizeof(colorBm), &colorBm))
            return false;

        out.width  = colorBm.bmWidth;
        out.height = colorBm.bmHeight;

        std::vector<uint32_t> color;
        if (!ReadBitmapBits(info.hbmColor, out.width, out.height, color))
            return false;

        if (HasRealAlpha(color))
        {
            out.pixels = std::move(color);
            PremultiplyInPlace(out.pixels);
            return true;
        }

        // Opaque colour bitmap: the AND mask carries transparency. A set mask
        // bit means "keep the screen", i.e. transparent here.
        std::vector<uint32_t> mask;
        if (!ReadBitmapBits(info.hbmMask, out.width, out.height, mask))
            return false;

        out.pixels.assign(color.size(), 0u);
        for (size_t i = 0; i < color.size(); ++i)
        {
            const bool transparent = (mask[i] & 0x00FFFFFFu) != 0u;
            out.pixels[i] = transparent ? 0u : (color[i] | 0xFF000000u);
        }
        return true;
    }

    // ── Monochrome: hbmMask is 2x height, AND on top, XOR below ──
    out.width  = maskBm.bmWidth;
    out.height = maskBm.bmHeight / 2;
    if (out.width <= 0 || out.height <= 0)
        return false;

    std::vector<uint32_t> both;
    if (!ReadBitmapBits(info.hbmMask, out.width, maskBm.bmHeight, both))
        return false;

    const size_t plane = static_cast<size_t>(out.width) * static_cast<size_t>(out.height);
    if (both.size() < plane * 2)
        return false;

    out.pixels.assign(plane, 0u);

    for (size_t i = 0; i < plane; ++i)
    {
        const bool andBit = (both[i]         & 0x00FFFFFFu) != 0u;
        const bool xorBit = (both[i + plane] & 0x00FFFFFFu) != 0u;

        // AND=0 XOR=0 -> black    AND=0 XOR=1 -> white
        // AND=1 XOR=0 -> transparent
        // AND=1 XOR=1 -> invert the screen. We draw into our own render target
        //                and cannot read the screen back, so this approximates
        //                with white. It affects only the I-beam's outline,
        //                which reads correctly against dark and light text
        //                backgrounds either way.
        if      (andBit && !xorBit) out.pixels[i] = 0x00000000u;
        else if (andBit &&  xorBit) out.pixels[i] = 0xFFFFFFFFu;
        else if (xorBit)            out.pixels[i] = 0xFFFFFFFFu;
        else                        out.pixels[i] = 0xFF000000u;
    }
    return true;
}

bool WriteCursorBitmapFile(const CursorBitmap& bmp, const wchar_t* path)
{
    if (bmp.pixels.empty() || bmp.width <= 0 || bmp.height <= 0)
        return false;

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    const DWORD dataSize = static_cast<DWORD>(bmp.pixels.size() * 4);

    fh.bfType    = 0x4D42;   // "BM"
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + dataSize;

    ih.biSize        = sizeof(ih);
    ih.biWidth       = bmp.width;
    ih.biHeight      = -bmp.height;   // top-down
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = BI_RGB;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f)
        return false;

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bmp.pixels.data(), 1, dataSize, f);
    fclose(f);
    return true;
}

} // namespace BetterMagnifier
