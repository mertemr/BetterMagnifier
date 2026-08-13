// Precompiled header: included automatically by every .cpp in the project.
// Heavy, rarely changing system headers go here so they are parsed once.

#pragma once

// Windows 10 1903 is the floor: the first version with DXGI 1.5 and Desktop
// Duplication v2. These must be defined BEFORE windows.h or we get the old
// API set.
#include <sdkddkver.h>
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

// Also set in the vcxproj; repeated here in case another file includes pch.h
// through a path that misses the project defines.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX    // windows.h min/max macros collide with std::min/max
#endif

#include <windows.h>
#include <windowsx.h>        // GET_X_LPARAM and friends
#include <shellapi.h>        // Shell_NotifyIcon
#include <shellscalingapi.h> // GetDpiForMonitor (needs Shcore.lib)

// ComPtr: RAII around COM reference counting.
#include <wrl/client.h>

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <d2d1_1.h>
#include <d3dcompiler.h>     // D3DCompile for the magnification shaders

#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <unordered_map>
#include <memory>
#include <optional>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cassert>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <format>
#include <source_location>
#include <filesystem>

#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>

// _CrtSetReportMode / _set_error_mode: main.cpp routes Debug assert output to
// stderr instead of a message box so the self-check can run from a script.
#include <crtdbg.h>
#include <cstdlib>

// Win32 returns wchar_t (UTF-16); the logger writes char (UTF-8).
//
// NOT std::string(ws.begin(), ws.end()): that common idiom narrows each
// wchar_t to a char, which warns (C4244, and warnings are errors here) and
// mangles every non-ASCII character. Monitor names can contain them.
inline std::string ToUtf8(std::wstring_view ws)
{
    if (ws.empty())
        return {};

    // Ask for the required size, then fill. Standard Win32 two-call pattern.
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

// Overlay mode: input transparency or flip-model presentation, not both.
//
// The choice spans two files (OverlayWindow sets the window style, D3DRenderer
// the swap effect), so it lives here as the single decision point.
//
// Layered, the default:
//   WS_EX_LAYERED | WS_EX_TRANSPARENT  -> clicks pass through
//   DXGI_SWAP_EFFECT_DISCARD (blt)     -> works on a layered window
//
//   Layered is required. WS_EX_TRANSPARENT on its own, even with
//   WM_NCHITTEST returning HTTRANSPARENT, does NOT give click-through; the
//   overlay swallowed every click when layered was dropped. Flip model in turn
//   refuses layered windows: CreateSwapChainForHwnd returns
//   DXGI_ERROR_INVALID_CALL.
//
//   A window made layered via SetLayeredWindowAttributes keeps its normal
//   redirection surface, so D3D drawing still works. The UpdateLayeredWindow
//   route would not.
//
// BM_OVERLAY_FLIP=1 restores the flip path: lower latency, no click-through.
inline bool UseFlipOverlay()
{
    static const bool flip = []() {
        wchar_t buf[8]{};
        const DWORD n = GetEnvironmentVariableW(L"BM_OVERLAY_FLIP", buf, 8);
        return (n > 0 && n < 8 && buf[0] == L'1');
    }();
    return flip;
}

// Staying above popups: two flawed options, pick one.
//
// On, the default: re-assert topmost so the popup is only ever seen magnified.
// But a fully occluded window stops repainting, so the desktop composition
// keeps its last painted state and our capture magnifies a FROZEN copy;
// highlight does not follow the mouse through a menu.
//
// Off (BM_NO_TOPMOST_FIGHT=1): the popup stays above us, live and correct, but
// unmagnified, and our magnified copy behind it means it appears twice.
//
// Neither is right. Composition-level magnification (Magnification API) is,
// and that costs per-monitor independent zoom.
inline bool FightPopupZOrder()
{
    static const bool off = []() {
        wchar_t buf[8]{};
        const DWORD n = GetEnvironmentVariableW(L"BM_NO_TOPMOST_FIGHT", buf, 8);
        return (n > 0 && n < 8 && buf[0] == L'1');
    }();
    return !off;
}

template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;
