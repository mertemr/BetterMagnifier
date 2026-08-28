// =============================================================================
// D3DRenderer.cpp — DirectX 11 Render Pipeline Implementation
// =============================================================================

#include "pch.h"
#include "D3DRenderer.h"
#include "Logger.h"

#include <d3dcompiler.h>

namespace BetterMagnifier {

namespace {

// =============================================================================
// Shader source — this is where the magnification actually happens
// =============================================================================
// An earlier version used CopySubresourceRegion, which copies pixels 1:1 and
// does not scale. Raising the zoom shrank the source region without enlarging
// anything, so 2x produced a cropped copy of the top-left quarter of the
// screen.
//
// The right approach is to turn srcRect into UV coordinates and sample it
// across a triangle that covers the whole target. The small region gets
// stretched to screen size, which is the magnification.
//
// One triangle rather than a two-triangle quad: fewer vertices, and no
// resampling seam along the shared diagonal. The standard fullscreen-triangle
// trick.
//
// No vertex buffer and no input layout either — SV_VertexID gives the corner
// index and the positions fall out of arithmetic on it.
// =============================================================================
// Two UV axes rather than an origin plus an extent, because a rotated output's
// texture is not in desktop orientation and the mapping is no longer a plain
// scale. See SourceUvMapping in D3DRenderer.h.
// =============================================================================
constexpr char kShaderSource[] = R"HLSL(
cbuffer UvParams : register(b0)
{
    // xy = source region's top-left UV, zw = UV travelled across its width
    float4 uvOriginU;
    // xy = UV travelled down the region's height, zw = unused padding
    float4 uvAxisV;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // vid 0,1,2 -> corner (0,0), (2,0), (0,2)
    // One oversized triangle covering the screen; the off-screen part is clipped.
    float2 corner = float2((vid << 1) & 2, vid & 2);

    VSOut o;
    // corner 0..2 maps to NDC -1..3 in x and 1..-3 in y. Y is flipped because
    // NDC grows upward while texture UV grows downward.
    o.pos = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    // Affine, not a scale: on a rotated output uAxis runs down the texture and
    // vAxis runs across it. corner leaves [0,1] on the oversized triangle, and
    // the extrapolation is linear, so the clipped part lands where it should.
    o.uv  = uvOriginU.xy + corner.x * uvOriginU.zw + corner.y * uvAxisV.xy;
    return o;
}

Texture2D    srcTex : register(t0);
SamplerState srcSmp : register(s0);

float4 PSMain(VSOut input) : SV_TARGET
{
    // Alpha 1.0 sabit: swap chain AlphaMode IGNORE, overlay opak.
    return float4(srcTex.Sample(srcSmp, input.uv).rgb, 1.0);
}

// ── Cursor sprite ──
// A pass of its own: the content is sampled bilinear, but a magnified arrow
// kenarli olmasi bulanik olmasindan iyi okunuyor, o yuzden kendi sampler'i var.
cbuffer SpriteParams : register(b0)
{
    // Hedef dikdortgen, NDC: xy = sol-ust, zw = sag-alt
    float4 spriteRect;
    // x = opacity, kalani hizalama dolgusu
    float4 spriteFade;
};

VSOut SpriteVS(uint vid : SV_VertexID)
{
    // Dort vertex'lik strip: (0,0) (1,0) (0,1) (1,1)
    float2 c = float2(vid & 1, (vid >> 1) & 1);

    VSOut o;
    o.pos = float4(lerp(spriteRect.xy, spriteRect.zw, c), 0.0, 1.0);
    o.uv  = c;
    return o;
}

float4 SpritePS(VSOut input) : SV_TARGET
{
    // Premultiplied: blend ONE / INV_SRC_ALPHA ile eslesiyor. Opacity hem rgb
    // hem alpha'yi olcekler, premultiplied'da dogru olan bu.
    return srcTex.Sample(srcSmp, input.uv) * spriteFade.x;
}
)HLSL";

// Constant buffer duzeni. D3D11 sabit tampon boyutunu 16'nin kati istiyor —
// two float4s are exactly 32 bytes.
struct UvParams
{
    float originX, originY;
    float uAxisX,  uAxisY;
    float vAxisX,  vAxisY;
    float pad0,    pad1;
};

} // anonymous namespace

// =============================================================================
// DesktopExtentForRotation / ComputeSourceUv — the rotation transform
// =============================================================================
void DesktopExtentForRotation(DXGI_MODE_ROTATION rotation, UINT texW, UINT texH,
                              long& outWidth, long& outHeight)
{
    if (rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270)
    {
        outWidth  = static_cast<long>(texH);
        outHeight = static_cast<long>(texW);
    }
    else
    {
        outWidth  = static_cast<long>(texW);
        outHeight = static_cast<long>(texH);
    }
}

