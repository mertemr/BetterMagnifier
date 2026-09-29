#pragma once

// Desktop Duplication capture for a single monitor.
//
// Frames stay on the GPU: no readback to system memory, which is what makes it
// fast enough for a live magnifier. Captures the whole desktop composition, so
// every window is included.
//
// Known limits: DXGI_ERROR_ACCESS_LOST when a fullscreen exclusive app takes
// over, and nothing at all on the secure desktop (UAC, lock screen).
//
// One instance per monitor.

#ifndef BETTER_MAGNIFIER_DXGI_CAPTURE_H
#define BETTER_MAGNIFIER_DXGI_CAPTURE_H

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <vector>

namespace BetterMagnifier {

struct CapturedFrame
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    DXGI_OUTDUPL_FRAME_INFO                 frameInfo{};

    // A new DESKTOP IMAGE — not merely a new acquire.
    //
    // AcquireNextFrame also succeeds when nothing but the pointer moved, and
    // says so by leaving LastPresentTime at zero. Counting those as new frames
    // meant a full-screen CopyResource, a full shader pass and a layered-window
    // Present on every single mouse move over a completely static desktop,
    // which for a magnifier is most of the frames it ever draws.
    bool                                    isNewFrame = false;

    UINT                                    width  = 0;
    UINT                                    height = 0;
};

class DXGICapture
{
public:
    DXGICapture() = default;
    ~DXGICapture();

    DXGICapture(const DXGICapture&) = delete;
    DXGICapture& operator=(const DXGICapture&) = delete;

    // Movable so it can live in a vector.
    DXGICapture(DXGICapture&& other) noexcept;
    DXGICapture& operator=(DXGICapture&& other) noexcept;

    // device comes from D3DRenderer and is NOT owned here.
    bool Initialize(ID3D11Device* device, IDXGIOutput* output);

    // isNewFrame false means the screen did not change; keep using the
    // previous frame.
    CapturedFrame AcquireFrame(UINT timeoutMs = 16);

    // ── What changed in the last acquired frame ──
    //
    // Desktop Duplication reports the regions it repainted, and a magnifier
    // showing one corner of the screen does not care about a video playing in
    // another. Skipping the draw when the change misses the magnified region
    // is worth far more here than it would be in a full-screen capture tool:
    // the overlay is a layered window, so every Present is a DWM surface
    // update over the whole monitor.
    //
    // Coordinates are the TEXTURE's, which on a rotated output is not desktop
    // space — see GetRotation. Use DesktopRectToTextureRect to compare.
    //
    // DirtyKnown() false means the metadata was unavailable or incomplete and
    // the caller must assume everything changed. That is the safe reading and
    // the one the first frame after a (re)initialisation always gets.
    bool DirtyKnown() const { return m_dirtyKnown; }
    bool DirtyIntersects(const RECT& textureRect) const;

    // The regions themselves, for copying only what changed. Meaningless
    // unless DirtyKnown(); move rects contribute both their ends.
    const std::vector<RECT>& DirtyRects() const { return m_dirtyRects; }

    // AcquireFrame releases a held frame itself before asking for the next
    // one, and the render loop relies on that: it holds each frame until then,
    // which is the order the Desktop Duplication documentation recommends.
    void ReleaseFrame();

    // Rebuild the duplication session after DXGI_ERROR_ACCESS_LOST.
    bool Reinitialize();

    // ── Idle monitors hold no duplication session ──
    //
    // An open session is not free even when nobody acquires from it: while the
    // client does not own a frame, the OS copies every desktop update into the
    // duplication surface (see IDXGIOutputDuplication::ReleaseFrame). A game on
    // a monitor that is not being magnified — or on any monitor while the app
    // sits in the tray — paid for that copy on every one of its presents.
    //
    // Suspend drops the session and keeps device and output, like recovery
    // does. Resume reopens it immediately, bypassing the recovery throttle,
    // because it runs on the user's own zoom-on and must not wait 500 ms.
    // Returns true only on the transition, so the caller can release what it
    // was holding for this monitor once rather than every tick.
    bool Suspend();
    bool Resume();

    bool IsInitialized() const { return m_initialized; }
    bool NeedsReinit()   const { return m_needsReinit; }
    bool IsSuspended()   const { return m_suspended; }

    // DESKTOP dimensions, from DXGI_OUTPUT_DESC.DesktopCoordinates. On a
    // rotated output these are deliberately NOT the acquired texture's
    // dimensions — see GetRotation.
    UINT GetWidth()      const { return m_width; }
    UINT GetHeight()     const { return m_height; }

    // How the panel is turned relative to the desktop image. Desktop
    // Duplication hands back the UNROTATED mode image, so on a portrait monitor
    // a 1080x1920 desktop arrives as a 1920x1080 texture lying on its side.
    // The renderer needs this to sample it upright; without it a portrait
    // monitor magnifies as though it were landscape.
    DXGI_MODE_ROTATION GetRotation() const { return m_rotation; }

private:
    // Full teardown, including the borrowed device/output pointers. Only for
    // the destructor and move assignment.
    void Cleanup();

    // Drops just the duplication session and keeps device and output, so
    // Reinitialize still has what it needs. Using Cleanup here is what made
    // locking the workstation permanently kill capture.
    void ReleaseDuplication();

    // Fills m_dirtyRects / m_dirtyKnown from the frame's move and dirty rect
    // metadata. Any failure leaves m_dirtyKnown false, which reads as "assume
    // the whole texture changed".
    void ReadDirtyMetadata(const DXGI_OUTDUPL_FRAME_INFO& frameInfo);

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<IDXGIOutput1>           m_output1;

    // Borrowed from D3DRenderer; never released here.
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    bool  m_frameAcquired = false;
    bool  m_initialized   = false;
    bool  m_needsReinit   = false;
    bool  m_suspended     = false;
    UINT  m_width  = 0;
    UINT  m_height = 0;

    // Re-read on every Initialize AND Reinitialize: rotating a display costs
    // the duplication session, and recovery must not carry the old orientation.
    DXGI_MODE_ROTATION m_rotation = DXGI_MODE_ROTATION_IDENTITY;

    uint64_t m_frameCount = 0;
    uint64_t m_errorCount = 0;

    // The first frame after DuplicateOutput carries the whole desktop whether
    // or not the metadata says so, and its LastPresentTime cannot be trusted
    // to be non-zero. Forced through as a full update exactly once, or a
    // session that starts on a perfectly static screen would never get an
    // image at all.
    bool m_firstFrame = true;

    // Reused across frames: the metadata is a few kilobytes and allocating it
    // per frame would put the render thread on the heap at display rate.
    std::vector<uint8_t> m_metadata;
    std::vector<RECT>    m_dirtyRects;
    bool                 m_dirtyKnown = false;

    // Throttles recovery attempts. While the workstation is locked every
    // attempt fails, and retrying per frame is pure log spam.
    std::chrono::steady_clock::time_point m_lastReinitAttempt{};
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_DXGI_CAPTURE_H
