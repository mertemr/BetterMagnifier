#pragma once

// Decodes a system cursor into straight BGRA pixels so it can be uploaded as a
// texture and drawn magnified.
//
// The monochrome path is not optional. Cursors with hbmColor == NULL store a
// double-height mask: the top half is AND, the bottom half is XOR. I-beam is
// monochrome, and it is the cursor a magnifier user spends the most time
// looking at, so skipping that path would leave the feature visibly
// half-built.
//
// Output is premultiplied so the renderer can blend with ONE / INV_SRC_ALPHA.
// Straight alpha with SRC_ALPHA / INV_SRC_ALPHA leaves a halo on the magnified
// edges, which is exactly where magnification makes it obvious.

#ifndef BETTER_MAGNIFIER_CURSOR_RENDERER_H
#define BETTER_MAGNIFIER_CURSOR_RENDERER_H

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace BetterMagnifier {

struct CursorBitmap
{
    std::vector<uint32_t> pixels;   // BGRA, premultiplied, row-major, top-down
    int width    = 0;
    int height   = 0;
    int hotspotX = 0;
    int hotspotY = 0;
};

// False when the handle is null or GDI refuses it. Never throws.
bool DecodeCursor(HCURSOR cursor, CursorBitmap& out);

// Writes a 32-bit top-down BMP. Debug harness only (--dump-cursors).
bool WriteCursorBitmapFile(const CursorBitmap& bmp, const wchar_t* path);

// GPU-side cache of decoded cursor shapes.
//
// Keyed by HCURSOR: shape handles are stable and few, so caching avoids a
// GetIconInfo plus two GetDIBits calls on every frame. An animated (.ani)
// cursor keeps one handle throughout, so this holds its first frame and the
// spinner appears frozen — accepted; walking .ani frames is not worth it for a
// state nobody looks at.
class CursorCache
{
public:
    struct Shape
    {
        ID3D11ShaderResourceView* srv = nullptr;   // owned by the cache
        int width    = 0;
        int height   = 0;
        int hotspotX = 0;
        int hotspotY = 0;
    };

    // Three outcomes, and the caller must tell them apart. "Hidden" is correct
    // behaviour — the OS pointer is genuinely gone, so drawing nothing is
    // right. "Failed" is a fault, and if the real pointer has been hidden for
    // us it means the user currently has no pointer at all, which has to be
    // backed out of rather than tolerated.
    enum class State { Ok, Hidden, Failed };

    void Initialize(ID3D11Device* device) { m_device = device; }

    State Current(Shape& out);

    void Clear();

private:
    bool Acquire(HCURSOR cursor, Shape& out);

    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int width = 0, height = 0, hotspotX = 0, hotspotY = 0;
    };

    ID3D11Device* m_device = nullptr;
    std::unordered_map<HCURSOR, Entry> m_entries;

    // Cursor shapes are few. Past this something is leaking handles, and
    // dropping the lot is cheaper and simpler than an LRU.
    static constexpr size_t kMaxEntries = 32;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_CURSOR_RENDERER_H
