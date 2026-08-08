// =============================================================================
// DXGICapture.cpp — DXGI Desktop Duplication Implementation
// =============================================================================
//
// COM Release Sirasi ACIKLAMASI (bu dosyada kritik!):
//
//   COM nesneleri referans sayimli (reference counted) calisir.
//   Her ComPtr<T> bir referans tutar. Scope bitince otomatik Release cagirilir.
//   
//   Python analojisi:
//     Python'da: del obj → garbage collector toplar (referans 0 olunca)
//     C++ COM'da: ComPtr scope disindan cikar → Release cagirilir → ref 0 olunca yikilir
//   
//   YANLIS Release sirasi = crash! Ornek:
//     Device'i Output'tan ONCE serbest birakirsan → Output dangen pointer'a isaret eder
//     OutputDuplication serbest birakilmadan yeni bir tane olusturursan → hata
//   
//   DOGRU sira (Cleanup'ta yapilan):
//     1. m_duplication serbest birak (Desktop Duplication session)
//     2. m_output1 serbest birak (DXGI Output referansi)
//     3. m_device ve m_context'e DOKUNMA (onlar D3DRenderer'a ait!)
//
// =============================================================================

#include "pch.h"
#include "DXGICapture.h"
#include "Logger.h"

namespace BetterMagnifier {

// =============================================================================
// Destructor
// =============================================================================
DXGICapture::~DXGICapture()
{
    Cleanup();
}

// =============================================================================
// Move Constructor & Assignment
// =============================================================================
DXGICapture::DXGICapture(DXGICapture&& other) noexcept
    : m_duplication(std::move(other.m_duplication))
    , m_output1(std::move(other.m_output1))
    , m_device(other.m_device)
    , m_context(other.m_context)
    , m_cachedFrame(std::move(other.m_cachedFrame))
    , m_frameAcquired(other.m_frameAcquired)
    , m_initialized(other.m_initialized)
    , m_needsReinit(other.m_needsReinit)
    , m_width(other.m_width)
    , m_height(other.m_height)
    , m_nextReinitAttempt(other.m_nextReinitAttempt)
    , m_frameCount(other.m_frameCount)
    , m_errorCount(other.m_errorCount)
{
    other.m_device = nullptr;
    other.m_context = nullptr;
    other.m_frameAcquired = false;
    other.m_initialized = false;
}

DXGICapture& DXGICapture::operator=(DXGICapture&& other) noexcept
{
    if (this != &other)
    {
        Cleanup();
        m_duplication       = std::move(other.m_duplication);
        m_output1           = std::move(other.m_output1);
        m_device            = other.m_device;
        m_context           = other.m_context;
        m_cachedFrame       = std::move(other.m_cachedFrame);
        m_frameAcquired     = other.m_frameAcquired;
        m_initialized       = other.m_initialized;
        m_needsReinit       = other.m_needsReinit;
        m_width             = other.m_width;
        m_height            = other.m_height;
        m_nextReinitAttempt = other.m_nextReinitAttempt;
        m_frameCount        = other.m_frameCount;
        m_errorCount        = other.m_errorCount;

        other.m_device = nullptr;
        other.m_context = nullptr;
        other.m_frameAcquired = false;
        other.m_initialized = false;
    }
    return *this;
}

// =============================================================================
// Initialize — Desktop Duplication session olustur
// =============================================================================
bool DXGICapture::Initialize(ID3D11Device* device, IDXGIOutput* output)
{
    if (!device || !output)
    {
        LOG_ERROR("DXGICapture::Initialize - null device veya output!");
        return false;
    }

    Cleanup();

    m_device = device;
    device->GetImmediateContext(&m_context);

    // ── IDXGIOutput1'e QueryInterface ──
    // Desktop Duplication API, IDXGIOutput1 veya ustunden calisiyor.
    // Python analojisi: output_v1 = output.as_interface(IDXGIOutput1)
    HRESULT hr = output->QueryInterface(IID_PPV_ARGS(&m_output1));
    if (FAILED(hr))
    {
        LOG_ERROR("IDXGIOutput1 QueryInterface basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    // ── Output boyutlarini al ──
    DXGI_OUTPUT_DESC outputDesc{};
    m_output1->GetDesc(&outputDesc);
    m_width  = static_cast<UINT>(outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left);
    m_height = static_cast<UINT>(outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);

    // ── Desktop Duplication olustur ──
    // Bu cagri monitor'un desktop composition'ina "abone" oluyor.
    // Her frame'de Windows bize GPU'daki texture'i verecek.
    //
    // OLASI HATALAR:
    //   E_ACCESSDENIED → UAC/Secure desktop (kullanici giris ekrani)
    //   DXGI_ERROR_NOT_CURRENTLY_AVAILABLE → Baska bir uygulama zaten duplication yapiyor
    //   DXGI_ERROR_UNSUPPORTED → GPU veya driver desteklemiyor
    hr = m_output1->DuplicateOutput(m_device, &m_duplication);
    if (FAILED(hr))
    {
        if (hr == E_ACCESSDENIED)
            LOG_ERROR("DuplicateOutput: E_ACCESSDENIED (UAC/secure desktop?)");
        else if (hr == static_cast<HRESULT>(DXGI_ERROR_NOT_CURRENTLY_AVAILABLE))
            LOG_ERROR("DuplicateOutput: DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (baska uygulama kullanıyor?)");
        else if (hr == static_cast<HRESULT>(DXGI_ERROR_UNSUPPORTED))
            LOG_ERROR("DuplicateOutput: DXGI_ERROR_UNSUPPORTED (GPU/driver desteği yok)");
        else
            LOG_ERROR("DuplicateOutput basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));

        Cleanup();
        return false;
    }

    m_initialized       = true;
    m_needsReinit       = false;
    m_frameCount        = 0;
    m_errorCount        = 0;
    m_nextReinitAttempt = {};

    LOG_INFO("DXGICapture baslatildi: {}x{}", m_width, m_height);
    return true;
}

// =============================================================================
// EnsureCacheTexture — onbellek texture'ini kaynak tanimina gore hazirla
// =============================================================================
//
// Kaynak (duplication) texture'in Width/Height/Format'ini aynen kopyaliyoruz;
// CopyResource iki texture'in TAM olarak ayni tanima sahip olmasini istiyor
// (boyut, format, mip, sample). Bind flag'i farkli olabilir — orasi serbest.
//
// SRV bind flag'i ekliyoruz: duplication texture'lari onsuz geliyor ve bu
// yuzden D3DRenderer su an shader kullanamiyor. Onbellek uzerinden shader
// tabanli olcekleme mumkun hale geliyor.
// =============================================================================
bool DXGICapture::EnsureCacheTexture(ID3D11Texture2D* src)
{
    if (!src || !m_device)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);

    if (m_cachedFrame)
    {
        D3D11_TEXTURE2D_DESC current{};
        m_cachedFrame->GetDesc(&current);

        // Ayni tanimdaysa yeniden olusturmuyoruz — her frame texture
        // ayirmak GPU bellegini bosuna dolduruyor.
        if (current.Width  == desc.Width &&
            current.Height == desc.Height &&
            current.Format == desc.Format)
        {
            return true;
        }

        LOG_INFO("Capture onbellegi yeniden olusturuluyor: {}x{} -> {}x{}",
            current.Width, current.Height, desc.Width, desc.Height);
        m_cachedFrame.Reset();
    }

    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags      = 0;

    const HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_cachedFrame);
    if (FAILED(hr))
    {
        LOG_ERROR("Capture onbellek texture'i olusturulamadi: 0x{:08X}",
            static_cast<unsigned long>(hr));
        m_cachedFrame.Reset();
        return false;
    }

    m_width  = desc.Width;
    m_height = desc.Height;
    return true;
}

// =============================================================================
// AcquireFrame — Desktop'un guncel frame'ini yakala ve onbellege al
// =============================================================================
//
// Frame akisi:
//   1. AcquireNextFrame → bekleyen frame var mi?
//   2. Varsa → onbellek texture'ina KOPYALA, frame'i HEMEN birak
//   3. Yoksa (DXGI_ERROR_WAIT_TIMEOUT) → normal, onbellekteki goruntu gecerli
//   4. DXGI_ERROR_ACCESS_LOST → full-screen app acildi, reinit gerek
//
// NEDEN KOPYALIYORUZ (onceki hali dogrudan duplication texture'ini donduruyordu):
//   Duplication texture'i sadece ReleaseFrame'e kadar gecerli. Onu dogrudan
//   kullanmak, "yeni frame yoksa hic render etme" davranisini zorunlu kiliyordu:
//   masaustu sabitken fareyi hareket ettirince zoom bolgesi kayiyor ama ekran
//   guncellenmiyordu — buyutulmus goruntu donuyordu. Onbellek bunu cozuyor.
//
//   Ikinci fayda: frame'i milisaniyeler yerine mikrosaniyeler boyunca tutuyoruz.
//   Duplication frame'ini elde tutmak masaustu composition'ini geciktirir.
//
// LastPresentTime == 0 → sadece imlec hareket etti, masaustu icerigi ayni.
// O durumda kopyayi atliyoruz (onbellek zaten guncel) — tek istisna, henuz
// hic goruntu almadigimiz ilk cagri.
// =============================================================================
CapturedFrame DXGICapture::AcquireFrame(UINT timeoutMs)
{
    CapturedFrame result{};
    result.width   = m_width;
    result.height  = m_height;
    result.texture = m_cachedFrame;   // Ilk basarili yakalamaya kadar null

    if (!m_initialized || !m_duplication)
    {
        m_needsReinit = true;
        return result;
    }

    // Onceki frame release edilmemisse (beklenmiyor ama ucuz sigorta)
    if (m_frameAcquired)
        ReleaseFrame();

    // ── AcquireNextFrame ──
    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};

    HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        // Normal durum — ekranda degisiklik yok.
        // Onbellekteki goruntu hala gecerli, isNewFrame false kaliyor.
        return result;
    }

