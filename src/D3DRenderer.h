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

    // ── Ara kaynak texture'i ──
    // Desktop Duplication texture'lari D3D11_BIND_SHADER_RESOURCE OLMADAN
    // gelir, yani shader'da dogrudan ornekleneMEZ. Her frame'i once bu
    // texture'a kopyalayip SRV'yi bunun uzerinde aciyoruz.
    //
    // Neden monitor BASINA? Monitorlerin cozunurlugu farkli (2560x1440 ve
    // 1920x1080). Tek ortak texture olsaydi iki monitorde de zoom acikken
    // her frame yeniden olusturmak gerekirdi.
    //
    // Ikinci gorevi: son frame'in kopyasini SAKLAMAK. Ekran degismediginde
    // Desktop Duplication yeni frame vermez; fare hareket ettiginde zoom
    // bolgesinin yine de kaymasi icin elimizde onceki goruntu kalmali.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          sourceTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceSrv;
    UINT                                             sourceWidth  = 0;
    UINT                                             sourceHeight = 0;
    DXGI_FORMAT                                      sourceFormat = DXGI_FORMAT_UNKNOWN;
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
    // srcRect bolgesini TUM ekrani kaplayacak sekilde olceklendirip cizer.
    //
    // srcTexture: DXGICapture'dan gelen yeni frame.
    //             nullptr verilirse son frame tekrar kullanilir — ekran
    //             degismedigi halde fare hareket ettiginde gerekiyor.
    // targetIndex: Hangi monitore (swap chain'e) render edilecek
    // srcRect: Kaynak texture'dan buyutulecek bolge (zoom bolgesi)
    //
    // Donus: cizim yapildiysa true. false ise Present cagrilmamali.
    bool RenderFrame(
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

#ifdef _DEBUG
    // Back buffer'i BMP olarak diske yazar (Present'ten ONCE cagrilmali —
    // FLIP_DISCARD'da Present sonrasi icerik tanimsiz).
    //
    // Neden var? Overlay penceresi WDA_EXCLUDEFROMCAPTURE ile korunuyor, yani
    // ekran goruntusu araclarinda GORUNMUYOR. Render'in dogru olup olmadigini
    // disaridan dogrulamanin baska yolu yok.
    //
    // BM_DUMP_FRAME ortam degiskeni bir dosya yoluna ayarlanirsa otomatik
    // olarak tek kare dokulur.
    bool DumpBackBuffer(size_t targetIndex, const wchar_t* path);
#endif

private:
    // ── Device Creation ──
    bool CreateDevice();
    bool CreateSamplerStates();
    bool CreateShaders();

    // ── Internal Render Helpers ──
    // Hedefin ara texture'ini kaynak boyut/formatina gore hazirlar.
    // Boyut degismediyse hicbir sey yapmaz (her frame yeniden olusturmayalim).
    bool EnsureSourceTexture(RenderTarget& rt, const D3D11_TEXTURE2D_DESC& srcDesc);

    // ── D3D11 Core ──
    Microsoft::WRL::ComPtr<ID3D11Device1>        m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1>  m_context;
    Microsoft::WRL::ComPtr<IDXGIFactory2>         m_dxgiFactory;
    D3D_FEATURE_LEVEL                             m_featureLevel = D3D_FEATURE_LEVEL_11_0;

    // ── Sampler States ──
    Microsoft::WRL::ComPtr<ID3D11SamplerState>    m_samplerLinear;   // Bilinear
    Microsoft::WRL::ComPtr<ID3D11SamplerState>    m_samplerPoint;    // Nearest-neighbor

    // ── Shader Pipeline ──
    // Fullscreen ucgen + bilinear ornekleme = gercek buyutme.
    // Vertex buffer YOK: vertex shader konumlari SV_VertexID'den uretiyor,
    // bu yuzden input layout da gerekmiyor.
    Microsoft::WRL::ComPtr<ID3D11VertexShader>    m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>     m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer>          m_uvBuffer;    // float4: UV bolgesi
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterNoCull;

    // ── Per-Monitor Render Targets ──
    std::vector<RenderTarget> m_renderTargets;

    // ── Debug ──
#ifdef _DEBUG
    Microsoft::WRL::ComPtr<ID3D11Debug> m_debug;
#endif
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_D3D_RENDERER_H
