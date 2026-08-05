#pragma once

// D3D11 magnification pipeline: one swap chain per monitor, a fullscreen
// triangle sampling the captured frame with bilinear filtering.

#ifndef BETTER_MAGNIFIER_D3D_RENDERER_H
#define BETTER_MAGNIFIER_D3D_RENDERER_H

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <vector>

namespace BetterMagnifier {

struct RenderTarget
{
    Microsoft::WRL::ComPtr<IDXGISwapChain1>        swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    HWND                                           targetWindow = nullptr;
    UINT                                           width  = 0;
    UINT                                           height = 0;

    // Desktop Duplication textures arrive WITHOUT D3D11_BIND_SHADER_RESOURCE,
    // so they cannot be sampled directly. Every frame is copied here first.
    //
    // Per monitor rather than shared, because resolutions differ and a shared
    // texture would have to be recreated whenever two monitors were active.
    //
    // Second job: hold on to the last frame. Desktop Duplication returns
    // nothing when the screen has not changed, and the anchor still needs to
    // be able to move across the previous image.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          sourceTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceSrv;
    UINT                                             sourceWidth  = 0;
    UINT                                             sourceHeight = 0;
    DXGI_FORMAT                                      sourceFormat = DXGI_FORMAT_UNKNOWN;
};

class D3DRenderer
{
public:
    D3DRenderer() = default;
    ~D3DRenderer();

    D3DRenderer(const D3DRenderer&) = delete;
    D3DRenderer& operator=(const D3DRenderer&) = delete;

    bool Initialize();

    bool CreateSwapChainForWindow(HWND hwnd, UINT width, UINT height, size_t index);

    // Stretches srcRect to fill the whole target.
    //
    // srcTexture nullptr means "reuse the last frame", which is what happens
    // when the screen has not changed but the anchor moved.
    //
    // Returns false when there is nothing to draw; do not Present then.
    bool RenderFrame(ID3D11Texture2D* srcTexture, size_t targetIndex, const RECT& srcRect);

    void Present(size_t targetIndex, bool vSync = true);

    ID3D11Device*        GetDevice()  const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }

    void RemoveRenderTarget(size_t index);

#ifdef _DEBUG
    // Writes the back buffer to a BMP. Must be called BEFORE Present, since
    // FLIP_DISCARD leaves the contents undefined afterwards.
    //
    // Why it exists: the overlay is WDA_EXCLUDEFROMCAPTURE, so it does not
    // appear in screenshots and there is no other way to check the render from
    // outside. Set BM_DUMP_FRAME to a path to dump one frame automatically,
    // BM_DUMP_AFTER to choose which frame.
    bool DumpBackBuffer(size_t targetIndex, const wchar_t* path);
#endif

private:
    bool CreateDevice();
    bool CreateSamplerStates();
    bool CreateShaders();

    // Prepares the intermediate texture for the source size and format.
    // No-op when nothing changed; creating textures per frame is expensive.
    bool EnsureSourceTexture(RenderTarget& rt, const D3D11_TEXTURE2D_DESC& srcDesc);

    Microsoft::WRL::ComPtr<ID3D11Device1>        m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context;
    Microsoft::WRL::ComPtr<IDXGIFactory2>        m_dxgiFactory;
    D3D_FEATURE_LEVEL                            m_featureLevel = D3D_FEATURE_LEVEL_11_0;

    Microsoft::WRL::ComPtr<ID3D11SamplerState>   m_samplerLinear;

    // No vertex buffer and no input layout: the vertex shader derives its
    // positions from SV_VertexID.
    Microsoft::WRL::ComPtr<ID3D11VertexShader>    m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>     m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer>          m_uvBuffer;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterNoCull;

    std::vector<RenderTarget> m_renderTargets;

#ifdef _DEBUG
    Microsoft::WRL::ComPtr<ID3D11Debug> m_debug;
#endif
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_D3D_RENDERER_H
