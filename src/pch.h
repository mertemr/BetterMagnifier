// =============================================================================
// pch.h — Precompiled Header
// =============================================================================
// Bu dosya projedeki TÜM .cpp dosyaları tarafından otomatik include edilir.
// Seyrek değişen, ağır sistem header'larını buraya koyarak compile süresini
// dramatik şekilde azaltırız.
//
// Python analojisi: Bu dosya, her modülün başında yazdığın
//   "from typing import *; import os, sys, logging" gibi ortak import'ların
//   tek bir yerde toplanmış hali. Fark: C++'ta bu header'lar precompile edilip
//   binary cache'lenir, böylece her .cpp için tekrar parse edilmez.
// =============================================================================

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// HEDEF WINDOWS VERSIYONU
// ─────────────────────────────────────────────────────────────────────────────
// Windows 10 1903 (build 18362) hedefliyoruz.
// Bu, DXGI 1.5 ve Desktop Duplication API v2'nin mevcut olduğu minimum versiyon.
// Bu tanımları windows.h'den ÖNCE yapmak gerekir, yoksa eski API setini alırız.
#include <sdkddkver.h>
#ifndef WINVER
#define WINVER 0x0A00           // Windows 10
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00     // Windows 10
#endif

// ─────────────────────────────────────────────────────────────────────────────
// WIN32 LEAN & MEAN — gereksiz Windows header'larını dışla
// ─────────────────────────────────────────────────────────────────────────────
// Bunlar .vcxproj'da preprocessor olarak da tanımlı, ama pch.h'de de
// güvence olarak tanımlıyoruz (başka bir dosya include ederse diye).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX    // Windows.h'nin min/max makrolarını engelle — std::min/max ile çakışır!
#endif

// ─────────────────────────────────────────────────────────────────────────────
// WINDOWS CORE
// ─────────────────────────────────────────────────────────────────────────────
#include <windows.h>
#include <windowsx.h>       // GET_X_LPARAM, GET_Y_LPARAM gibi mesaj makroları
#include <shellapi.h>       // Shell_NotifyIcon (system tray) için
#include <commctrl.h>       // Common controls
#include <hidusage.h>       // Raw input HID usage pages
#include <shellscalingapi.h>// GetDpiForMonitor, MDT_EFFECTIVE_DPI (Shcore.lib gerekir)

// ─────────────────────────────────────────────────────────────────────────────
// COM & WRL (Windows Runtime Library)
// ─────────────────────────────────────────────────────────────────────────────
// Python analojisi: ComPtr<T> = Python'daki referans sayımlı nesne yönetimi.
// Python'da garbage collector her şeyi otomatik toplar. C++'ta COM objeleri
// manuel AddRef/Release gerektirir — ComPtr bunu RAII ile otomatikleştirir.
// Yani ComPtr = "COM nesneleri için smart pointer".
#include <wrl/client.h>     // Microsoft::WRL::ComPtr<T>

// ─────────────────────────────────────────────────────────────────────────────
// DIRECTX 11 & DXGI
// ─────────────────────────────────────────────────────────────────────────────
#include <d3d11_1.h>        // ID3D11Device1, ID3D11DeviceContext1
#include <dxgi1_5.h>        // IDXGIFactory5, IDXGIOutput5 (HDR + Desktop Duplication v2)
#include <d2d1_1.h>         // ID2D1Factory1 (smooth scaling, anti-aliased render)
#include <dwrite_1.h>       // IDWriteFactory1 (text rendering — tray tooltip, debug overlay)
#include <d3dcompiler.h>    // D3DCompileFromFile (shader compilation)

// ─────────────────────────────────────────────────────────────────────────────
// DIRECT2D HELPERS
// ─────────────────────────────────────────────────────────────────────────────
#include <d2d1helper.h>     // D2D1::RectF, D2D1::ColorF gibi helper'lar

