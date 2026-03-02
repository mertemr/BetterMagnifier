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
    , m_frameAcquired(other.m_frameAcquired)
    , m_initialized(other.m_initialized)
    , m_needsReinit(other.m_needsReinit)
    , m_width(other.m_width)
    , m_height(other.m_height)
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
        m_duplication   = std::move(other.m_duplication);
        m_output1       = std::move(other.m_output1);
        m_device        = other.m_device;
        m_context       = other.m_context;
        m_frameAcquired = other.m_frameAcquired;
        m_initialized   = other.m_initialized;
        m_needsReinit   = other.m_needsReinit;
        m_width         = other.m_width;
        m_height        = other.m_height;
        m_frameCount    = other.m_frameCount;
        m_errorCount    = other.m_errorCount;

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

    m_initialized = true;
    m_needsReinit = false;
    m_frameCount  = 0;
    m_errorCount  = 0;

    LOG_INFO("DXGICapture baslatildi: {}x{}", m_width, m_height);
    return true;
}

// =============================================================================
// AcquireFrame — Desktop'un guncel frame'ini yakala
// =============================================================================
//
// Frame akisi:
//   1. AcquireNextFrame → bekleyen frame var mi?
//   2. Varsa → ID3D11Texture2D olarak al (GPU'da!)
//   3. Yoksa (DXGI_ERROR_WAIT_TIMEOUT) → normal, ekranda degisiklik yok
//   4. DXGI_ERROR_ACCESS_LOST → full-screen app acildi, reinit gerek
//
// ONEMLI: AcquireFrame'den sonra MUTLAKA ReleaseFrame cagirilmali!
// Aksi halde sonraki AcquireFrame "frame already acquired" hatasi verir.
// Python analojisi: context manager gibi dusun — __enter__ ve __exit__
//
// =============================================================================
CapturedFrame DXGICapture::AcquireFrame(UINT timeoutMs)
{
    CapturedFrame result{};

    if (!m_initialized || !m_duplication)
    {
        m_needsReinit = true;
        return result;
    }

    // Onceki frame release edilmemisse, once onu release et
    if (m_frameAcquired)
    {
        ReleaseFrame();
    }

    // ── AcquireNextFrame ──
    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};

    HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        // Normal durum — ekranda degisiklik yok.
        // Python'da: raise TimeoutError — ama burada hata degil, normal.
        // Onceki frame'i kullanmaya devam et.
        result.isNewFrame = false;
        return result;
    }

    if (hr == static_cast<HRESULT>(DXGI_ERROR_ACCESS_LOST))
    {
        // Full-screen exclusive app acildi veya kapandi.
        // Ornek: Oyun acildi → desktop composition devre disi kaldi.
        // Cozum: Desktop Duplication session'i yeniden olustur.
        LOG_WARN("DXGI_ERROR_ACCESS_LOST — reinit gerekiyor");
        m_needsReinit = true;
        Cleanup();
        return result;
    }

    if (FAILED(hr))
    {
        m_errorCount++;
        if (m_errorCount % 100 == 1)  // Her 100 hatada bir logla (spam onleme)
        {
            LOG_WARN("AcquireNextFrame basarisiz: 0x{:08X} (hata #{:d})",
                static_cast<unsigned long>(hr), m_errorCount);
        }
        return result;
    }

    m_frameAcquired = true;

    // ── ID3D11Texture2D'ye donustur ──
    // IDXGIResource genel bir COM interface'i.
    // Biz GPU texture'i olarak kullanacagimiz icin ID3D11Texture2D'ye cast ediyoruz.
    // Python analojisi: texture = cast(resource, ID3D11Texture2D)
    hr = desktopResource.As(&result.texture);
    if (FAILED(hr) || !result.texture)
    {
        LOG_ERROR("IDXGIResource -> ID3D11Texture2D donusumu basarisiz");
        ReleaseFrame();
        return result;
    }

    result.frameInfo  = frameInfo;
    result.isNewFrame = true;
    result.width      = m_width;
    result.height     = m_height;
    m_frameCount++;

    return result;
}

// =============================================================================
// ReleaseFrame — Yakalanan frame'i serbest birak
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
// Reinitialize — ACCESS_LOST sonrasi recovery
// =============================================================================
bool DXGICapture::Reinitialize()
{
    LOG_INFO("DXGICapture yeniden baslatiliyor...");

    if (!m_device || !m_output1)
    {
        LOG_ERROR("Reinitialize: device veya output kaybolmus!");
        return false;
    }

    // Mevcut duplication'i temizle
    if (m_frameAcquired && m_duplication)
    {
        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }
    m_duplication.Reset();

    // Yeniden olustur
    HRESULT hr = m_output1->DuplicateOutput(m_device, &m_duplication);
    if (FAILED(hr))
    {
        LOG_WARN("Reinitialize basarisiz: 0x{:08X} — sonra tekrar denenecek", static_cast<unsigned long>(hr));
        return false;
    }

    m_initialized = true;
    m_needsReinit = false;
    m_errorCount  = 0;

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
