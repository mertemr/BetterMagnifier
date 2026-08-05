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
// Shader kaynagi — buyutmeyi yapan asil is burada
// =============================================================================
// Eski kod CopySubresourceRegion kullaniyordu; o 1:1 piksel kopyalar,
// OLCEKLEME YAPMAZ. Sonuc: zoom seviyesi kaynak bolgeyi kucultuyordu ama
// goruntu buyumuyordu — 2x'te ekranin sol ust ceyregine kirpilmis bir kopya.
//
// Dogru cozum: kaynak bolgeyi (srcRect) UV koordinatina cevirip TUM ekrani
// kaplayan bir ucgene bilinear ornekleme ile cizmek. Boylece kucuk bolge
// ekran boyutuna GERILIR = buyutme.
//
// Neden ucgen, dortgen degil?
//   Ekrani kaplayan tek bir ucgen, iki ucgenden olusan bir quad'a gore hem
//   daha az vertex hem de diagonal boyunca tekrar ornekleme yapmiyor.
//   Standart "fullscreen triangle" hilesi.
//
// Neden vertex buffer yok?
//   SV_VertexID = kacinci vertex oldugumuz. Uc kosenin konumunu bundan
//   hesapliyoruz. Vertex buffer da input layout da gerekmiyor.
//   Python analojisi: for i in range(3) icinde koordinati hesaplamak,
//   onceden hazirlanmis bir liste okumak yerine.
// =============================================================================
constexpr char kShaderSource[] = R"HLSL(
cbuffer UvParams : register(b0)
{
    // xy = buyutulecek bolgenin sol-ust UV'si, zw = bolgenin UV boyutu
    float4 uvRect;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // vid 0,1,2 -> corner (0,0), (2,0), (0,2)
    // Ekrani tasan buyuk bir ucgen; ekran disi kisimlar kirpiliyor.
    float2 corner = float2((vid << 1) & 2, vid & 2);

    VSOut o;
    // corner 0..2 -> NDC -1..3 (x), 1..-3 (y). Y ters cevriliyor cunku
    // NDC'de yukari +1, texture UV'sinde asagi +1.
    o.pos = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = uvRect.xy + corner * uvRect.zw;
    return o;
}

Texture2D    srcTex : register(t0);
SamplerState srcSmp : register(s0);

float4 PSMain(VSOut input) : SV_TARGET
{
    // Alpha 1.0 sabit: swap chain AlphaMode IGNORE, overlay opak.
    return float4(srcTex.Sample(srcSmp, input.uv).rgb, 1.0);
}
)HLSL";

