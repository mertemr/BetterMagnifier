#pragma once

// =============================================================================
// DXGICapture.h — DXGI Desktop Duplication Capture
// =============================================================================
//
// Python analojisi:
//   Python'da: mss.grab(monitor) veya PIL.ImageGrab.grab()
//   Burada:    DXGICapture::AcquireFrame() → GPU texture (CPU'ya inmez!)
//
// DXGI Desktop Duplication API nedir?
//   Windows 8+'da eklenen, masaustunu GPU seviyesinde yakalayan API.
//   Avantajlari:
//     - GPU->GPU transfer (CPU'ya inmez, cok hizli)
//     - Full desktop composition capture (tum pencereler dahil)
//     - Dirty region tracking (sadece degisen pikseller)
//     - Mouse cursor bilgisi ayrica alinabilir
//
//   Dezavantajlari:
//     - Full-screen exclusive app'larda DXGI_ERROR_ACCESS_LOST
//     - UAC/secure desktop'ta calismaz
//     - Per-output (monitor) bazinda calisir
//
// Her monitor icin ayri bir DXGICapture nesnesi olusturulur.
//
// =============================================================================

#ifndef BETTER_MAGNIFIER_DXGI_CAPTURE_H
#define BETTER_MAGNIFIER_DXGI_CAPTURE_H

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <chrono>
#include <atomic>

namespace BetterMagnifier {

// ─────────────────────────────────────────────────────────────────────────────
// Captured Frame — Yakalanan tek bir frame'in bilgileri
// ─────────────────────────────────────────────────────────────────────────────
struct CapturedFrame
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;     // GPU'daki yakalanan frame
    DXGI_OUTDUPL_FRAME_INFO                 frameInfo;   // Timestamp, mouse info, vb.
    bool                                    isNewFrame = false;  // Yeni frame mi, eski mi?
    UINT                                    width  = 0;
    UINT                                    height = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// DXGICapture — Tek bir monitor icin Desktop Duplication
// ─────────────────────────────────────────────────────────────────────────────
class DXGICapture
{
public:
    DXGICapture() = default;
    ~DXGICapture();

    // Kopyalama yasak (COM pointer'lar)
    DXGICapture(const DXGICapture&) = delete;
    DXGICapture& operator=(const DXGICapture&) = delete;

    // Move izin ver (vector'de tutmak icin)
    DXGICapture(DXGICapture&& other) noexcept;
    DXGICapture& operator=(DXGICapture&& other) noexcept;

    // ── Initialization ──
    // D3D11 device ve DXGI output ile ilklendirme.
    // device: D3DRenderer'dan alinan paylasimli device
    // output: MonitorManager'dan alinan IDXGIOutput
    bool Initialize(ID3D11Device* device, IDXGIOutput* output);

    // ── Frame Capture ──
    // Desktop'un guncel frame'ini yakala.
    // timeoutMs: Bekleme suresi (0 = anlik, 16 = ~60fps)
    // Dondurulen CapturedFrame.isNewFrame true ise yeni frame var.
    // False ise ekranda degisiklik yok (eski frame'i kullanmaya devam et).
    CapturedFrame AcquireFrame(UINT timeoutMs = 16);

    // ── Release ──
    // Yakalanan frame'i serbest birak. AcquireFrame'den sonra MUTLAKA cagirilmali!
    // Aksi halde sonraki AcquireFrame basarisiz olur.
    void ReleaseFrame();

    // ── Recovery ──
    // DXGI_ERROR_ACCESS_LOST sonrasi yeniden olustur.
    // Full-screen exclusive app acilip kapaninca gerekir.
    bool Reinitialize();

    // ── State ──
    bool IsInitialized() const { return m_initialized; }
    bool NeedsReinit()   const { return m_needsReinit; }
    UINT GetWidth()      const { return m_width; }
    UINT GetHeight()     const { return m_height; }

private:
    // ── Internal ──
    void Cleanup();

    // ── COM Resources ──
    // Release sirasi KRITIK:
    //   1. OutputDuplication (en son olusturulan, en once serbest birakilir)
    //   2. StagingTexture (varsa)
    //   3. Output referansi
    //   4. Device/Context referansi (BUNLARI BIZ RELEASE ETMEYIZ — D3DRenderer sahip)
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<IDXGIOutput1>           m_output1;
    ID3D11Device*                                  m_device  = nullptr;  // Owned by D3DRenderer
    ID3D11DeviceContext*                            m_context = nullptr;  // Owned by D3DRenderer

    // ── Frame State ──
    bool  m_frameAcquired = false;   // AcquireFrame yapildi, ReleaseFrame henuz yapilmadi
    bool  m_initialized   = false;
    bool  m_needsReinit   = false;   // DXGI_ERROR_ACCESS_LOST durumu
    UINT  m_width  = 0;
    UINT  m_height = 0;

    // ── Stats ──
    uint64_t m_frameCount = 0;
    uint64_t m_errorCount = 0;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_DXGI_CAPTURE_H
