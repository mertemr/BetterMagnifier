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

#include "StatusSnapshot.h"   // UpdateState

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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

// ── The parts that touch the network ──────────────────────────────────────

// Where a downloaded installer is staged, under %TEMP%.
std::filesystem::path UpdateStagingDir();

// Removes it, at startup, so an abandoned update leaves no installers behind.
void ClearUpdateStagingDir();

// The API endpoint, or whatever BM_UPDATE_FEED names — the override lets a
// test release be checked against without publishing one.
std::wstring UpdateFeedUrl();

// Blocking. Fetches and parses the feed, rejecting a setup URL that is not on a
// GitHub host. Any failure returns false with one WARN line and nothing else
// said. Called on the update thread and by --check-update.
bool FetchLatestRelease(std::wstring_view feedUrl, ReleaseInfo& out);

class UpdateChecker
{
public:
    UpdateChecker() = default;
    ~UpdateChecker();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    // Starts the worker. engineHwnd receives WM_APP_UPDATE_STATE; status is
    // written but never read back for a decision.
    void Start(HWND engineHwnd, StatusSnapshot* status);

    // Signals the worker and joins it. Idempotent.
    void Stop();

    // The 24-hour floor is enforced in App, next to the settings that describe
    // it; `force` is only a marker for the caller.
    void RequestCheck(bool force);

    // Download the last-seen release, verify it, and hand it to the installer.
    // Refuses in a portable copy: there is nothing for a setup to replace.
    void RequestInstall();

    // The last successful result. False when there has not been one.
    bool LatestRelease(ReleaseInfo& out) const;

private:
    void ThreadMain();
    void PublishState(UpdateState state);
    void RunCheck();

    std::thread       m_thread;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_checkRequested{false};

    // Signalled by RequestCheck and by Stop, so the worker never polls.
    HANDLE m_wake = nullptr;

    HWND            m_engineHwnd = nullptr;
    StatusSnapshot* m_status     = nullptr;

    // The one lock here, nowhere near the render path: it guards a wstring,
    // which cannot be an atomic, and is held for the length of a copy.
    mutable std::mutex m_resultLock;
    ReleaseInfo        m_latest;
    bool               m_haveLatest = false;
};

#ifdef _DEBUG
// Assertions for the pure functions above; run by --self-check.
void UpdateCheckerSelfCheck();
#endif

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_UPDATE_CHECKER_H