// ─────────────────────────────────────────────────────────────────────────────
// C++ STANDARD LIBRARY
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <unordered_map>
#include <memory>           // std::unique_ptr, std::shared_ptr
#include <optional>         // std::optional — C++17 "None or value" (Python's Optional)
#include <functional>       // std::function — Python'daki callable/lambda
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cassert>
#include <stdexcept>
#include <chrono>           // High-resolution timing
#include <thread>
#include <mutex>
#include <atomic>
#include <format>           // C++20 std::format — Python's f-string karşılığı!
#include <source_location>  // C++20 — __FILE__, __LINE__ yerine type-safe alternatif
#include <filesystem>       // std::filesystem — Python's pathlib karşılığı

// ─────────────────────────────────────────────────────────────────────────────
// LOGGING SUPPORT
// ─────────────────────────────────────────────────────────────────────────────
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>

// ─────────────────────────────────────────────────────────────────────────────
// DEBUG ASSERT YÖNLENDİRMESİ
// ─────────────────────────────────────────────────────────────────────────────
// _CrtSetReportMode / _set_error_mode için. Debug build'de assert çıktısını
// MessageBox yerine stderr'e yöneltiyoruz (bkz. main.cpp) — böylece
// self-check betikten koşulabilir hale geliyor.
#include <crtdbg.h>
#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
// HRESULT CHECK MAKROSU
// ─────────────────────────────────────────────────────────────────────────────
// Python'da:   response.raise_for_status()
// C++ COM'da:  HR_CHECK(hr, "İşlem başarısız")
//
// HRESULT, Windows COM dünyasının "error code" sistemidir.
// 0 veya pozitif = başarı (S_OK, S_FALSE)
// Negatif = hata (E_FAIL, E_INVALIDARG, DXGI_ERROR_xxx)
//
// FAILED(hr) makrosu: hr < 0 ise true döner.
//
// Bu makroyu her COM çağrısından sonra kullanacağız. Hata olursa:
// 1. Log'a yaz (dosya, satır, thread bilgisiyle)
// 2. Exception fırlat (RAII stack unwinding ile cleanup garantili)
//
// Neden makro ve fonksiyon değil?
// → __FILE__ ve __LINE__ çağrıldığı yeri göstermeli, fonksiyon içinden değil.
//   C++20'de source_location ile fonksiyon da yazılabilir ama makro daha basit.
#define HR_CHECK(hr, msg)                                                       \
    do {                                                                        \
        HRESULT _hr = (hr);                                                     \
        if (FAILED(_hr)) {                                                      \
            auto _errMsg = std::format(L"{} (HRESULT: 0x{:08X}, File: {}, Line: {})",  \
                L##msg, static_cast<unsigned long>(_hr),                        \
                __FILEW__, __LINE__);                                            \
            OutputDebugStringW(_errMsg.c_str());                                \
            OutputDebugStringW(L"\n");                                           \
            throw std::runtime_error(std::format("{} (HRESULT: 0x{:08X})",      \
                msg, static_cast<unsigned long>(_hr)));                         \
        }                                                                       \
    } while(false)