    if (hr == static_cast<HRESULT>(DXGI_ERROR_ACCESS_LOST))
    {
        // Full-screen exclusive app acildi veya kapandi.
        // Ornek: Oyun acildi → desktop composition devre disi kaldi.
        //
        // SADECE duplication oturumunu birakiyoruz. Eskiden burada Cleanup()
        // cagriliyordu — o m_output1 ve m_device'i de sifirliyordu, boylece
        // Reinitialize() "device veya output kaybolmus" deyip her seferinde
        // basarisiz oluyordu: ACCESS_LOST'tan sonra capture BIR DAHA acilmiyordu.
        LOG_WARN("DXGI_ERROR_ACCESS_LOST — duplication yeniden kurulacak");
        ReleaseDuplication();
        m_needsReinit       = true;
        m_nextReinitAttempt = {};   // Ilk deneme beklemesin
        return result;
    }

    if (FAILED(hr))
    {
        m_errorCount++;
        if (m_errorCount % 100 == 1)  // Her 100 hatada bir logla (spam onleme)
        {
            LOG_WARN("AcquireNextFrame basarisiz: 0x{:08X} (hata #{})",
                static_cast<unsigned long>(hr), m_errorCount);
        }
        return result;
    }

    m_frameAcquired  = true;
    result.frameInfo = frameInfo;

