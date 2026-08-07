#include "pch.h"
#include "SystemCursor.h"
#include "Logger.h"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <vector>

namespace BetterMagnifier {
namespace {

using MagInitializeFn       = BOOL (WINAPI*)();
using MagUninitializeFn     = BOOL (WINAPI*)();
using MagShowSystemCursorFn = BOOL (WINAPI*)(BOOL);

HMODULE               g_magDll        = nullptr;
MagShowSystemCursorFn g_magShowCursor = nullptr;
MagUninitializeFn     g_magUninit     = nullptr;

std::atomic<bool> g_probed{false};
std::atomic<bool> g_magAvailable{false};
std::atomic<bool> g_hidden{false};

std::terminate_handler g_previousTerminate = nullptr;

// The OCR_* constants only exist when OEMRESOURCE is defined before windows.h,
// and windows.h arrives through the precompiled header. Spelling the values
// out is less fragile than reaching back into pch.h for one file's sake.
//
// OCR_SIZE (32640) and OCR_ICON (32641) are deliberately absent: they are
// obsolete and SetSystemCursor rejects them.
constexpr WORD kCursorIds[] = {
    32512, // OCR_NORMAL
    32513, // OCR_IBEAM
    32514, // OCR_WAIT
    32515, // OCR_CROSS
    32516, // OCR_UP
    32642, // OCR_SIZENWSE
    32643, // OCR_SIZENESW
    32644, // OCR_SIZEWE
    32645, // OCR_SIZENS
    32646, // OCR_SIZEALL
    32648, // OCR_NO
    32649, // OCR_HAND
    32650, // OCR_APPSTARTING
};

// A fully transparent cursor at the system cursor size.
//
// Not 1x1: CreateCursor is documented to want the system dimensions, and an
// off-size cursor is where this quietly fails on some drivers. AND = 1 keeps
// the screen, XOR = 0 does not invert it, so every pixel is transparent.
//
// Recreated per call because SetSystemCursor takes ownership of the handle.
HCURSOR MakeBlankCursor()
{
    const int w = GetSystemMetrics(SM_CXCURSOR);
    const int h = GetSystemMetrics(SM_CYCURSOR);
    if (w <= 0 || h <= 0)
        return nullptr;

    const size_t stride = static_cast<size_t>((w + 7) / 8);
    std::vector<BYTE> andMask(stride * h, 0xFF);
    std::vector<BYTE> xorMask(stride * h, 0x00);

    return CreateCursor(GetModuleHandleW(nullptr), 0, 0, w, h,
                        andMask.data(), xorMask.data());
}

// The single reliable undo for SetSystemCursor: reload every system cursor
// from the user's own registry settings.
void RestoreFromRegistry()
{
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
}

BOOL WINAPI ConsoleCtrlHandler(DWORD)
{
    SystemCursor::Restore();
    return FALSE;   // let the default handler carry on
}

void TerminateHandler()
{
    SystemCursor::Restore();
    if (g_previousTerminate)
        g_previousTerminate();
    std::abort();
}

} // namespace

bool SystemCursor::Probe()
{
    if (g_probed.exchange(true))
        return g_magAvailable.load();

    g_magDll = LoadLibraryW(L"magnification.dll");
    if (!g_magDll)
    {
        LOG_WARN("magnification.dll not present — pointer compositing gated");
        return false;
    }

    auto init = reinterpret_cast<MagInitializeFn>(
                    GetProcAddress(g_magDll, "MagInitialize"));
    g_magUninit = reinterpret_cast<MagUninitializeFn>(
                    GetProcAddress(g_magDll, "MagUninitialize"));
    g_magShowCursor = reinterpret_cast<MagShowSystemCursorFn>(
                    GetProcAddress(g_magDll, "MagShowSystemCursor"));

    if (!init || !g_magShowCursor)
    {
        LOG_WARN("MagInitialize/MagShowSystemCursor missing — pointer compositing gated");
        return false;
    }

    if (!init())
    {
        LOG_WARN("MagInitialize failed ({}) — pointer compositing gated", GetLastError());
        return false;
    }

    // Prove the call works rather than trusting that it resolved. A round trip
    // returning FALSE means this build of Windows refuses it for us, and the
    // whole cursor design must not lean on it. One frame of flicker at startup
    // is a cheap price for not discovering this mid-session.
    const BOOL hid = g_magShowCursor(FALSE);
    g_magShowCursor(TRUE);

    g_magAvailable.store(hid != FALSE);
    LOG_INFO("MagShowSystemCursor probe: {}", hid ? "AVAILABLE" : "REFUSED");
    return g_magAvailable.load();
}

bool SystemCursor::MagPathAvailable() { return g_magAvailable.load(); }
bool SystemCursor::IsHidden()         { return g_hidden.load(); }

void SystemCursor::Hide()
{
    if (g_hidden.exchange(true))
        return;

    if (g_magAvailable.load())
    {
        g_magShowCursor(FALSE);
        return;
    }

    for (WORD id : kCursorIds)
    {
        if (HCURSOR blank = MakeBlankCursor())
            SetSystemCursor(blank, id);   // takes ownership of blank
    }
}

void SystemCursor::Restore()
{
    if (!g_hidden.exchange(false))
        return;

    if (g_magAvailable.load())
    {
        g_magShowCursor(TRUE);
        return;
    }

    RestoreFromRegistry();
}

void SystemCursor::InstallGuards()
{
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    g_previousTerminate = std::set_terminate(TerminateHandler);
}

} // namespace BetterMagnifier
