#pragma once

// A short-lived on-screen readout — "2.50x", "Frozen" — drawn over the
// magnified content on the monitor whose state just changed.
//
// Why it exists: zoom had no feedback at all. Stepping with Win+Plus or
// Ctrl+Alt+wheel changed the picture but never said what level you had reached,
// and the level only appeared in the control panel, which is off by default and
// on the other side of a tray menu. On a magnified screen even the tray tooltip
// is a poor place for it — it is on whichever monitor holds the taskbar, at
// whatever size the taskbar is, which may be nowhere near where the user is
// looking.
//
// GDI renders the text into a DIB and the result is uploaded as one texture,
// rather than pulling in DirectWrite for a handful of short strings. The panel
// spent a lot of this project's time proving that adding a UI framework to this
// process is expensive; a DrawTextW into a memory DC is a known quantity.
//
// Output is premultiplied BGRA, matching CursorRenderer, so the same
// D3DRenderer::RenderSprite blend state draws it with no halo.
//
// Thread ownership: the render thread, exclusively. Nothing here is atomic and
// nothing needs to be.

#ifndef BETTER_MAGNIFIER_OSD_RENDERER_H
#define BETTER_MAGNIFIER_OSD_RENDERER_H

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>

namespace BetterMagnifier {

// GPU-side cache of rendered strings.
//
// Keyed by the text AND the height it was rendered at, because the same string
// on a 4K display and on a 1080p one are different pixels. The set of strings is
// small and repetitive — a zoom ramp revisits "2.00x" constantly — so caching
// turns a per-frame GDI render into a hash lookup.
class OsdCache
{
public:
    struct Label
    {
        ID3D11ShaderResourceView* srv = nullptr;   // owned by the cache
        int width  = 0;
        int height = 0;
    };

    void Initialize(ID3D11Device* device) { m_device = device; }

    // False when the text could not be rendered or uploaded. A missing OSD is
    // cosmetic: the caller draws nothing and carries on.
    //
    // opacity is baked into the bitmap and forms part of the cache key. Scaling
    // the sprite in the shader would be the other way to do it, and would mean a
    // second constant buffer register and an HLSL change for one faded label.
    bool Acquire(const std::wstring& text, int fontHeightPx, float opacity, Label& out);

    void Clear();

private:
    struct Entry
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int width = 0, height = 0;
    };

    ID3D11Device* m_device = nullptr;
    std::unordered_map<std::wstring, Entry> m_entries;

    // Zoom levels between the limits at the finest step, both freeze states and
    // a couple of DPIs still fit inside this. Past it something is generating
    // strings rather than reusing them, and dropping the lot beats an LRU.
    static constexpr size_t kMaxEntries = 128;
};

// Draw one string into premultiplied BGRA. Exposed for the self-check, which
// asserts the layout arithmetic without needing a D3D device.
//
// The pill background is part of the bitmap rather than a separate draw: the
// readout has to stay legible over arbitrary desktop content, and one texture
// with the background baked in costs one RenderSprite instead of two.
struct OsdBitmap
{
    std::vector<uint32_t> pixels;   // BGRA, premultiplied, top-down
    int width  = 0;
    int height = 0;
};

// opacity scales every channel of the premultiplied result, which is the whole
// of what fading a premultiplied bitmap means.
bool RenderOsdText(const std::wstring& text, int fontHeightPx, OsdBitmap& out,
                   float opacity = 1.0f);

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_OSD_RENDERER_H