    // ── Masaustu icerigi gercekten degistiyse onbellege kopyala ──
    // Onbellek henuz bossa kosulsuz kopyaliyoruz: LastPresentTime == 0 sadece
    // "son yakalamadan beri yeni present olmadi" demek, texture yine de guncel
    // masaustunu tasiyor. Bu olmadan hareketsiz bir ekranda zoom acildiginda
    // ilk goruntu ancak masaustu degisince gelirdi.
    const bool needFirstFrame = (m_cachedFrame == nullptr);

    if (frameInfo.LastPresentTime.QuadPart != 0 || needFirstFrame)
    {
        ComPtr<ID3D11Texture2D> acquired;
        hr = desktopResource.As(&acquired);

        if (FAILED(hr) || !acquired)
        {
            LOG_ERROR("IDXGIResource -> ID3D11Texture2D donusumu basarisiz");
        }
        else if (EnsureCacheTexture(acquired.Get()) && m_context)
        {
            // GPU icinde texture->texture kopya. CPU'ya inmiyor.
            m_context->CopyResource(m_cachedFrame.Get(), acquired.Get());

            result.texture    = m_cachedFrame;
            result.width      = m_width;
            result.height     = m_height;
            result.isNewFrame = true;
            m_frameCount++;
        }
    }

    // Kaynak referansini frame'i birakmadan ONCE dusur — DXGI bunu bekliyor.
    desktopResource.Reset();
    ReleaseFrame();

