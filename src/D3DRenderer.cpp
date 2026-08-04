// =============================================================================
// D3DRenderer.cpp — DirectX 11 Render Pipeline Implementation
// =============================================================================

#include "pch.h"
#include "D3DRenderer.h"
#include "Logger.h"

namespace BetterMagnifier {

// =============================================================================
// Destructor
// =============================================================================
D3DRenderer::~D3DRenderer()
{
    // Render target'lari temizle (swap chain'ler dahil)
    m_renderTargets.clear();

    // Sampler state'leri
    m_samplerLinear.Reset();
    m_samplerPoint.Reset();

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

    LOG_INFO("D3DRenderer basariyla baslatildi (Feature Level: 0x{:X})",
        static_cast<unsigned int>(m_featureLevel));

    return true;
}

// =============================================================================
// CreateDevice — D3D11 Device + Context + DXGI Factory
// =============================================================================
//
// Python analojisi:
//   pygame.display.init() veya moderngl.create_context()
//   D3D11 Device = GPU ile konusma noktasi (komut gonderme)
//   DeviceContext = "hemen simdi su komutu calistir" (immediate context)
//
// Debug Layer:
//   Debug build'de D3D11_CREATE_DEVICE_DEBUG flag ekliyoruz.
//   Bu flag ile DirectX validation layer aktif olur:
//   - Yanlis API kullanimi varsa Output penceresinde uyari/hata gosterir
//   - Memory leak'leri raporlar (ReportLiveDeviceObjects)
//   - Performans etkisi var ama debug icin goldmine!
//
// =============================================================================
bool D3DRenderer::CreateDevice()
{
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // Direct2D interop icin gerekli

#ifdef _DEBUG
    // Debug build'de DirectX debug layer aktif
    // VS2022 Output penceresinde DirectX hatalarini gosterir
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
    LOG_DEBUG("D3D11 Debug Layer AKTIF");
#endif

    // Feature level listesi — en yüksekten en düşüğe dene
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    ComPtr<ID3D11Device> baseDevice;
    ComPtr<ID3D11DeviceContext> baseContext;

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // Varsayilan adapter (primary GPU)
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
        LOG_ERROR("D3D11CreateDevice basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));

        // Debug layer yuklu degilse, onsuz tekrar dene
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            LOG_WARN("Debug layer bulunamadi, debug'suz deneniyor...");
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
            LOG_ERROR("D3D11CreateDevice tamamen basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
            return false;
        }
    }

    // ID3D11Device1'e upgrade (DXGI 1.2 swap chain icin gerekli)
    hr = baseDevice.As(&m_device);
    if (FAILED(hr))
    {
        LOG_ERROR("ID3D11Device1 QueryInterface basarisiz");
        return false;
    }

    hr = baseContext.As(&m_context);
    if (FAILED(hr))
    {
        LOG_ERROR("ID3D11DeviceContext1 QueryInterface basarisiz");
        return false;
    }

#ifdef _DEBUG
    // Debug interface'i al (live object report icin)
    m_device.As(&m_debug);
#endif

    // ── DXGI Factory al ──
    // Device'in bagli oldugu DXGI Factory'yi kullanmaliyiz.
    // Yeni bir Factory olusturmak yerine mevcut olani almak best practice.
    ComPtr<IDXGIDevice1> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr))
    {
        LOG_ERROR("IDXGIDevice1 QueryInterface basarisiz");
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr))
    {
        LOG_ERROR("GetAdapter basarisiz");
        return false;
    }

    hr = adapter->GetParent(IID_PPV_ARGS(&m_dxgiFactory));
    if (FAILED(hr))
    {
        LOG_ERROR("DXGI Factory al basarisiz");
        return false;
    }

    // GPU bilgilerini logla
    DXGI_ADAPTER_DESC adapterDesc{};
    adapter->GetDesc(&adapterDesc);
    LOG_INFO("GPU: {} (VRAM: {} MB)",
        ToUtf8(adapterDesc.Description),
        adapterDesc.DedicatedVideoMemory / (1024 * 1024));

    return true;
}