SourceUvMapping ComputeSourceUv(DXGI_MODE_ROTATION rotation,
                                UINT texW, UINT texH, RECT srcRect)
{
    SourceUvMapping m{};
    if (texW == 0 || texH == 0)
        return m;

    const long tw = static_cast<long>(texW);
    const long th = static_cast<long>(texH);

    long deskW = 0, deskH = 0;
    DesktopExtentForRotation(rotation, texW, texH, deskW, deskH);

    // Clamped in DESKTOP space. The old code clamped against the texture, which
    // on a portrait monitor pinned the vertical range to the panel's short side
    // and let the horizontal one run off the end.
    const long L = std::clamp(srcRect.left, 0L, deskW - 1);
    const long T = std::clamp(srcRect.top,  0L, deskH - 1);
    const long R = std::clamp(srcRect.right,  L + 1, deskW);
    const long B = std::clamp(srcRect.bottom, T + 1, deskH);

    const float fw = static_cast<float>(tw);
    const float fh = static_cast<float>(th);
    const float sw = static_cast<float>(R - L);   // region width,  desktop space
    const float sh = static_cast<float>(B - T);   // region height, desktop space

    // Each case is the inverse of "rotate the mode image by N degrees clockwise
    // to get the desktop", written in continuous coordinates:
    //   ROTATE90   u = y,      v = th - x
    //   ROTATE180  u = tw - x, v = th - y
    //   ROTATE270  u = tw - y, v = x
    // and the three corners (L,T), (R,T), (L,B) give origin, uAxis and vAxis.
    switch (rotation)
    {
    case DXGI_MODE_ROTATION_ROTATE90:
        m.originX = static_cast<float>(T)      / fw;
        m.originY = static_cast<float>(th - L) / fh;
        m.uAxisX  = 0.0f;      m.uAxisY = -sw / fh;
        m.vAxisX  = sh / fw;   m.vAxisY = 0.0f;
        break;

    case DXGI_MODE_ROTATION_ROTATE180:
        m.originX = static_cast<float>(tw - L) / fw;
        m.originY = static_cast<float>(th - T) / fh;
        m.uAxisX  = -sw / fw;  m.uAxisY = 0.0f;
        m.vAxisX  = 0.0f;      m.vAxisY = -sh / fh;
        break;

    case DXGI_MODE_ROTATION_ROTATE270:
        m.originX = static_cast<float>(tw - T) / fw;
        m.originY = static_cast<float>(L)      / fh;
        m.uAxisX  = 0.0f;      m.uAxisY = sw / fh;
        m.vAxisX  = -sh / fw;  m.vAxisY = 0.0f;
        break;

    // UNSPECIFIED included: it is what a driver reports when it does not rotate.
    default:
        m.originX = static_cast<float>(L) / fw;
        m.originY = static_cast<float>(T) / fh;
        m.uAxisX  = sw / fw;   m.uAxisY = 0.0f;
        m.vAxisX  = 0.0f;      m.vAxisY = sh / fh;
        break;
    }

    return m;
}

// =============================================================================
// Destructor
// =============================================================================
D3DRenderer::~D3DRenderer()
{
    // Render target'lari temizle (swap chain'ler dahil)
    m_renderTargets.clear();

    m_samplerLinear.Reset();

    // Shader pipeline
    m_rasterNoCull.Reset();
    m_uvBuffer.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();

    // Context flush — bekleyen GPU komutlarini bitir
    if (m_context)
    {
        m_context->ClearState();
        m_context->Flush();
    }

#ifdef _DEBUG
    // Debug build'de COM leak report
    if (m_debug)
    {
        LOG_INFO("=== D3D11 Live Object Report ===");
        m_debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
        m_debug.Reset();
    }
#endif

    m_context.Reset();
    m_device.Reset();
    m_dxgiFactory.Reset();

    LOG_INFO("D3DRenderer temizlendi");
}

// =============================================================================
// Initialize
// =============================================================================
bool D3DRenderer::Initialize()
{
    LOG_INFO("D3DRenderer baslatiliyor...");

    if (!CreateDevice())
        return false;

    if (!CreateSamplerStates())
        return false;

    if (!CreateShaders())
        return false;

    LOG_INFO("D3DRenderer initialised (feature level: 0x{:X})",
        static_cast<unsigned int>(m_featureLevel));

    return true;
}

// =============================================================================
// CreateDevice — D3D11 Device + Context + DXGI Factory
// =============================================================================
//
// Debug builds enable the D3D11 validation layer: misuse of the API shows up
// as a warning in the debugger output instead of as undefined behaviour, and
// ReportLiveDeviceObjects catches leaked interfaces at shutdown. It costs
// performance, which is why it is Debug only.
// =============================================================================
bool D3DRenderer::CreateDevice()
{
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // needed for Direct2D interop

#ifdef _DEBUG
    // The DirectX debug layer, on Debug builds
    // Surfaces DirectX errors in the Visual Studio Output window
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
    LOG_DEBUG("D3D11 debug layer enabled");
#endif

    // Highest first; D3D11CreateDevice picks the first the adapter supports.
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    ComPtr<ID3D11Device> baseDevice;
    ComPtr<ID3D11DeviceContext> baseContext;

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // default adapter: the primary GPU
        D3D_DRIVER_TYPE_HARDWARE,   // Gercek GPU kullan (WARP = software fallback)
        nullptr,                    // Software rasterizer modulu (kulllanmiyoruz)
        createFlags,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        &baseDevice,
        &m_featureLevel,
        &baseContext
    );

    if (FAILED(hr))
    {
        LOG_ERROR("D3D11CreateDevice failed: 0x{:08X}", static_cast<unsigned long>(hr));

        // Debug layer yuklu degilse, onsuz tekrar dene
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            LOG_WARN("Debug layer not present, retrying without it");
            createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                createFlags, featureLevels, _countof(featureLevels),
                D3D11_SDK_VERSION,
                &baseDevice, &m_featureLevel, &baseContext
            );
        }

        if (FAILED(hr))
        {
            LOG_ERROR("D3D11CreateDevice failed outright: 0x{:08X}", static_cast<unsigned long>(hr));
            return false;
        }
    }

    // ID3D11Device1 is what the DXGI 1.2 swap chain path needs.
    hr = baseDevice.As(&m_device);
    if (FAILED(hr))
    {
        LOG_ERROR("ID3D11Device1 QueryInterface failed");
        return false;
    }

    hr = baseContext.As(&m_context);
    if (FAILED(hr))
    {
        LOG_ERROR("ID3D11DeviceContext1 QueryInterface failed");
        return false;
    }

#ifdef _DEBUG
    // Kept for ReportLiveDeviceObjects in the destructor.
    m_device.As(&m_debug);
