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
