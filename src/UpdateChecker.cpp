#include "pch.h"
#include "UpdateChecker.h"
#include "Version.h"
#include "Logger.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <cctype>

namespace {

// The API answers UTF-8; JsonObject wants UTF-16. The reverse of ToUtf8 in
// pch.h, and here because this is the only place that needs it.
std::wstring Utf8ToWide(std::string_view utf8)
{
    if (utf8.empty())
        return {};

    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);

    if (needed <= 0)
        return {};

    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), needed);
    return wide;
}

bool EndsWith(std::wstring_view text, std::wstring_view suffix)
{
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Reads the leading numeric triple; anything non-numeric ends the scan, so
// "1.2.3-beta" equals "1.2.3". The feed never offers prereleases, and a suffix
// we cannot order is better ignored than guessed at.
void ParseTriple(std::wstring_view v, int out[3])
{
    out[0] = out[1] = out[2] = 0;

    if (!v.empty() && (v.front() == L'v' || v.front() == L'V'))
        v.remove_prefix(1);

    int  index = 0;
    int  acc   = 0;
    bool seen  = false;

    for (const wchar_t c : v)
    {
        if (c >= L'0' && c <= L'9')
        {
            acc  = acc * 10 + (c - L'0');
            seen = true;
        }
        else if (c == L'.')
        {
            out[index] = seen ? acc : 0;
            acc  = 0;
            seen = false;
            if (++index > 2)
                return;
        }
        else
        {
            break;
        }
    }

    out[index] = seen ? acc : 0;
}

} // namespace

namespace BetterMagnifier {

int CompareVersion(std::wstring_view a, std::wstring_view b)
{
    int va[3]{}, vb[3]{};
    ParseTriple(a, va);
    ParseTriple(b, vb);

    for (int i = 0; i < 3; ++i)
        if (va[i] != vb[i])
            return va[i] < vb[i] ? -1 : 1;

    return 0;
}

bool IsTrustedDownloadUrl(std::wstring_view url)
{
    constexpr std::wstring_view kScheme = L"https://";

    if (url.size() <= kScheme.size())
        return false;
    if (url.compare(0, kScheme.size(), kScheme) != 0)
        return false;

    const std::wstring_view rest  = url.substr(kScheme.size());
    const size_t            slash = rest.find(L'/');
    const std::wstring_view host  =
        (slash == std::wstring_view::npos) ? rest : rest.substr(0, slash);

    if (host.empty())
        return false;

    // The userinfo form is a well-worn way to make a URL read as one host and
    // resolve as another. Refuse it rather than reason about it.
    if (host.find(L'@') != std::wstring_view::npos)
        return false;

    // No explicit port. GitHub serves on 443 and nothing else should be humoured.
    if (host.find(L':') != std::wstring_view::npos)
        return false;

    constexpr std::wstring_view kDomains[] = {
        L"github.com",
        L"githubusercontent.com",
    };

    for (const std::wstring_view domain : kDomains)
    {
        if (host == domain)
            return true;

        // A real subdomain, so "github.com.evil.com" cannot pass as one: the
        // character before the suffix has to be the label separator.
        if (host.size() > domain.size() + 1 &&
            host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
            host[host.size() - domain.size() - 1] == L'.')
            return true;
    }

    return false;
}

std::string HexEncodeLower(const std::vector<unsigned char>& bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";

    std::string out;
    out.reserve(bytes.size() * 2);

    for (const unsigned char b : bytes)
    {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }

    return out;
}

bool ParseSha256Sums(std::string_view text, std::wstring_view fileName,
                     std::string& outHex)
{
    // Asset names are ASCII by construction: the workflow builds them from the
    // version number.
    std::string want;
    want.reserve(fileName.size());
    for (const wchar_t c : fileName)
    {
        if (c > 127)
            return false;
        want.push_back(static_cast<char>(c));
    }

    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t eol = text.find('\n', pos);
        std::string_view line = text.substr(
            pos, (eol == std::string_view::npos ? text.size() : eol) - pos);
        pos = (eol == std::string_view::npos) ? text.size() : eol + 1;

        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);

