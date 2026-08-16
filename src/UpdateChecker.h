#pragma once

// Checks GitHub Releases for a newer build and, on the user's word, downloads,
// verifies and hands one to the installer.
//
// Runs on a thread of its own: a network call would stall Present on the render
// thread and sit in front of every event on the input thread. Talks to the
// engine by PostMessage and StatusSnapshot atomics, like everything else here.
//
// The free functions are pure logic so --self-check can assert them — the same
// split ViewportController uses.

#ifndef BETTER_MAGNIFIER_UPDATE_CHECKER_H
#define BETTER_MAGNIFIER_UPDATE_CHECKER_H

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace BetterMagnifier {

struct ReleaseAsset
{
    std::wstring name;
    std::wstring url;
    uint64_t     size = 0;
};

struct ReleaseInfo
{
    std::wstring version;   // "0.2.0" — the leading 'v' already stripped
    ReleaseAsset setup;     // the *-setup.exe asset
    ReleaseAsset sums;      // SHA256SUMS.txt; url is empty when absent
};

// Fills `out` from a GitHub /releases/latest body (UTF-8). False when it does
// not parse, has no usable tag, or has no setup asset — all of which mean the
// same thing: nothing to offer. `out` is untouched on failure, so a half-filled
// parse cannot leave a version pointing at another release's URL.
bool ParseRelease(std::string_view json, ReleaseInfo& out);

// -1 / 0 / +1. Compares the numeric triple and ignores any suffix; the feed
// never offers prereleases. Unparseable input compares as 0.0.0 rather than
// throwing — a garbage feed should read as "nothing newer".
int CompareVersion(std::wstring_view a, std::wstring_view b);

// Pulls one file's digest out of a `<64 hex chars>  <name>` listing, as written
// by sha256sum and by our release workflow. Output is lowercase hex.
bool ParseSha256Sums(std::string_view text, std::wstring_view fileName,
                     std::string& outHex);

// Guards the one place a remote document chooses where we connect. Requires
// https, a github.com or githubusercontent.com host or a real subdomain of one,
// no userinfo and no explicit port.
bool IsTrustedDownloadUrl(std::wstring_view url);

// Lowercase hex, no separators — the form SHA256SUMS.txt uses.
std::string HexEncodeLower(const std::vector<unsigned char>& bytes);

// Same directory, compared the way Windows compares paths: case-insensitively,
// ignoring a trailing separator. Split out of IsInstalledCopy so the decision
// can be asserted without a registry.
bool PathsNameSameDirectory(std::wstring_view a, std::wstring_view b);

// True when this binary runs from the directory
// HKLM\SOFTWARE\BetterMagnifier\InstallDir names. Auto-update is offered only
// to an installed copy — a portable one has nothing for a setup to replace.
bool IsInstalledCopy();

#ifdef _DEBUG
// Assert-based self-check for the functions above, run from main on Debug
// startup and by --self-check.
void UpdateCheckerSelfCheck();
#endif

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_UPDATE_CHECKER_H