#endif

    // Reach the factory the device was created from rather than making a new
    // one; a swap chain created on a foreign factory is not guaranteed to work
    // with this device.
    ComPtr<IDXGIDevice1> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr))
    {
        LOG_ERROR("IDXGIDevice1 QueryInterface failed");
        return false;
    }

    // DXGI queues three frames by default, which at 75 Hz is up to 40 ms
    // between moving the mouse and seeing the view follow. On a magnifier that
    // reads as lag.
    //
    // One frame of latency costs buffering between CPU and GPU, which would
    // hurt throughput on a heavy scene. The workload here is a single
    // fullscreen triangle, so there is nothing to lose.
    hr = dxgiDevice->SetMaximumFrameLatency(1);
    if (FAILED(hr))
        LOG_WARN("SetMaximumFrameLatency(1) failed: 0x{:08X}", static_cast<unsigned long>(hr));
    else
        LOG_INFO("Frame latency = 1 (the default is 3)");

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr))
    {
        LOG_ERROR("GetAdapter failed");
        return false;
    }

    hr = adapter->GetParent(IID_PPV_ARGS(&m_dxgiFactory));
    if (FAILED(hr))
    {
        LOG_ERROR("Could not obtain the DXGI factory");
        return false;
    }

    // Log which GPU we ended up on
    DXGI_ADAPTER_DESC adapterDesc{};
    adapter->GetDesc(&adapterDesc);
    LOG_INFO("GPU: {} (VRAM: {} MB)",
        ToUtf8(adapterDesc.Description),
        adapterDesc.DedicatedVideoMemory / (1024 * 1024));

    return true;
}

