#pragma once

// Hides and restores the system mouse pointer.
//
// One path only: magnification.dll's MagShowSystemCursor. There IS another way
// — SetSystemCursor with a blank cursor — and it is deliberately not here. Its
// effect is documented to be global and persistent, so a kill from Task
// Manager would leave the user with no mouse pointer at all until they sign
// out. On an accessibility tool that is the worst possible failure, and no
// feature is worth risking it.
//
// When MagShowSystemCursor is unavailable we therefore do not hide the pointer
// at all. Callers gate on MagPathAvailable and fall back to native pointer
// behaviour. Degrade, do not improvise.
//
// UNVERIFIED, and the reason InstallGuards still exists: MagShowSystemCursor's
// effect is *expected* to die with the process, but that has not been proved
// here — proving it means hiding the pointer on a live machine and hard-killing
// the process, which is the very outcome being guarded against. Until someone
// tests it on a spare machine, the exit guards are cheap insurance against the
// assumption being wrong.
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

    // Probe()'s result. Callers MUST gate pointer compositing on this: when it
    // is false, Hide() does nothing and the pointer stays visible.
    static bool MagPathAvailable();

    // Idempotent, and a no-op unless MagPathAvailable. Only call while at least
    // one monitor is magnified, so the window in which the pointer is hidden is
    // exactly "actively magnifying".
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