// Constant buffer duzeni. D3D11 sabit tampon boyutunu 16'nin kati istiyor —
// tek float4 tam olarak 16 bayt.
struct UvParams
{
    float uvLeft;
    float uvTop;
    float uvWidth;
    float uvHeight;
};

} // anonymous namespace

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

    // Shader hatti
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

    // ── Frame latency = 1 ──
    // DXGI varsayilani 3 frame kuyruk tutuyor. 75 Hz'de bu 40 ms'ye kadar
    // girdi-ekran gecikmesi demek: fareyi oynatiyorsun, goruntu uc kare
    // sonra yetisiyor. Bir magnifier'da bu "kasma" olarak hissediliyor.
    //
    // 1'e cekmek gecikmeyi tek kareye indiriyor. Bedeli: GPU ile CPU arasinda
    // daha az tampon, yani cok agir sahnelerde throughput dusebilir — bizim
    // is yukumuz tek bir fullscreen ucgen, sorun olmaz.
    hr = dxgiDevice->SetMaximumFrameLatency(1);
    if (FAILED(hr))
        LOG_WARN("SetMaximumFrameLatency(1) basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
    else
        LOG_INFO("Frame latency = 1 (varsayilan 3)");

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
// CreateShaders — buyutme hattini kur
// =============================================================================
// Shader'lari calisma zamaninda derliyoruz (D3DCompile). Alternatif, build
// sirasinda fxc ile .cso uretip dosyadan okumakti; runtime derleme tek seferlik
// birkac ms surer ve exe'nin yaninda ekstra dosya tasimayi gerektirmez.
//
// Python analojisi: kaynak kodu string olarak tutup exec() ile derlemek —
// ama burada hedef GPU byte code.
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
        // sizeof - 1: sondaki null terminator kaynak uzunluguna dahil degil.
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
        LOG_ERROR("CreateVertexShader basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    hr = m_device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    if (FAILED(hr))
    {
        LOG_ERROR("CreatePixelShader basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    // ── UV constant buffer ──
    // Her frame UpdateSubresource ile yaziyoruz, DEFAULT usage yeterli.
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
    // "ekran neden siyah" sinifindan bir hata ihtimalini komple eliyor.
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

    LOG_DEBUG("Shader hatti hazir (fullscreen ucgen + bilinear)");
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
bool D3DRenderer::RenderFrame(
    ID3D11Texture2D* srcTexture,
    size_t targetIndex,
    const RECT& srcRect)
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

        // Kopyalamadan once SRV bagini kes. Ayni kaynak hem shader girdisi
        // hem kopya hedefi olamaz — D3D11 debug layer uyarir ve islem duser.
        ID3D11ShaderResourceView* const noSrv[1] = { nullptr };
        m_context->PSSetShaderResources(0, 1, noSrv);

        m_context->CopyResource(rt.sourceTex.Get(), srcTexture);
    }

    // Hic frame gelmediyse cizecek bir sey yok (ilk karelerde olabilir).
    if (!rt.sourceTex || !rt.sourceSrv)
        return false;

    // ── srcRect'i UV'ye cevir ──
    // Piksel koordinati -> 0..1 arasi texture koordinati.
    // Python analojisi: box = (l, t, r, b) piksel; UV = box / (W, H).
    const long texW = static_cast<long>(rt.sourceWidth);
    const long texH = static_cast<long>(rt.sourceHeight);
    if (texW <= 0 || texH <= 0)
        return false;

    const long left   = std::clamp(srcRect.left,   0L, texW);
    const long top    = std::clamp(srcRect.top,    0L, texH);
    const long right  = std::clamp(srcRect.right,  left + 1, texW);
    const long bottom = std::clamp(srcRect.bottom, top  + 1, texH);

    const float fTexW = static_cast<float>(texW);
    const float fTexH = static_cast<float>(texH);

    UvParams params{};
    params.uvLeft   = static_cast<float>(left) / fTexW;
    params.uvTop    = static_cast<float>(top)  / fTexH;
    params.uvWidth  = static_cast<float>(right  - left) / fTexW;
    params.uvHeight = static_cast<float>(bottom - top)  / fTexH;

    m_context->UpdateSubresource(m_uvBuffer.Get(), 0, nullptr, &params, 0, 0);

    // ── Pipeline'i kur ──
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

    // Vertex buffer ve input layout YOK — konumlar SV_VertexID'den geliyor.
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_uvBuffer.GetAddressOf());

    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, rt.sourceSrv.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_samplerLinear.GetAddressOf());

    // Uc vertex = ekrani kaplayan tek ucgen.
    m_context->Draw(3, 0);

#ifdef _DEBUG
    // ── Tek kare dokumu (dogrulama araci) ──
    // BM_DUMP_FRAME ayarliysa, isler oturduktan sonra bir kare diske yazilir.
    // Present'ten ONCE, cizimden HEMEN SONRA — arada back buffer bozulmasin.
    {
        static const std::wstring dumpPath = []() -> std::wstring {
            wchar_t buf[MAX_PATH]{};
            const DWORD n = GetEnvironmentVariableW(L"BM_DUMP_FRAME", buf, MAX_PATH);
            return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring{};
        }();

        if (!dumpPath.empty())
        {
            // Kacinci karede dokulecek? BM_DUMP_AFTER ile ayarlanabilir
            // (varsayilan 60 ~= 1 saniye). Fare takibi gibi zamana bagli
            // davranislari dogrularken gec bir kare gerekiyor.
            static const int dumpAtFrame = []() {
                wchar_t buf[32]{};
                const DWORD n = GetEnvironmentVariableW(L"BM_DUMP_AFTER", buf, 32);
                const int parsed = (n > 0 && n < 32) ? _wtoi(buf) : 0;
                return (parsed > 0) ? parsed : 60;
            }();

            static int frameCounter = 0;
            if (++frameCounter == dumpAtFrame)
                DumpBackBuffer(targetIndex, dumpPath.c_str());
        }
    }
#endif

    return true;
}

// =============================================================================
// EnsureSourceTexture — shader'da orneklenebilir ara texture
// =============================================================================
// Desktop Duplication texture'i D3D11_BIND_SHADER_RESOURCE olmadan geldigi
// icin uzerinde SRV acilamiyor. Ayni boyut/formatta, SRV bind'li kendi
// texture'imizi tutup her frame'i oraya kopyaliyoruz.
//
// Boyut degismediyse dokunmuyoruz — texture olusturmak pahali, her frame
// yapilacak is degil.
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

    // Kaynagin tanimini temel al ama bayraklari kendimiz belirle:
    // paylasim/staging bayraklarini tasimak istemiyoruz.
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
        LOG_ERROR("Ara texture icin SRV olusturulamadi: 0x{:08X}",
            static_cast<unsigned long>(hr));
        rt.sourceTex.Reset();
        return false;
    }

    rt.sourceWidth  = srcDesc.Width;
    rt.sourceHeight = srcDesc.Height;
    rt.sourceFormat = srcDesc.Format;

    LOG_DEBUG("Ara kaynak texture'i olusturuldu: {}x{}", srcDesc.Width, srcDesc.Height);
    return true;
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
// DumpBackBuffer — render sonucunu diske yaz (sadece Debug)
// =============================================================================
// Overlay penceresi ekran goruntusu araclarina gorunmez oldugu icin render'i
// disaridan dogrulamanin tek yolu bu.
//
// Akis: back buffer (GPU) -> staging texture (CPU okunabilir) -> Map -> BMP.
// GPU belleği CPU'dan dogrudan okunamaz; STAGING usage tam olarak bunun icin var.
//
// Python analojisi: GPU tensor'unu .cpu().numpy() ile almak.
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
        LOG_ERROR("Dump: Map basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
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
        LOG_INFO("Dump: back buffer yazildi ({}x{}) -> {}",
            desc.Width, desc.Height, "BM_DUMP_FRAME");
    }
    else
    {
        LOG_ERROR("Dump: dosya acilamadi ({})", GetLastError());
    }

    m_context->Unmap(staging.Get(), 0);
    return ok;
}
#endif // _DEBUG

} // namespace BetterMagnifier