    return result;
}

// =============================================================================
// ReleaseFrame — Elde tutulan duplication frame'ini serbest birak
// =============================================================================
void DXGICapture::ReleaseFrame()
{
    if (m_frameAcquired && m_duplication)
    {
        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }
}

// =============================================================================
// ReleaseDuplication — sadece oturumu birak, device/output referanslarini tut
// =============================================================================
void DXGICapture::ReleaseDuplication()
{
    if (m_frameAcquired && m_duplication)
    {
        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }

    m_duplication.Reset();
    m_initialized = false;
}

// =============================================================================
// Reinitialize — ACCESS_LOST sonrasi recovery
// =============================================================================
bool DXGICapture::Reinitialize()
{
    if (!m_device || !m_output1)
    {
        // Buraya sadece hic Initialize edilmemis bir nesnede dusulur.
        // needsReinit'i kapatiyoruz ki cagiran her frame tekrar denemesin.
        m_needsReinit = false;
        return false;
    }

    // ── Yeniden deneme araligi ──
    // Fullscreen bir oyun acikken DuplicateOutput surekli basarisiz olur.
    // Her frame denemek hem log'u doldurur hem bosa is. 500 ms'de bir yeter —
    // insan gozu icin aninda geri gelmis gibi gorunur.
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextReinitAttempt)
        return false;

    ReleaseDuplication();

    HRESULT hr = m_output1->DuplicateOutput(m_device, &m_duplication);
    if (FAILED(hr))
    {
        m_nextReinitAttempt = now + kReinitRetryDelay;
        LOG_DEBUG("Reinitialize basarisiz: 0x{:08X} — {} ms sonra tekrar denenecek",
            static_cast<unsigned long>(hr), kReinitRetryDelay.count());
        return false;
    }

    m_initialized       = true;
    m_needsReinit       = false;
    m_errorCount        = 0;
    m_nextReinitAttempt = {};

    LOG_INFO("DXGICapture yeniden baslatildi ({}x{})", m_width, m_height);
    return true;
}

// =============================================================================
// Cleanup — Tum COM kaynaklarini serbest birak (DOGRU SIRADA!)
// =============================================================================
void DXGICapture::Cleanup()
{
    // SIRA ONEMLI! En son olusturulan en once serbest birakilir.
    // Python'da: finally blogu, ama siralama onemli degil (GC halleder).
    // C++ COM'da: LIFO sirasi takip edilmeli.

    // 1. Yakalanan frame varsa release et
    if (m_frameAcquired && m_duplication)
    {
        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }

    // 2. Desktop Duplication session'i serbest birak
    m_duplication.Reset();

    // 2b. Onbellek texture'i (bizim olusturdugumuz, bizim birakmamiz gerek)
    m_cachedFrame.Reset();

    // 3. Output referansini serbest birak
    m_output1.Reset();

    // 4. Context'i release et (biz aldık GetImmediateContext ile, ref count artti)
    if (m_context)
    {
        m_context->Release();
        m_context = nullptr;
    }

    // NOT: m_device'i release ETMIYORUZ — o D3DRenderer'a ait!
    // Biz sadece raw pointer tutuyoruz, sahiplik bizde degil.
    m_device = nullptr;

    m_initialized = false;

    LOG_DEBUG("DXGICapture cleanup tamamlandi (frames: {}, errors: {})",
        m_frameCount, m_errorCount);
}

} // namespace BetterMagnifier