// =============================================================================
// CreateSamplerStates — how the source texture is filtered
// =============================================================================
//
// Bilinear for content: smooth enough through roughly 3x, which covers most
// use. A Lanczos shader would be a visible improvement past that and is not
// written yet.
//
// Point sampling exists separately for the cursor sprite, where crisp edges
// read better than a smooth blur. See RenderSprite.
// =============================================================================
bool D3DRenderer::CreateSamplerStates()
{
    // Bilinear sampler
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD         = 0;
    sampDesc.MaxLOD         = D3D11_FLOAT32_MAX;

    HRESULT hr = m_device->CreateSamplerState(&sampDesc, &m_samplerLinear);
    if (FAILED(hr))
    {
        LOG_ERROR("Linear sampler olusturulamadi: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    return true;
}

// =============================================================================
// CreateShaders — buyutme hattini kur
// =============================================================================
// Compiled at run time with D3DCompile. The alternative is fxc at build time
// and .cso files loaded from disk; that saves a few milliseconds once at
// startup in exchange for shipping loose files next to the exe. Not a trade
// worth making here.
// =============================================================================
bool D3DRenderer::CreateShaders()
{
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    auto compileStage = [&](const char* entryPoint,
                            const char* target,
                            ComPtr<ID3DBlob>& blob) -> bool
    {
        ComPtr<ID3DBlob> errors;
        // sizeof - 1: the trailing null is not part of the source length.
        const HRESULT compileHr = D3DCompile(
            kShaderSource, sizeof(kShaderSource) - 1,
            "BetterMagnifier.hlsl",
            nullptr, nullptr,
            entryPoint, target,
            compileFlags, 0,
            &blob, &errors);

        if (FAILED(compileHr))
        {
            LOG_ERROR("Shader derlenemedi ({}): 0x{:08X} — {}",
                entryPoint,
                static_cast<unsigned long>(compileHr),
                errors ? static_cast<const char*>(errors->GetBufferPointer())
                       : "derleyici mesaj vermedi");
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vsBlob;
    if (!compileStage("VSMain", "vs_5_0", vsBlob))
        return false;

    ComPtr<ID3DBlob> psBlob;
    if (!compileStage("PSMain", "ps_5_0", psBlob))
        return false;

    HRESULT hr = m_device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    if (FAILED(hr))
    {
        LOG_ERROR("CreateVertexShader failed: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    hr = m_device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    if (FAILED(hr))
    {
        LOG_ERROR("CreatePixelShader failed: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    // ── The sprite pass: cursor and readout ──
    ComPtr<ID3DBlob> spriteVsBlob;
    if (!compileStage("SpriteVS", "vs_5_0", spriteVsBlob))
        return false;

    ComPtr<ID3DBlob> spritePsBlob;
    if (!compileStage("SpritePS", "ps_5_0", spritePsBlob))
        return false;

    hr = m_device->CreateVertexShader(spriteVsBlob->GetBufferPointer(),
                                      spriteVsBlob->GetBufferSize(), nullptr, &m_spriteVS);
    if (FAILED(hr))
    {
        LOG_ERROR("Sprite VS olusturulamadi: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    hr = m_device->CreatePixelShader(spritePsBlob->GetBufferPointer(),
                                     spritePsBlob->GetBufferSize(), nullptr, &m_spritePS);
    if (FAILED(hr))
    {
        LOG_ERROR("Sprite PS olusturulamadi: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    // Premultiplied alpha, so the source factor is ONE. Straight alpha leaves a
    // kenarlarda hale olusuyor, ki buyutme onu tam da gorunur kildigi yer.
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable           = TRUE;
    blendDesc.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = m_device->CreateBlendState(&blendDesc, &m_alphaBlend);
    if (FAILED(hr))
    {
        LOG_ERROR("Alpha blend state olusturulamadi: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    // Point sampling for the sprite only. Kept here beside the rest of the
    // sprite state rather than with the content sampler, because the two make
    // opposite choices on purpose.
    D3D11_SAMPLER_DESC pointDesc{};
    pointDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    pointDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    pointDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    pointDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    pointDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    pointDesc.MaxLOD         = D3D11_FLOAT32_MAX;

    hr = m_device->CreateSamplerState(&pointDesc, &m_samplerPoint);
    if (FAILED(hr))
    {
        LOG_ERROR("Point sampler olusturulamadi: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_BUFFER_DESC spriteCb{};
    spriteCb.ByteWidth      = sizeof(float) * 8;
    spriteCb.Usage          = D3D11_USAGE_DEFAULT;
    spriteCb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    spriteCb.CPUAccessFlags = 0;

    hr = m_device->CreateBuffer(&spriteCb, nullptr, &m_spriteBuffer);
    if (FAILED(hr))
    {
        LOG_ERROR("Sprite constant buffer olusturulamadi: 0x{:08X}",
                  static_cast<unsigned long>(hr));
        return false;
    }

    // ── UV constant buffer ──
    // Written every frame with UpdateSubresource, so DEFAULT usage is enough.
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth      = sizeof(UvParams);
    cbDesc.Usage          = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = 0;

    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_uvBuffer);
    if (FAILED(hr))
    {
        LOG_ERROR("UV constant buffer olusturulamadi: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    // ── Rasterizer: culling KAPALI ──
    // Fullscreen ucgenin sarim yonu dogru olsa da, culling'i kapatmak
    // and rules out an entire class of "why is the screen black" faults.
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode        = D3D11_FILL_SOLID;
    rasterDesc.CullMode        = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;

    hr = m_device->CreateRasterizerState(&rasterDesc, &m_rasterNoCull);
    if (FAILED(hr))
    {
        LOG_ERROR("Rasterizer state olusturulamadi: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    LOG_DEBUG("Shader pipeline ready (fullscreen triangle + bilinear)");
    return true;
}

// =============================================================================
// CreateSwapChainForWindow — one swap chain per overlay window
// =============================================================================
//
// One swap chain per overlay window. Which swap effect it gets is not a free
// choice; see the comment on SwapEffect below.
// =============================================================================
bool D3DRenderer::CreateSwapChainForWindow(HWND hwnd, UINT width, UINT height, size_t index)
{
    if (!m_dxgiFactory || !m_device)
    {
        LOG_ERROR("CreateSwapChain: device or factory is not ready");
        return false;
    }

    if (index >= m_renderTargets.size())
    {
        m_renderTargets.resize(index + 1);
    }

    auto& rt = m_renderTargets[index];

    // Release any previous chain for this index before creating another.
    rt.rtv.Reset();
    rt.swapChain.Reset();

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width       = width;
    swapDesc.Height      = height;
    swapDesc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;   // BGRA, Direct2D compatible
    swapDesc.SampleDesc  = { 1, 0 };                     // no MSAA; nothing here is an edge
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // Flip model refuses layered windows: CreateSwapChainForHwnd returns
    // DXGI_ERROR_INVALID_CALL. The overlay has to be WS_EX_LAYERED to get
    // click-through (see OverlayWindow.cpp), so this falls back to blt.
    //
    // Blt costs an extra DWM copy and a little latency. It buys working input,
    // which is the whole point of the application.
    //
    // BufferCount 1 is the standard for blt DISCARD.
    if (UseFlipOverlay())
    {
        swapDesc.BufferCount = 2;
        swapDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    }
    else
    {
        swapDesc.BufferCount = 1;
        swapDesc.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
    }

    // An HWND swap chain does not support premultiplied alpha; only
    // CreateSwapChainForComposition does. The overlay is visually opaque
    // anyway, so IGNORE is the correct choice rather than a compromise.
    swapDesc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    swapDesc.Flags       = 0;

    HRESULT hr = m_dxgiFactory->CreateSwapChainForHwnd(
        m_device.Get(),
        hwnd,
        &swapDesc,
        nullptr,    // no fullscreen desc; the overlay is a windowed topmost
        nullptr,    // no output restriction
        &rt.swapChain
    );

    if (FAILED(hr))
    {
        LOG_ERROR("CreateSwapChainForHwnd failed: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = rt.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr))
    {
        LOG_ERROR("GetBuffer failed: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rt.rtv);
    if (FAILED(hr))
    {
        LOG_ERROR("CreateRenderTargetView failed: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    rt.targetWindow = hwnd;
    rt.width  = width;
    rt.height = height;

    // Alt+Tab ile fullscreen gecisi engelle
    m_dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    LOG_INFO("SwapChain olusturuldu: index={}, {}x{}", index, width, height);
    return true;
}

// =============================================================================
// RenderFrame — Captured texture'i zoom ile render et
// =============================================================================
bool D3DRenderer::RenderFrame(
    ID3D11Texture2D* srcTexture,
    size_t targetIndex,
    const RECT& srcRect,
    DXGI_MODE_ROTATION rotation)
{
    if (targetIndex >= m_renderTargets.size())
        return false;

    auto& rt = m_renderTargets[targetIndex];
    if (!rt.swapChain || !rt.rtv)
        return false;

    if (!m_vertexShader || !m_pixelShader || !m_uvBuffer)
        return false;

    // ── Yeni frame geldiyse kendi texture'imiza al ──
    if (srcTexture)
    {
        D3D11_TEXTURE2D_DESC srcDesc{};
        srcTexture->GetDesc(&srcDesc);

        if (!EnsureSourceTexture(rt, srcDesc))
            return false;

        // Unbind the SRV before copying. The same resource cannot be a shader
        // hem kopya hedefi olamaz — D3D11 debug layer uyarir ve islem duser.
        ID3D11ShaderResourceView* const noSrv[1] = { nullptr };
        m_context->PSSetShaderResources(0, 1, noSrv);

        m_context->CopyResource(rt.sourceTex.Get(), srcTexture);
    }

    // No frame has arrived yet, so there is nothing to draw. Normal at startup.
    if (!rt.sourceTex || !rt.sourceSrv)
        return false;

    // ── srcRect'i UV'ye cevir ──
    // Desktop pixel coordinates to normalised texture coordinates. The rotation
    // is absorbed here and nowhere else; ComputeSourceUv also does the clamping,
    // in desktop space, which is why none is left in this function.
    if (rt.sourceWidth == 0 || rt.sourceHeight == 0)
        return false;

    const SourceUvMapping uv =
        ComputeSourceUv(rotation, rt.sourceWidth, rt.sourceHeight, srcRect);

    UvParams params{};
    params.originX = uv.originX;
    params.originY = uv.originY;
    params.uAxisX  = uv.uAxisX;
    params.uAxisY  = uv.uAxisY;
    params.vAxisX  = uv.vAxisX;
    params.vAxisY  = uv.vAxisY;

    m_context->UpdateSubresource(m_uvBuffer.Get(), 0, nullptr, &params, 0, 0);

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width    = static_cast<float>(rt.width);
    viewport.Height   = static_cast<float>(rt.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    m_context->RSSetViewports(1, &viewport);
    m_context->RSSetState(m_rasterNoCull.Get());
    m_context->OMSetRenderTargets(1, rt.rtv.GetAddressOf(), nullptr);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    m_context->OMSetDepthStencilState(nullptr, 0);

    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_context->ClearRenderTargetView(rt.rtv.Get(), clearColor);

    // No vertex buffer and no input layout; positions come from SV_VertexID.
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_uvBuffer.GetAddressOf());

    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, rt.sourceSrv.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_samplerLinear.GetAddressOf());

    // Three vertices: the single triangle that covers the screen.
    m_context->Draw(3, 0);

    return true;
}

#ifdef _DEBUG
void D3DRenderer::MaybeDumpFrame(size_t targetIndex)
{
    // ── Frame dump (verification tool) ──
    //
    // The overlay is WDA_EXCLUDEFROMCAPTURE, so a screenshot cannot show what
    // we render. Dumping the back buffer before Present is the only outside
    // view of it.
    //
    // A sequence rather than a single frame, because the question this exists
    // for is whether something MOVES between frames: an occluded popup that
    // keeps repainting shows a menu highlight that tracks the mouse, a frozen
    // one does not. One frame cannot tell those apart.
    //
    // Called from Present rather than from RenderFrame so the cursor sprite,
    // which is composited in between, is in the dump. Still before the actual
    // Present call: FLIP_DISCARD leaves the back buffer undefined afterwards.
    //
    // BM_DUMP_FRAME    path prefix; files are <prefix>.NNN.bmp
    // BM_DUMP_AFTER    first frame to dump (default 60)
    // BM_DUMP_COUNT    how many to dump (default 1)
    // BM_DUMP_EVERY    frames between dumps (default 30)
    // BM_DUMP_MONITOR  which monitor to dump (default 0)
    //
    // The monitor filter matters: Render runs once per monitor per frame, so
    // without it a multi-frame dump alternates displays and the sequence is
    // uninterpretable.
    {
        static const auto readInt = [](const wchar_t* name, int fallback) {
            wchar_t buf[32]{};
            const DWORD n = GetEnvironmentVariableW(name, buf, 32);
            const int parsed = (n > 0 && n < 32) ? _wtoi(buf) : 0;
            return (parsed > 0) ? parsed : fallback;
        };

        static const std::wstring dumpPath = []() -> std::wstring {
            wchar_t buf[MAX_PATH]{};
            const DWORD n = GetEnvironmentVariableW(L"BM_DUMP_FRAME", buf, MAX_PATH);
            return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring{};
        }();

        if (!dumpPath.empty())
        {
            static const int dumpAtFrame  = readInt(L"BM_DUMP_AFTER",   60);
            static const int dumpCount    = readInt(L"BM_DUMP_COUNT",    1);
            static const int dumpEvery    = readInt(L"BM_DUMP_EVERY",   30);
            static const size_t dumpMonitor =
                static_cast<size_t>(readInt(L"BM_DUMP_MONITOR", 0));

            if (targetIndex == dumpMonitor)
            {
                static int frameCounter = 0;
                static int dumpsDone    = 0;

                ++frameCounter;
                if (dumpsDone < dumpCount &&
                    frameCounter >= dumpAtFrame &&
                    ((frameCounter - dumpAtFrame) % dumpEvery) == 0)
                {
                    wchar_t path[MAX_PATH]{};
                    swprintf_s(path, L"%ls.%03d.bmp", dumpPath.c_str(), dumpsDone);
                    if (DumpBackBuffer(targetIndex, path))
                        LOG_INFO("Dump {}/{} at frame {}", dumpsDone + 1, dumpCount,
                                 frameCounter);
                    ++dumpsDone;
                }
            }
        }
    }
}
#endif

// =============================================================================
// EnsureSourceTexture — an intermediate the shader can actually sample
// =============================================================================
// Desktop Duplication hands over textures without D3D11_BIND_SHADER_RESOURCE,
// so no SRV can be created on them. Every frame is copied into a texture of
// the same size and format that does have the bind flag.
//
// A no-op when nothing changed. Creating textures is expensive and has no
// business happening per frame.
// =============================================================================
bool D3DRenderer::EnsureSourceTexture(RenderTarget& rt, const D3D11_TEXTURE2D_DESC& srcDesc)
{
    if (rt.sourceTex && rt.sourceSrv
        && rt.sourceWidth  == srcDesc.Width
        && rt.sourceHeight == srcDesc.Height
        && rt.sourceFormat == srcDesc.Format)
    {
        return true;
    }

    rt.sourceSrv.Reset();
    rt.sourceTex.Reset();
    rt.sourceWidth  = 0;
    rt.sourceHeight = 0;
    rt.sourceFormat = DXGI_FORMAT_UNKNOWN;

    // Start from the source description but set the flags here: the sharing
    // and staging flags the duplication texture carries must not come along.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = srcDesc.Width;
    desc.Height         = srcDesc.Height;
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = srcDesc.Format;
    desc.SampleDesc     = { 1, 0 };
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags      = 0;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &rt.sourceTex);
    if (FAILED(hr))
    {
        LOG_ERROR("Ara kaynak texture'i olusturulamadi ({}x{}): 0x{:08X}",
            srcDesc.Width, srcDesc.Height, static_cast<unsigned long>(hr));
        return false;
    }

    hr = m_device->CreateShaderResourceView(rt.sourceTex.Get(), nullptr, &rt.sourceSrv);
    if (FAILED(hr))
    {
        LOG_ERROR("Could not create the SRV for the intermediate texture: 0x{:08X}",
            static_cast<unsigned long>(hr));
        rt.sourceTex.Reset();
        return false;
    }

    rt.sourceWidth  = srcDesc.Width;
    rt.sourceHeight = srcDesc.Height;
    rt.sourceFormat = srcDesc.Format;

    LOG_DEBUG("Intermediate source texture created: {}x{}", srcDesc.Width, srcDesc.Height);
    return true;
}

// =============================================================================
// Present — put the rendered frame on screen
// =============================================================================
// =============================================================================
// RenderSprite — composite the cursor over the magnified content
// =============================================================================
//
// Called between RenderFrame and Present, so it composites onto the frame that
// is already there rather than clearing it.
// =============================================================================
bool D3DRenderer::RenderSprite(size_t targetIndex, ID3D11ShaderResourceView* srv,
                               float x, float y, float width, float height,
                               float opacity)
{
    if (targetIndex >= m_renderTargets.size() || !srv || !m_spriteVS || !m_spritePS)
        return false;

    if (opacity <= 0.0f)
        return false;

    RenderTarget& rt = m_renderTargets[targetIndex];
    if (!rt.rtv || rt.width == 0 || rt.height == 0)
        return false;

    // Wholly off-target: nothing to do, and a degenerate rect is not worth
    // sending to the GPU.
    if (width <= 0.0f || height <= 0.0f ||
        x + width <= 0.0f || y + height <= 0.0f ||
        x >= static_cast<float>(rt.width) || y >= static_cast<float>(rt.height))
        return false;

    // Pixels -> NDC. Y flips: NDC grows upward, pixels downward.
    const float fw = static_cast<float>(rt.width);
    const float fh = static_cast<float>(rt.height);

    const float params[8] = {
        (x / fw) * 2.0f - 1.0f,
        1.0f - (y / fh) * 2.0f,
        ((x + width)  / fw) * 2.0f - 1.0f,
        1.0f - ((y + height) / fh) * 2.0f,
        std::min(opacity, 1.0f), 0.0f, 0.0f, 0.0f,
    };

    m_context->UpdateSubresource(m_spriteBuffer.Get(), 0, nullptr, params, 0, 0);

    m_context->OMSetRenderTargets(1, rt.rtv.GetAddressOf(), nullptr);

    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_context->OMSetBlendState(m_alphaBlend.Get(), blendFactor, 0xFFFFFFFFu);

    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    m_context->VSSetShader(m_spriteVS.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_spriteBuffer.GetAddressOf());

    m_context->PSSetShader(m_spritePS.Get(), nullptr, 0);
    // The pixel shader reads spriteFade out of the same buffer. Bound to the VS
    // alone it reads zeros there, and every sprite comes out fully transparent.
    m_context->PSSetConstantBuffers(0, 1, m_spriteBuffer.GetAddressOf());
    m_context->PSSetShaderResources(0, 1, &srv);
    m_context->PSSetSamplers(0, 1, m_samplerPoint.GetAddressOf());

    m_context->Draw(4, 0);

    // Leave blending off so the next frame's content pass is unaffected.
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    return true;
}

void D3DRenderer::Present(size_t targetIndex, bool vSync)
{
#ifdef _DEBUG
    MaybeDumpFrame(targetIndex);
#endif

    if (targetIndex >= m_renderTargets.size())
        return;

    auto& rt = m_renderTargets[targetIndex];
    if (!rt.swapChain)
        return;

    // SyncInterval:
    //   0 = V-Sync KAPALI — aninda goster (tearing olabilir ama dusuk latency)
    //   1 = V-Sync ACIK — monitor refresh rate'ine senkronize (60/144 FPS)
    UINT syncInterval = vSync ? 1 : 0;
    HRESULT hr = rt.swapChain->Present(syncInterval, 0);

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        LOG_ERROR("GPU device lost! Reason: 0x{:08X}",
            static_cast<unsigned long>(m_device->GetDeviceRemovedReason()));
        // TODO: Full device recovery
    }
}


// =============================================================================
// RemoveRenderTarget
// =============================================================================
void D3DRenderer::RemoveRenderTarget(size_t index)
{
    if (index < m_renderTargets.size())
    {
        auto& rt = m_renderTargets[index];
        rt.sourceSrv.Reset();
        rt.sourceTex.Reset();
        rt.sourceWidth  = 0;
        rt.sourceHeight = 0;
        rt.sourceFormat = DXGI_FORMAT_UNKNOWN;
        rt.rtv.Reset();
        rt.swapChain.Reset();
        rt.targetWindow = nullptr;
    }
}

#ifdef _DEBUG
// =============================================================================
// DumpBackBuffer — write the rendered frame to disk (Debug only)
// =============================================================================
// The overlay is invisible to screenshot tools, so this is the only way to see
// what was actually rendered.
//
// back buffer (GPU) -> staging texture (CPU readable) -> Map -> BMP. GPU memory
// cannot be read directly from the CPU; a STAGING texture exists for exactly
// this.
//
// =============================================================================
bool D3DRenderer::DumpBackBuffer(size_t targetIndex, const wchar_t* path)
{
    if (targetIndex >= m_renderTargets.size() || !path)
        return false;

    auto& rt = m_renderTargets[targetIndex];
    if (!rt.swapChain)
        return false;

    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = rt.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr))
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage          = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags      = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags      = 0;

    ComPtr<ID3D11Texture2D> staging;
    hr = m_device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr))
    {
        LOG_ERROR("Dump: staging texture olusturulamadi: 0x{:08X}",
            static_cast<unsigned long>(hr));
        return false;
    }

    m_context->CopyResource(staging.Get(), backBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        LOG_ERROR("Dump: Map failed: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    // ── 32-bit BMP yaz ──
    // biHeight NEGATIF = top-down satir sirasi (bizim bellek duzenimiz boyle).
    const DWORD pixelBytes = desc.Width * desc.Height * 4u;

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType    = 0x4D42;  // "BM"
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize    = fileHeader.bfOffBits + pixelBytes;

    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize        = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth       = static_cast<LONG>(desc.Width);
    infoHeader.biHeight      = -static_cast<LONG>(desc.Height);
    infoHeader.biPlanes      = 1;
    infoHeader.biBitCount    = 32;
    infoHeader.biCompression = BI_RGB;

    bool ok = false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
        WriteFile(file, &infoHeader, sizeof(infoHeader), &written, nullptr);

        // Satir satir yaziyoruz: GPU'nun satir adimi (RowPitch) genelde
        // genislikten buyuk (hizalama dolgusu). Dolguyu atlamak zorundayiz.
        const auto* src = static_cast<const uint8_t*>(mapped.pData);
        for (UINT y = 0; y < desc.Height; ++y)
        {
            WriteFile(file, src + static_cast<size_t>(y) * mapped.RowPitch,
                      desc.Width * 4u, &written, nullptr);
        }

        CloseHandle(file);
        ok = true;
        LOG_INFO("Dump: back buffer written ({}x{}) -> {}",
            desc.Width, desc.Height, "BM_DUMP_FRAME");
    }
    else
    {
        LOG_ERROR("Dump: dosya acilamadi ({})", GetLastError());
    }

    m_context->Unmap(staging.Get(), 0);
    return ok;
}

// =============================================================================
// D3DRendererSelfCheck — the rotation transform, asserted without a GPU
// =============================================================================
// Everything checked here is ComputeSourceUv, which is why it was pulled out of
// RenderFrame in the first place: the sideways-portrait bug was a pure geometry
// error sitting inside a function that needs a device, a swap chain and a live
// duplication session to reach.
//
// The reference numbers are not derived from the DXGI documentation, which is
// ambiguous about which way ROTATE270 turns. They come from a measurement: a
// 1080x1920 portrait monitor's duplication texture was resampled with each
// candidate mapping and compared against a GDI screenshot of the same monitor.
// The counter-clockwise one reproduced it exactly (mean |difference| 0.00 per
// channel); the others scored 40-51.
// =============================================================================
void D3DRendererSelfCheck()
{
    constexpr float kEps = 1e-6f;

    // UV at a corner of the destination region. cx/cy are 0..1 across the
    // magnified view: (0,0) its top-left, (1,1) its bottom-right.
    const auto uvX = [](const SourceUvMapping& m, float cx, float cy) {
        return m.originX + cx * m.uAxisX + cy * m.vAxisX;
    };
    const auto uvY = [](const SourceUvMapping& m, float cx, float cy) {
        return m.originY + cx * m.uAxisY + cy * m.vAxisY;
    };
    const auto approx = [](float a, float b) { return std::abs(a - b) < 1e-6f; };

    // ── Desktop extent: swapped for the quarter turns, untouched otherwise ──
    {
        long w = 0, h = 0;
        DesktopExtentForRotation(DXGI_MODE_ROTATION_IDENTITY, 1920, 1080, w, h);
        BM_SELFCHECK(w == 1920 && h == 1080);

        DesktopExtentForRotation(DXGI_MODE_ROTATION_ROTATE180, 1920, 1080, w, h);
        BM_SELFCHECK(w == 1920 && h == 1080);

        DesktopExtentForRotation(DXGI_MODE_ROTATION_ROTATE90, 1920, 1080, w, h);
        BM_SELFCHECK(w == 1080 && h == 1920);

        // The reported bug's exact topology: a 1080x1920 desktop rectangle on a
        // 1920x1080 panel.
        DesktopExtentForRotation(DXGI_MODE_ROTATION_ROTATE270, 1920, 1080, w, h);
        BM_SELFCHECK(w == 1080 && h == 1920);
    }

    // ── Unrotated, whole frame: the identity mapping ──
    {
        const SourceUvMapping m = ComputeSourceUv(
            DXGI_MODE_ROTATION_IDENTITY, 1920, 1080, RECT{ 0, 0, 1920, 1080 });

        BM_SELFCHECK(approx(m.originX, 0.0f) && approx(m.originY, 0.0f));
        BM_SELFCHECK(approx(m.uAxisX, 1.0f)  && approx(m.uAxisY, 0.0f));
        BM_SELFCHECK(approx(m.vAxisX, 0.0f)  && approx(m.vAxisY, 1.0f));
    }

    // ── Unrotated, 2x on the centre ──
    {
        const SourceUvMapping m = ComputeSourceUv(
            DXGI_MODE_ROTATION_IDENTITY, 1920, 1080, RECT{ 480, 270, 1440, 810 });

        BM_SELFCHECK(approx(m.originX, 0.25f) && approx(m.originY, 0.25f));
        BM_SELFCHECK(approx(m.uAxisX, 0.5f)   && approx(m.uAxisY, 0.0f));
        BM_SELFCHECK(approx(m.vAxisX, 0.0f)   && approx(m.vAxisY, 0.5f));
    }

    // ── ROTATE270, whole frame ──
    //
    // The desktop's top-left corner is the texture's TOP-RIGHT, and walking
    // right across the desktop walks DOWN the texture. That is the measured
    // counter-clockwise mapping, and asserting the corners individually is what
    // makes a 90-degree sign error impossible to miss.
    {
        const SourceUvMapping m = ComputeSourceUv(
            DXGI_MODE_ROTATION_ROTATE270, 1920, 1080, RECT{ 0, 0, 1080, 1920 });

        BM_SELFCHECK(approx(uvX(m, 0, 0), 1.0f) && approx(uvY(m, 0, 0), 0.0f));  // top-right
        BM_SELFCHECK(approx(uvX(m, 1, 0), 1.0f) && approx(uvY(m, 1, 0), 1.0f));  // bottom-right
        BM_SELFCHECK(approx(uvX(m, 0, 1), 0.0f) && approx(uvY(m, 0, 1), 0.0f));  // top-left
        BM_SELFCHECK(approx(uvX(m, 1, 1), 0.0f) && approx(uvY(m, 1, 1), 1.0f));  // bottom-left

        // The regression guard. Before the fix the mapping was a plain scale,
        // so uAxis ran along the texture's x. On a quarter turn it must not.
        BM_SELFCHECK(approx(m.uAxisX, 0.0f) && std::abs(m.uAxisY) > kEps);
        BM_SELFCHECK(approx(m.vAxisY, 0.0f) && std::abs(m.vAxisX) > kEps);
    }

    // ── ROTATE90, whole frame: the other quarter turn, corners mirrored ──
    {
        const SourceUvMapping m = ComputeSourceUv(
            DXGI_MODE_ROTATION_ROTATE90, 1920, 1080, RECT{ 0, 0, 1080, 1920 });

        BM_SELFCHECK(approx(uvX(m, 0, 0), 0.0f) && approx(uvY(m, 0, 0), 1.0f));  // bottom-left
        BM_SELFCHECK(approx(uvX(m, 1, 0), 0.0f) && approx(uvY(m, 1, 0), 0.0f));  // top-left
        BM_SELFCHECK(approx(uvX(m, 0, 1), 1.0f) && approx(uvY(m, 0, 1), 1.0f));  // bottom-right
        BM_SELFCHECK(approx(uvX(m, 1, 1), 1.0f) && approx(uvY(m, 1, 1), 0.0f));  // top-right
    }

    // ── ROTATE180, 2x on the centre: the region flips about both axes ──
    {
        const SourceUvMapping m = ComputeSourceUv(
            DXGI_MODE_ROTATION_ROTATE180, 1920, 1080, RECT{ 480, 270, 1440, 810 });

        BM_SELFCHECK(approx(uvX(m, 0, 0), 0.75f) && approx(uvY(m, 0, 0), 0.75f));
        BM_SELFCHECK(approx(uvX(m, 1, 1), 0.25f) && approx(uvY(m, 1, 1), 0.25f));
    }

    // ── Every rotation, zoomed and panned: nothing may leave the texture ──
    //
    // The failure this catches is the one the user sees as a view that "goes
    // mad": a mapping that runs off the edge of the texture clamps against the
    // sampler instead, which smears the last row or column across the screen.
    {
        const DXGI_MODE_ROTATION kRotations[] = {
            DXGI_MODE_ROTATION_UNSPECIFIED, DXGI_MODE_ROTATION_IDENTITY,
            DXGI_MODE_ROTATION_ROTATE90,    DXGI_MODE_ROTATION_ROTATE180,
            DXGI_MODE_ROTATION_ROTATE270,
        };

        for (DXGI_MODE_ROTATION rot : kRotations)
        {
            long deskW = 0, deskH = 0;
            DesktopExtentForRotation(rot, 1920, 1080, deskW, deskH);

            for (int zoom = 1; zoom <= 8; ++zoom)
            {
                const long w = (std::max)(1L, deskW / zoom);
                const long h = (std::max)(1L, deskH / zoom);

                // Top-left, bottom-right, and deliberately out of bounds.
                const RECT kRects[] = {
                    RECT{ 0, 0, w, h },
                    RECT{ deskW - w, deskH - h, deskW, deskH },
                    RECT{ -500, -500, w, h },
                    RECT{ deskW - w, deskH - h, deskW + 500, deskH + 500 },
                };

                for (const RECT& r : kRects)
                {
                    const SourceUvMapping m = ComputeSourceUv(rot, 1920, 1080, r);

                    for (float cx : { 0.0f, 1.0f })
                    {
                        for (float cy : { 0.0f, 1.0f })
                        {
                            const float u = uvX(m, cx, cy);
                            const float v = uvY(m, cx, cy);
                            BM_SELFCHECK(u >= -kEps && u <= 1.0f + kEps);
                            BM_SELFCHECK(v >= -kEps && v <= 1.0f + kEps);
                        }
                    }

                    // A quarter turn swaps which texture axis the view's width
                    // spans. Getting this backwards is the sideways bug itself,
                    // and it survives the bounds test above unnoticed.
                    const float uSpan = std::abs(uvX(m, 1, 0) - uvX(m, 0, 0))
                                      + std::abs(uvY(m, 1, 0) - uvY(m, 0, 0));
                    BM_SELFCHECK(uSpan > kEps);
                }
            }
        }
    }

    // ── A zero-sized texture must not divide by zero ──
    {
        const SourceUvMapping m = ComputeSourceUv(
            DXGI_MODE_ROTATION_IDENTITY, 0, 0, RECT{ 0, 0, 100, 100 });
        BM_SELFCHECK(approx(m.uAxisX, 0.0f) && approx(m.vAxisY, 0.0f));
    }

    LOG_INFO("D3DRenderer self-check passed");
}
#endif // _DEBUG

} // namespace BetterMagnifier
