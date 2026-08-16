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

namespace BetterMagnifier {

struct CapturedFrame
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    DXGI_OUTDUPL_FRAME_INFO                 frameInfo{};
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

    // MUST be called after AcquireFrame or the next acquire fails.
    void ReleaseFrame();

    // Rebuild the duplication session after DXGI_ERROR_ACCESS_LOST.
    bool Reinitialize();

    bool IsInitialized() const { return m_initialized; }
    bool NeedsReinit()   const { return m_needsReinit; }

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

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<IDXGIOutput1>           m_output1;

    // Borrowed from D3DRenderer; never released here.
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    bool  m_frameAcquired = false;
    bool  m_initialized   = false;
    bool  m_needsReinit   = false;
    UINT  m_width  = 0;
    UINT  m_height = 0;

    // Re-read on every Initialize AND Reinitialize: rotating a display costs
    // the duplication session, and recovery must not carry the old orientation.
    DXGI_MODE_ROTATION m_rotation = DXGI_MODE_ROTATION_IDENTITY;

    uint64_t m_frameCount = 0;
    uint64_t m_errorCount = 0;

    // Throttles recovery attempts. While the workstation is locked every
    // attempt fails, and retrying per frame is pure log spam.
    std::chrono::steady_clock::time_point m_lastReinitAttempt{};
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_DXGI_CAPTURE_H
