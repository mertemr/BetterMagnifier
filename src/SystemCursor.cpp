#include "pch.h"
#include "SystemCursor.h"
#include "Logger.h"

#include <atomic>
#include <cstdlib>
#include <exception>

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
    // No Magnification path means we do not hide the pointer, full stop. The
    // caller is expected to have checked, but this is the last line of defence
    // and it must fail closed.
    if (!g_magAvailable.load())
        return;

    if (g_hidden.exchange(true))
        return;

    g_magShowCursor(FALSE);
}

void SystemCursor::Restore()
{
    if (!g_hidden.exchange(false))
        return;

    if (g_magAvailable.load())
        g_magShowCursor(TRUE);
}

void SystemCursor::InstallGuards()
{
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    g_previousTerminate = std::set_terminate(TerminateHandler);
}

} // namespace BetterMagnifier