// ─────────────────────────────────────────────────────────────────────────────
// WIDE → UTF-8 DÖNÜŞÜMÜ
// ─────────────────────────────────────────────────────────────────────────────
// Win32 API'leri wchar_t (UTF-16) döner, Logger ise char (UTF-8) yazıyor.
// Bu köprü fonksiyonu her iki dünyayı birleştirir.
//
// NEDEN std::string(ws.begin(), ws.end()) KULLANMIYORUZ?
//   O yaygın idiom her wchar_t'yi tek tek char'a daraltır (narrowing).
//   İki sorun: (1) derleyici C4244 uyarısı verir — bu projede warning-as-error,
//   (2) ASCII dışı her karakter BOZULUR. "DELL Ü2723" → "DELL ?2723".
//   Monitör isimlerinde Türkçe karakter olabilir, kayıpsız dönüşüm gerekli.
//
// Python analojisi: ws.encode("utf-8") — Python'da bir satır, C++'ta bu.
inline std::string ToUtf8(std::wstring_view ws)
{
    if (ws.empty())
        return {};

    // İki aşamalı çağrı: önce gereken byte sayısını sor, sonra doldur.
    // Win32'de yaygın kalıp — buffer boyutunu tahmin etmek yerine API'ye sor.
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
        nullptr, 0, nullptr, nullptr);

    if (needed <= 0)
        return {};

    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
        out.data(), needed, nullptr, nullptr);

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// OVERLAY MODU — girdi şeffaflığı mı, flip-model mi?
// ─────────────────────────────────────────────────────────────────────────────
// Bu iki şey Win32'de birlikte olamıyor ve seçim iki dosyayı birden etkiliyor
// (OverlayWindow pencere stilini, D3DRenderer swap effect'i kurar). Tek karar
// noktası olsun diye buraya koydu.
//
// LAYERED (varsayılan):
//   Pencere WS_EX_LAYERED | WS_EX_TRANSPARENT  -> tıklamalar ALTA GEÇER
//   Swap chain DXGI_SWAP_EFFECT_DISCARD (blt)  -> layered pencerede çalışır
//
//   Neden layered şart: WS_EX_TRANSPARENT tek başına ve WM_NCHITTEST'ten
//   HTTRANSPARENT döndürmek click-through için YETMİYOR. Güvenilir reçete
//   LAYERED | TRANSPARENT ikilisi. LAYERED'ı kaldırdığımızda overlay bütün
//   girdiyi yutmaya başladı — uygulamanın tüm amacı çöktü.
//
//   Neden blt: flip-model (FLIP_DISCARD) layered pencereyi reddeder,
//   CreateSwapChainForHwnd DXGI_ERROR_INVALID_CALL döner.
//
//   SetLayeredWindowAttributes ile layered yapılan pencere normal redirection
//   surface'ini KORUR, yani D3D çizimi çalışmaya devam eder. UpdateLayeredWindow
//   yolu olsaydı çalışmazdı — aradaki fark kritik.
//
// FLIP (BM_OVERLAY_FLIP=1 ile):
//   Eski davranış. Daha düşük latency ama girdi geçmiyor. Karşılaştırma için.
inline bool UseFlipOverlay()
{
    static const bool flip = []() {
        wchar_t buf[8]{};
        const DWORD n = GetEnvironmentVariableW(L"BM_OVERLAY_FLIP", buf, 8);
        return (n > 0 && n < 8 && buf[0] == L'1');
    }();
    return flip;
}

// ─────────────────────────────────────────────────────────────────────────────
// POPUP'LARIN ÜSTÜNDE KALMAK — iki kötü seçenek arasında tercih
// ─────────────────────────────────────────────────────────────────────────────
// Menüler ve dropdown'lar HWND_TOPMOST ile ve bizden SONRA doğuyor, yani
// z-order'da üstümüze çıkıyorlar. İki seçenek var ve ikisi de kusurlu:
//
// AÇIK (varsayılan) — topmost'u yeniden iddia edip üste çıkıyoruz:
//   Popup'ı sadece büyütülmüş halde görürsün. AMA tamamen kapatılan pencere
//   yeniden çizim yapmayı bırakıyor; masaüstü kompozisyonunda son boyanmış
//   hali kalıyor ve bizim capture'ımız o DONMUŞ hali büyütüyor. Menüde
//   gezinirken vurgu (highlight) güncellenmiyor.
//
// KAPALI (BM_NO_TOPMOST_FIGHT=1) — popup üstümüzde kalıyor:
//   Popup CANLI ve doğru çalışıyor ama büyütülmüyor, ve arkada bizim
//   büyütülmüş kopyamız da göründüğü için ekranda iki kez var.
//
// Doğru çözüm ikisi de değil: kompozisyon seviyesinde büyütme (Magnification
// API) gerekiyor, o da per-monitor bağımsız zoom'u kaybettiriyor.
inline bool FightPopupZOrder()
{
    static const bool off = []() {
        wchar_t buf[8]{};
        const DWORD n = GetEnvironmentVariableW(L"BM_NO_TOPMOST_FIGHT", buf, 8);
        return (n > 0 && n < 8 && buf[0] == L'1');
    }();
    return !off;
}

// ─────────────────────────────────────────────────────────────────────────────
// SAFE RELEASE HELPER (ComPtr kullanmadığımız nadir durumlar için)
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
inline void SafeRelease(T*& ptr)
{
    if (ptr)
    {
        ptr->Release();
        ptr = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// COMMON ALIASES
// ─────────────────────────────────────────────────────────────────────────────
namespace wrl = Microsoft::WRL;

template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;