        if (line.size() < 65)
            continue;

        const std::string_view hex = line.substr(0, 64);

        bool hexOk = true;
        for (const char c : hex)
        {
            if (!std::isxdigit(static_cast<unsigned char>(c)))
            {
                hexOk = false;
                break;
            }
        }
        if (!hexOk)
            continue;

        // sha256sum writes two spaces for text mode and " *" for binary.
        std::string_view name = line.substr(64);
        while (!name.empty() &&
               (name.front() == ' ' || name.front() == '\t' || name.front() == '*'))
            name.remove_prefix(1);

        if (name != want)
            continue;

        outHex.assign(hex);
        for (char& c : outHex)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        return true;
    }

    return false;
}

bool ParseRelease(std::string_view json, ReleaseInfo& out)
{
    namespace WDJ = winrt::Windows::Data::Json;

    const std::wstring wide = Utf8ToWide(json);
    if (wide.empty())
        return false;

    // GetNamed* throws on a wrong-typed key, which is exactly what a document
    // we do not control produces. Every failure here means the same thing.
    try
    {
        WDJ::JsonObject root{ nullptr };
        if (!WDJ::JsonObject::TryParse(winrt::hstring{ wide }, root))
            return false;

        if (!root.HasKey(L"tag_name") || !root.HasKey(L"assets"))
            return false;

        ReleaseInfo info;

        info.version = root.GetNamedString(L"tag_name").c_str();
        if (!info.version.empty() &&
            (info.version.front() == L'v' || info.version.front() == L'V'))
            info.version.erase(0, 1);

        if (info.version.empty())
            return false;

        for (const auto& value : root.GetNamedArray(L"assets"))
        {
            const WDJ::JsonObject asset = value.GetObject();

            ReleaseAsset a;
            a.name = asset.GetNamedString(L"name", L"").c_str();
            a.url  = asset.GetNamedString(L"browser_download_url", L"").c_str();
            a.size = static_cast<uint64_t>(asset.GetNamedNumber(L"size", 0.0));

            if (a.url.empty() || a.name.empty())
                continue;

            if (EndsWith(a.name, L"-setup.exe"))
                info.setup = a;
            else if (a.name == L"SHA256SUMS.txt")
                info.sums = a;
        }

        // A release with no installer is not an update we can offer.
        if (info.setup.url.empty())
            return false;

        out = std::move(info);
        return true;
    }
    catch (const winrt::hresult_error&)
    {
        return false;
    }
}

bool PathsNameSameDirectory(std::wstring_view a, std::wstring_view b)
{
    const auto trim = [](std::wstring_view p) {
        while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
            p.remove_suffix(1);
        return p;
    };

    a = trim(a);
    b = trim(b);

    if (a.empty() || b.empty())
        return false;
    if (a.size() != b.size())
        return false;

    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

bool IsInstalledCopy()
{
    wchar_t installDir[MAX_PATH]{};
    DWORD   size = sizeof(installDir);

    const LSTATUS st = RegGetValueW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\BetterMagnifier", L"InstallDir",
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, installDir, &size);

    if (st != ERROR_SUCCESS)
        return false;

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return false;

    const std::filesystem::path exeDir =
        std::filesystem::path(exePath).parent_path();

    return PathsNameSameDirectory(exeDir.wstring(), installDir);
}

#ifdef _DEBUG

