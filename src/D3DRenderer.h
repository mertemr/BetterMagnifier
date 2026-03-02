#pragma once

// =============================================================================
// D3DRenderer.h — DirectX 11 Render Pipeline
// =============================================================================
//
// Python analojisi:
//   Python'da: pygame.display.set_mode() + surface.blit()
//   Burada:    D3DRenderer → D3D11 device, swap chain, render
//
// Bu sinif ne yapar:
//   1. D3D11 Device + DeviceContext olusturur (GPU ile konusma noktasi)
//   2. Her monitor icin SwapChain1 olusturur (DXGI 1.2 flip model)
//   3. Captured texture'i zoom bolgesiyle crop edip overlay'e render eder
//   4. Bilinear interpolation ile smooth scaling yapar
//
// =============================================================================

#ifndef BETTER_MAGNIFIER_D3D_RENDERER_H
#define BETTER_MAGNIFIER_D3D_RENDERER_H

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <vector>

namespace BetterMagnifier {

// Forward declarations
struct MonitorInfo;

// ─────────────────────────────────────────────────────────────────────────────
// Per-Monitor Render Target — Her monitore ait swap chain ve render hedefi
// ─────────────────────────────────────────────────────────────────────────────
struct RenderTarget
{
    Microsoft::WRL::ComPtr<IDXGISwapChain1>       swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    HWND                                           targetWindow = nullptr;
    UINT                                           width  = 0;
    UINT                                           height = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// D3DRenderer Class
// ─────────────────────────────────────────────────────────────────────────────
class D3DRenderer
{
public:
    D3DRenderer() = default;
    ~D3DRenderer();

    D3DRenderer(const D3DRenderer&) = delete;
    D3DRenderer& operator=(const D3DRenderer&) = delete;

    // ── Initialization ──
    bool Initialize();

    // ── Per-Monitor Swap Chain ──
    // Overlay window'a bagli swap chain olusturur.
    // index: MonitorManager'daki monitor indexi (render target ID olarak kullanilir)
    bool CreateSwapChainForWindow(HWND hwnd, UINT width, UINT height, size_t index);

    // ── Rendering ──
    // Captured texture'i zoom parametreleriyle render et.
    // srcTexture: DXGICapture'dan gelen GPU texture
    // targetIndex: Hangi monitore (swap chain'e) render edilecek
    // srcRect: Source texture'dan kesilecek bolge (zoom bolgesi)
    void RenderFrame(
        ID3D11Texture2D* srcTexture,
        size_t targetIndex,
        const RECT& srcRect);

    // ── Present ──
    // Render edilen frame'i ekrana goster.
    // vSync: true = monitör refresh rate'ine senkronize, false = uncapped
    void Present(size_t targetIndex, bool vSync = true);

    // ── Resize ──
    void ResizeTarget(size_t targetIndex, UINT width, UINT height);

    // ── Access ──
    ID3D11Device*        GetDevice()  const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    size_t GetRenderTargetCount() const { return m_renderTargets.size(); }

    // ── Cleanup ──
    void RemoveRenderTarget(size_t index);

private:
    // ── Device Creation ──
    bool CreateDevice();
    bool CreateSamplerStates();

    // ── Internal Render Helpers ──
    bool EnsureShaderResourceView(ID3D11Texture2D* texture,
                                  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);

    // ── D3D11 Core ──
    Microsoft::WRL::ComPtr<ID3D11Device1>        m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1>  m_context;
    Microsoft::WRL::ComPtr<IDXGIFactory2>         m_dxgiFactory;
    D3D_FEATURE_LEVEL                             m_featureLevel = D3D_FEATURE_LEVEL_11_0;

    // ── Sampler States ──
    Microsoft::WRL::ComPtr<ID3D11SamplerState>    m_samplerLinear;   // Bilinear
    Microsoft::WRL::ComPtr<ID3D11SamplerState>    m_samplerPoint;    // Nearest-neighbor

    // ── Per-Monitor Render Targets ──
    std::vector<RenderTarget> m_renderTargets;

    // ── Debug ──
#ifdef _DEBUG
    Microsoft::WRL::ComPtr<ID3D11Debug> m_debug;
#endif
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_D3D_RENDERER_H
