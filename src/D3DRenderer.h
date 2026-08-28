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

// =============================================================================
// Rotated outputs
// =============================================================================
// Desktop Duplication hands back the display's UNROTATED mode image. On a
// portrait monitor — a 1080x1920 desktop rectangle driven by a 1920x1080 panel
// rotated 270 degrees — the acquired texture is 1920x1080 and lying on its
// side. Measured, not assumed: DXGI_OUTPUT_DESC.DesktopCoordinates said
// 1080x1920 while D3D11_TEXTURE2D_DESC said 1920x1080, and resampling the
// texture 90 degrees counter-clockwise reproduced a GDI screenshot of that
// monitor exactly (mean |difference| 0.00 per channel).
//
// Everything else in this application — ViewportController, the source rect,
// the cursor sprite, the overlay and its swap chain — works in DESKTOP
// coordinates, which is correct and stays that way. The rotation is absorbed
// here, in the one place that reads the texture: instead of a single UV origin
// plus extent, the vertex shader gets two UV axis vectors, and rotating the
// view is a matter of which way those axes point.
struct SourceUvMapping
{
    float originX = 0.0f, originY = 0.0f;  // UV of the region's top-left corner
    float uAxisX  = 0.0f, uAxisY  = 0.0f;  // UV travelled across the region's width
    float vAxisX  = 0.0f, vAxisY  = 0.0f;  // UV travelled down the region's height
};

// Desktop-space extent of a duplication texture under the given rotation.
// Swapped for ROTATE90/270, identical otherwise.
void DesktopExtentForRotation(DXGI_MODE_ROTATION rotation, UINT texW, UINT texH,
                              long& outWidth, long& outHeight);

// Pure math, which is the point: this is the whole rotation transform and it is
// asserted by D3DRendererSelfCheck without a GPU.
//
// texW/texH are the DUPLICATION TEXTURE's dimensions; srcRect is in DESKTOP
// coordinates and is clamped here, against the desktop extent rather than
// against the texture — those differ on a rotated output, and clamping against
// the wrong one is exactly the bug this replaces.
SourceUvMapping ComputeSourceUv(DXGI_MODE_ROTATION rotation,
                                UINT texW, UINT texH, RECT srcRect);

#ifdef _DEBUG
// Assert-based self-check, run from main. Mirrors ViewportControllerSelfCheck.
void D3DRendererSelfCheck();
#endif

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
    // srcRect is in DESKTOP coordinates. rotation is the capture's, and is what
    // turns those into texture coordinates — see SourceUvMapping above. Not
    // defaulted on purpose: a caller that forgets it on a portrait monitor gets
    // a compile error rather than a sideways screen.
    //
    // Returns false when there is nothing to draw; do not Present then.
    bool RenderFrame(ID3D11Texture2D* srcTexture, size_t targetIndex, const RECT& srcRect,
                     DXGI_MODE_ROTATION rotation);

    // Draws a premultiplied-BGRA sprite over whatever RenderFrame just drew.
    // Coordinates are target pixels; the top-left corner, hotspot already
    // subtracted by the caller. Call between RenderFrame and Present.
    bool RenderSprite(size_t targetIndex, ID3D11ShaderResourceView* srv,
                      float x, float y, float width, float height,
                      float opacity = 1.0f);

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
#ifdef _DEBUG
    // Env-driven frame dumping. Called from Present so the cursor sprite is
    // included, since that is composited after RenderFrame returns.
    void MaybeDumpFrame(size_t targetIndex);
#endif

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

    // Point, not linear: a magnified pointer reads better with crisp edges.
    Microsoft::WRL::ComPtr<ID3D11SamplerState>    m_samplerPoint;
    Microsoft::WRL::ComPtr<ID3D11BlendState>      m_alphaBlend;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>    m_spriteVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>     m_spritePS;
    Microsoft::WRL::ComPtr<ID3D11Buffer>          m_spriteBuffer;

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