// =============================================================================
// UpdateCheckerSelfCheck — assertions for the pure logic
// =============================================================================
// Everything asserted here decides either what version we think is available or
// whether we are willing to execute a downloaded file. Neither is a place to
// find out by observation.
// =============================================================================
void UpdateCheckerSelfCheck()
{
    LOG_INFO("UpdateChecker self-check starting");

    // ── 1. CompareVersion: ordering ──
    {
        BM_SELFCHECK(CompareVersion(L"1.0.0", L"1.0.0") == 0);
        BM_SELFCHECK(CompareVersion(L"1.0.0", L"1.0.1") < 0);
        BM_SELFCHECK(CompareVersion(L"1.0.1", L"1.0.0") > 0);
        BM_SELFCHECK(CompareVersion(L"1.9.0", L"1.10.0") < 0);   // not lexical
        BM_SELFCHECK(CompareVersion(L"2.0.0", L"1.99.99") > 0);
    }

    // ── 2. CompareVersion: the shapes a feed actually delivers ──
    {
        BM_SELFCHECK(CompareVersion(L"v1.2.3", L"1.2.3") == 0);   // tag vs version
        BM_SELFCHECK(CompareVersion(L"1.2", L"1.2.0") == 0);      // missing patch
        BM_SELFCHECK(CompareVersion(L"1.2.3-beta", L"1.2.3") == 0);
    }

    // ── 3. CompareVersion: garbage reads as 0.0.0, never as an exception ──
    {
        BM_SELFCHECK(CompareVersion(L"", L"0.0.0") == 0);
        BM_SELFCHECK(CompareVersion(L"not-a-version", L"0.0.0") == 0);
        BM_SELFCHECK(CompareVersion(L"", L"0.0.1") < 0);
    }

    // ── 4. IsTrustedDownloadUrl ──
    //
    // This is the guard on where we connect and what we execute, so the
    // rejections matter more than the acceptances.
    {
        BM_SELFCHECK(IsTrustedDownloadUrl(
            L"https://github.com/mertemr/BetterMagnifier/releases/download/v1/x-setup.exe"));
        BM_SELFCHECK(IsTrustedDownloadUrl(
            L"https://objects.githubusercontent.com/github-production-release-asset/1/2"));

        BM_SELFCHECK(!IsTrustedDownloadUrl(L"http://github.com/a"));       // not TLS
        BM_SELFCHECK(!IsTrustedDownloadUrl(L"https://evil.com/a"));
        BM_SELFCHECK(!IsTrustedDownloadUrl(L"https://notgithub.com/a"));
        BM_SELFCHECK(!IsTrustedDownloadUrl(L"https://github.com.evil.com/a"));
        BM_SELFCHECK(!IsTrustedDownloadUrl(L"https://evil.com@github.com/a"));
        BM_SELFCHECK(!IsTrustedDownloadUrl(L"https://github.com:8443/a"));
        BM_SELFCHECK(!IsTrustedDownloadUrl(L""));
        BM_SELFCHECK(!IsTrustedDownloadUrl(L"https://"));
    }

    // ── 5. ParseSha256Sums ──
    {
        const std::string sums =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
            "BetterMagnifier-0.2.0-x64-setup.exe\r\n"
            "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB *"
            "BetterMagnifier-0.2.0-x64-portable.zip\n";

        std::string hex;
        BM_SELFCHECK(ParseSha256Sums(sums, L"BetterMagnifier-0.2.0-x64-setup.exe", hex));
        BM_SELFCHECK(hex == std::string(64, 'a'));

        // Uppercase digests normalise, and the '*' binary marker is skipped.
        BM_SELFCHECK(ParseSha256Sums(sums, L"BetterMagnifier-0.2.0-x64-portable.zip", hex));
        BM_SELFCHECK(hex == std::string(64, 'b'));

        BM_SELFCHECK(!ParseSha256Sums(sums, L"absent.exe", hex));
        BM_SELFCHECK(!ParseSha256Sums("", L"anything.exe", hex));
        BM_SELFCHECK(!ParseSha256Sums("garbage line\n", L"anything.exe", hex));

        // A name that is a prefix of a listed one must not match it.
        BM_SELFCHECK(!ParseSha256Sums(sums, L"BetterMagnifier-0.2.0-x64-setup", hex));
    }

    // ── 6. HexEncodeLower ──
    //
    // The digest comparison is what decides whether we run a downloaded
    // binary, so the formatting either side of it is worth pinning down.
    {
        const std::vector<unsigned char> digest{ 0x00, 0x0f, 0xa0, 0xff };
        BM_SELFCHECK(HexEncodeLower(digest) == "000fa0ff");
        BM_SELFCHECK(HexEncodeLower({}).empty());
    }

    // ── 7. ParseRelease: the response we actually get ──
    {
        const std::string json = R"JSON({
            "tag_name": "v0.2.0",
            "name": "BetterMagnifier 0.2.0",
            "prerelease": false,
            "assets": [
                { "name": "BetterMagnifier-0.2.0-x64-setup.exe",
                  "size": 41943040,
                  "browser_download_url": "https://github.com/mertemr/BetterMagnifier/releases/download/v0.2.0/BetterMagnifier-0.2.0-x64-setup.exe" },
                { "name": "BetterMagnifier-0.2.0-x64-portable.zip",
                  "size": 2097152,
                  "browser_download_url": "https://github.com/mertemr/BetterMagnifier/releases/download/v0.2.0/BetterMagnifier-0.2.0-x64-portable.zip" },
                { "name": "SHA256SUMS.txt",
                  "size": 178,
                  "browser_download_url": "https://github.com/mertemr/BetterMagnifier/releases/download/v0.2.0/SHA256SUMS.txt" }
            ]
        })JSON";

        ReleaseInfo info;
        BM_SELFCHECK(ParseRelease(json, info));
        BM_SELFCHECK(info.version == L"0.2.0");              // the 'v' is stripped
        BM_SELFCHECK(info.setup.name == L"BetterMagnifier-0.2.0-x64-setup.exe");
        BM_SELFCHECK(info.setup.size == 41943040);
        BM_SELFCHECK(IsTrustedDownloadUrl(info.setup.url));
        BM_SELFCHECK(info.sums.name == L"SHA256SUMS.txt");
        BM_SELFCHECK(!info.sums.url.empty());
    }

    // ── 8. ParseRelease: a release with nothing to install is a failure ──
    {
        const std::string noSetup = R"JSON({
            "tag_name": "v0.2.0",
            "assets": [
                { "name": "notes.txt", "size": 12,
                  "browser_download_url": "https://github.com/mertemr/BetterMagnifier/releases/download/v0.2.0/notes.txt" }
            ]
        })JSON";

        ReleaseInfo info;
        BM_SELFCHECK(!ParseRelease(noSetup, info));
    }

    // ── 9. ParseRelease: malformed input leaves the output untouched ──
    {
        ReleaseInfo info;
        info.version = L"sentinel";

        BM_SELFCHECK(!ParseRelease("", info));
        BM_SELFCHECK(!ParseRelease("{\"tag_name\":", info));               // truncated
        BM_SELFCHECK(!ParseRelease("[1,2,3]", info));                      // not an object
        BM_SELFCHECK(!ParseRelease("{\"message\":\"Not Found\"}", info));  // API error body
        BM_SELFCHECK(!ParseRelease("{\"tag_name\":\"\",\"assets\":[]}", info));
        BM_SELFCHECK(!ParseRelease("{\"tag_name\":123,\"assets\":[]}", info));  // wrong type

        BM_SELFCHECK(info.version == L"sentinel");
    }

    // ── 10. PathsNameSameDirectory ──
    {
        BM_SELFCHECK(PathsNameSameDirectory(
            L"C:\\Program Files\\BetterMagnifier",
            L"C:\\Program Files\\BetterMagnifier"));

        // Windows paths are case-insensitive, and whether the registry value
        // carries a trailing separator is not something we get to depend on.
        BM_SELFCHECK(PathsNameSameDirectory(
            L"C:\\Program Files\\BetterMagnifier",
            L"c:\\program files\\bettermagnifier"));
        BM_SELFCHECK(PathsNameSameDirectory(
            L"C:\\Program Files\\BetterMagnifier\\",
            L"C:\\Program Files\\BetterMagnifier"));

        BM_SELFCHECK(!PathsNameSameDirectory(
            L"C:\\Program Files\\BetterMagnifier",
            L"C:\\Program Files\\BetterMagnifier2"));
        BM_SELFCHECK(!PathsNameSameDirectory(L"", L"C:\\x"));
        BM_SELFCHECK(!PathsNameSameDirectory(L"C:\\x", L""));
        BM_SELFCHECK(!PathsNameSameDirectory(L"", L""));
    }

    LOG_INFO("UpdateChecker self-check passed");
}

#endif // _DEBUG

} // namespace BetterMagnifier
