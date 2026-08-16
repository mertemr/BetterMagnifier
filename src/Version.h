#pragma once

// The single source of the application's version.
//
// Also included by BetterMagnifier.rc, so it stays preprocessor-only: nothing
// rc.exe cannot parse. RC_INVOKED guards the token pasting.
//
// Do not edit the numbers by hand — release-please rewrites the marked lines,
// and tools\check-version.ps1 asserts they match version.txt, app.manifest and
// the resource in the built exe.
//
// The triple and the string are separate literals rather than one built from
// the other: deriving it needs the # operator, and rc.exe's preprocessor is not
// the one the rest of the project compiles against.

#ifndef BETTER_MAGNIFIER_VERSION_H
#define BETTER_MAGNIFIER_VERSION_H

#define BM_VERSION_MAJOR 0 // x-release-please-major
#define BM_VERSION_MINOR 2 // x-release-please-minor
#define BM_VERSION_PATCH 0 // x-release-please-patch

#define BM_VERSION_STRING "0.2.0" // x-release-please-version

// The four-part form Windows shows in file properties. The fourth component is
// always zero: this project releases on the semantic triple.
#define BM_VERSION_STRING_FULL BM_VERSION_STRING ".0"

#ifndef RC_INVOKED
    #define BM_VERSION_WIDEN_(x) L##x
    #define BM_VERSION_WIDEN(x)  BM_VERSION_WIDEN_(x)

    #define BM_VERSION_STRING_W BM_VERSION_WIDEN(BM_VERSION_STRING)
#endif

#endif // BETTER_MAGNIFIER_VERSION_H
