#pragma once

// Hides and restores the system mouse pointer.
//
// The primary path is magnification.dll's MagShowSystemCursor. It is preferred
// for one reason that outweighs convenience: its effect dies with the process.
// SetSystemCursor's does not — if BetterMagnifier is killed from Task Manager
// while the pointer is hidden, the user is left with no pointer at all until
// they sign out. On an accessibility tool that is the worst possible failure,
// so the SetSystemCursor fallback is gated behind MagPathAvailable and an
// explicit opt-in.
//
// All members are static: there is exactly one system pointer.

#ifndef BETTER_MAGNIFIER_SYSTEM_CURSOR_H
#define BETTER_MAGNIFIER_SYSTEM_CURSOR_H

#include <windows.h>

namespace BetterMagnifier {

class SystemCursor
{
public:
    // Loads magnification.dll, calls MagInitialize, and round-trips
    // MagShowSystemCursor to prove it actually works rather than merely
    // exists. Call once at startup. Idempotent.
    static bool Probe();

    // Probe()'s result. Callers gate pointer compositing on this.
    static bool MagPathAvailable();

    // Idempotent. Only call while at least one monitor is magnified, so the
    // window in which the pointer is hidden is exactly "actively magnifying".
    static void Hide();

    // Idempotent and safe from any exit path, including a terminate handler.
    static void Restore();

    static bool IsHidden();

    // Registers the console-control and terminate handlers that call Restore.
    // Call once, after Probe.
    static void InstallGuards();
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_SYSTEM_CURSOR_H