// =============================================================================
// CreateSamplerStates — Texture sampling kalitesi
// =============================================================================
//
// Sampler state = texture'dan piksel okurken nasil filtre uygulayacagi.
//
// Bilinear: Komsulari ortalayarak yumusak gecis yapar.
//   Zoom 1x-3x arasi icin yeterli kalite.
//   Python analojisi: PIL.Image.resize(size, Image.BILINEAR)
//
// Point (Nearest Neighbor): En yakin pikseli alir, filtre yok.
//   Piksel-art veya 1:1 mapping icin.
//   Python analojisi: PIL.Image.resize(size, Image.NEAREST)
//
// Lanczos shader ileride eklenecek (zoom >3x icin belirgin kalite fark1).
//
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

    // Point sampler
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = m_device->CreateSamplerState(&sampDesc, &m_samplerPoint);
    if (FAILED(hr))
    {
        LOG_ERROR("Point sampler olusturulamadi: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    return true;
}

// =============================================================================
// CreateSwapChainForWindow — Overlay penceresi icin swap chain
// =============================================================================
//
// Swap Chain nedir?
//   GPU'nun render ettigi image'i ekrana gostermek icin kullanilan mekanizma.
//   "Flip model" (DXGI_SWAP_EFFECT_FLIP_DISCARD):
//     - Modern ve verimli (eski BITBLT'ye gore)
//     - GPU composition ile entegre (DWM = Desktop Window Manager)
//     - V-Sync destegi dahili
//
//   Python analojisi: pygame.display.flip() veya tkinter canvas.update()
//
// =============================================================================
bool D3DRenderer::CreateSwapChainForWindow(HWND hwnd, UINT width, UINT height, size_t index)
{
    if (!m_dxgiFactory || !m_device)
    {
        LOG_ERROR("CreateSwapChain: Device veya factory hazir degil!");
        return false;
    }

    // index'e gore render target listesini buyut
    if (index >= m_renderTargets.size())
    {
        m_renderTargets.resize(index + 1);
    }

    auto& rt = m_renderTargets[index];

    // Eski swap chain varsa temizle
    rt.rtv.Reset();
    rt.swapChain.Reset();

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width       = width;
    swapDesc.Height      = height;
    swapDesc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;  // BGRA — Direct2D uyumlu
    swapDesc.SampleDesc  = { 1, 0 };                     // No MSAA (overlay'de gerek yok)
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;                             // Double buffering
    swapDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD; // Modern flip model
    // HWND swap chain'de PREMULTIPLIED alpha DESTEKLENMEZ — sadece
    // CreateSwapChainForComposition (DirectComposition) kabul eder.
    // Overlay opak oldugu icin IGNORE dogru secim (bkz. OverlayWindow.cpp).
    swapDesc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    swapDesc.Flags       = 0;

    HRESULT hr = m_dxgiFactory->CreateSwapChainForHwnd(
        m_device.Get(),
        hwnd,
        &swapDesc,
        nullptr,    // Fullscreen desc (windowed kullanacagiz)
        nullptr,    // Output restriction (yok)
        &rt.swapChain
    );

    if (FAILED(hr))
    {
        LOG_ERROR("CreateSwapChainForHwnd basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    // ── Render Target View olustur ──
    // Swap chain'in back buffer'ini render hedefi olarak kullanmak icin RTV gerekli.
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = rt.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr))
    {
        LOG_ERROR("GetBuffer basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rt.rtv);
    if (FAILED(hr))
    {
        LOG_ERROR("CreateRenderTargetView basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
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
void D3DRenderer::RenderFrame(
    ID3D11Texture2D* srcTexture,
    size_t targetIndex,
    const RECT& srcRect)
{
    if (targetIndex >= m_renderTargets.size())
        return;

    auto& rt = m_renderTargets[targetIndex];
    if (!rt.swapChain || !rt.rtv)
        return;

    if (!srcTexture)
        return;

    // NOT: Burada SRV olusturmaya CALISMIYORUZ.
    // Desktop Duplication texture'lari D3D11_BIND_SHADER_RESOURCE flag'i
    // OLMADAN gelir — CreateShaderResourceView her zaman basarisiz olur.
    // Bu asamada CopySubresourceRegion kullaniyoruz, o SRV gerektirmiyor.
    // Shader tabanli olceklendirmeye gecince (Adim 4) DD texture'i once
    // SRV bind flag'li ara bir texture'a kopyalamak gerekecek.

    // ── Viewport ayarla ──
    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width    = static_cast<float>(rt.width);
    viewport.Height   = static_cast<float>(rt.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    m_context->RSSetViewports(1, &viewport);
    m_context->OMSetRenderTargets(1, rt.rtv.GetAddressOf(), nullptr);

    // ── Clear ──
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // Transparent
    m_context->ClearRenderTargetView(rt.rtv.Get(), clearColor);

    // ── Source rect'ten crop + scale ──
    // srcRect: kaynak texture'dan kesilecek bolge
    // Bu bolge, zoom level ve focal point'e gore hesaplaniyor.
    //
    // Basit yaklasim: CopySubresourceRegion ile crop, sonra full-screen quad ile render.
    // Simdilik CopySubresourceRegion + StretchRect equivalent kullaniyoruz.
    // Shader-based render (vertex/pixel shader) ileride eklenecek.

    // Source texture boyutlarini al
    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTexture->GetDesc(&srcDesc);

    // Clamp source rect to texture bounds
    D3D11_BOX sourceBox{};
    sourceBox.left   = static_cast<UINT>(std::clamp(static_cast<long>(srcRect.left), 0L, static_cast<long>(srcDesc.Width)));
    sourceBox.top    = static_cast<UINT>(std::clamp(static_cast<long>(srcRect.top), 0L, static_cast<long>(srcDesc.Height)));
    sourceBox.right  = static_cast<UINT>(std::clamp(static_cast<long>(srcRect.right), 0L, static_cast<long>(srcDesc.Width)));
    sourceBox.bottom = static_cast<UINT>(std::clamp(static_cast<long>(srcRect.bottom), 0L, static_cast<long>(srcDesc.Height)));
    sourceBox.front  = 0;
    sourceBox.back   = 1;

    // Gecici texture'a crop
    UINT cropW = sourceBox.right - sourceBox.left;
    UINT cropH = sourceBox.bottom - sourceBox.top;

    if (cropW == 0 || cropH == 0)
        return;

    // Back buffer'a kopyala.
    // ponytail: CopySubresourceRegion 1:1 kopyalar — OLCEKLEME YAPMAZ.
    // Yani su an zoom seviyesi kirpma bolgesini kucultuyor ama goruntu
    // buyumuyor. Gercek zoom icin shader pipeline gerekli (Adim 4):
    // fullscreen quad + linear sampler + srcRect'i UV'ye ceviren constant buffer.
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = rt.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr))
    {
        // KRITIK: Back buffer su an render target olarak BAGLI.
        // Bagli bir kaynaga CopySubresourceRegion yapilamaz — D3D11 debug layer
        // "resource is bound as render target" hatasi verir ve kopya sessizce duser.
        // Once RTV'yi cikar.
        m_context->OMSetRenderTargets(0, nullptr, nullptr);

        m_context->CopySubresourceRegion(
            backBuffer.Get(), 0,    // Hedef
            0, 0, 0,                // Hedef offset
            srcTexture, 0,          // Kaynak
            &sourceBox              // Kaynak bolgesi
        );
    }
}

// =============================================================================
// Present — Render edilen frame'i ekrana goster
// =============================================================================
void D3DRenderer::Present(size_t targetIndex, bool vSync)
{
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
// ResizeTarget
// =============================================================================
void D3DRenderer::ResizeTarget(size_t targetIndex, UINT width, UINT height)
{
    if (targetIndex >= m_renderTargets.size())
        return;

    auto& rt = m_renderTargets[targetIndex];
    if (!rt.swapChain)
        return;

    // RTV'yi release et (resize oncesi zorunlu)
    rt.rtv.Reset();
    m_context->Flush();

    HRESULT hr = rt.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
    {
        LOG_ERROR("ResizeBuffers basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
        return;
    }

    // Yeni RTV olustur
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = rt.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr))
    {
        m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rt.rtv);
    }

    rt.width  = width;
    rt.height = height;

    LOG_DEBUG("SwapChain resize: index={}, {}x{}", targetIndex, width, height);
}

// =============================================================================
// RemoveRenderTarget
// =============================================================================
void D3DRenderer::RemoveRenderTarget(size_t index)
{
    if (index < m_renderTargets.size())
    {
        m_renderTargets[index].rtv.Reset();
        m_renderTargets[index].swapChain.Reset();
        m_renderTargets[index].targetWindow = nullptr;
    }
}

// =============================================================================
// EnsureShaderResourceView
// =============================================================================
bool D3DRenderer::EnsureShaderResourceView(
    ID3D11Texture2D* texture,
    ComPtr<ID3D11ShaderResourceView>& srv)
{
    if (!texture)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                    = desc.Format;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels       = 1;

    HRESULT hr = m_device->CreateShaderResourceView(texture, &srvDesc, &srv);
    if (FAILED(hr))
    {
        // Desktop Duplication texture'lari genellikle SRV bind flag'i olmadan gelir.
        // Bu durumda ara (staging) texture'a kopyalamak gerekir.
        // Simdilik hatayi sessizce gec — CopySubresourceRegion zaten SRV gerektirmiyor.
        return false;
    }

    return true;
}

} // namespace BetterMagnifier
