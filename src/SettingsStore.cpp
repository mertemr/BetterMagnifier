// =============================================================================
// SettingsStore.cpp — INI kalicilik implementasyonu
// =============================================================================

#include "pch.h"
#include "SettingsStore.h"
#include "Logger.h"

#include <shlobj.h>       // SHGetKnownFolderPath, FOLDERID_RoamingAppData
#include <cwctype>        // towlower, towupper
#include <cwchar>         // wcstof

namespace BetterMagnifier {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Case-insensitive compare done by hand: _wcsicmp wants null-terminated input
// and a string_view is not obliged to be.
bool EqualsCI(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        if (towlower(a[i]) != towlower(b[i]))
            return false;
    }
    return true;
}

// "ctrl" -> MOD_CONTROL. Taninmazsa 0.
UINT ModifierFromName(std::wstring_view name)
{
    if (EqualsCI(name, L"ctrl"))  return MOD_CONTROL;
    if (EqualsCI(name, L"alt"))   return MOD_ALT;
    if (EqualsCI(name, L"shift")) return MOD_SHIFT;
    if (EqualsCI(name, L"win"))   return MOD_WIN;
    return 0;
}

// "Z" -> 'Z', "F12" -> VK_F12. Taninmazsa 0.
UINT VirtualKeyFromName(std::wstring_view name)
{
    if (name.empty())
        return 0;

    // Tek karakter: A-Z veya 0-9
    if (name.size() == 1)
    {
        const wchar_t c = static_cast<wchar_t>(towupper(name[0]));
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))
            return static_cast<UINT>(c);
        return 0;
    }

    // F1 - F24
    if (towupper(name[0]) == L'F')
    {
        UINT num = 0;
        for (size_t i = 1; i < name.size(); ++i)
        {
            if (name[i] < L'0' || name[i] > L'9')
                return 0;
            num = num * 10 + static_cast<UINT>(name[i] - L'0');
        }
        if (num >= 1 && num <= 24)
            return VK_F1 + (num - 1);
    }

    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// INI float yardimcilari
// INI has no float type, so these go through text.
//
// std::format rather than std::to_wstring: to_wstring is locale-dependent and
// will happily write "1,25" on a locale that uses a decimal comma, which
// wcstof then reads back as 1. std::format always emits '.', so the file stays
// portable between machines.
float ReadFloat(const std::wstring& file, const std::wstring& section,
                const wchar_t* key, float fallback)
{
    wchar_t buf[32]{};
    GetPrivateProfileStringW(section.c_str(), key, L"", buf, 32, file.c_str());
    if (buf[0] == L'\0')
        return fallback;

    wchar_t* endPtr = nullptr;
    const float v = std::wcstof(buf, &endPtr);
    if (endPtr == buf)      // hic rakam okunamadi
        return fallback;

    return v;
}

bool WriteFloat(const std::wstring& file, const std::wstring& section,
                const wchar_t* key, float value)
{
    const std::wstring text = std::format(L"{:.4g}", value);
    return WritePrivateProfileStringW(section.c_str(), key, text.c_str(), file.c_str()) != 0;
}

bool WriteInt(const std::wstring& file, const std::wstring& section,
              const wchar_t* key, int value)
{
    const std::wstring text = std::to_wstring(value);
    return WritePrivateProfileStringW(section.c_str(), key, text.c_str(), file.c_str()) != 0;
}

} // anonymous namespace

// =============================================================================
// ParseHotkey
// =============================================================================
// "Ctrl+Alt+Z" -> (MOD_CONTROL|MOD_ALT, 'Z')
//
// The output parameters are written only after the whole string has parsed.
// Writing as we go would corrupt the caller's current binding whenever the
// input turned out to be invalid halfway through.
// =============================================================================
bool ParseHotkey(std::wstring_view text, UINT& modifiers, UINT& vk)
{
    if (text.empty())
        return false;

    UINT parsedMods = 0;
    size_t start = 0;

    while (true)
    {
        const size_t plus = text.find(L'+', start);
        const std::wstring_view piece = (plus == std::wstring_view::npos)
            ? text.substr(start)
            : text.substr(start, plus - start);

        if (piece.empty())
            return false;   // malformed input such as "Ctrl+" or "Ctrl++Z"

        if (plus == std::wstring_view::npos)
        {
            // Last piece: it has to be the key, not another modifier.
            const UINT parsedVk = VirtualKeyFromName(piece);
            if (parsedVk == 0)
                return false;

            modifiers = parsedMods;
            vk        = parsedVk;
            return true;
        }

        const UINT mod = ModifierFromName(piece);
        if (mod == 0)
            return false;   // Taninmayan modifier

        parsedMods |= mod;
        start = plus + 1;
    }
}

// =============================================================================
// FormatHotkey
// =============================================================================
// Fixed order: Ctrl, Alt, Shift, Win. The round trip has to be stable — the
// same (modifiers, vk) must always produce the same text, or writing the file
// and reading it back would drift.
// =============================================================================
std::wstring FormatHotkey(UINT modifiers, UINT vk)
{
    std::wstring out;

    if (modifiers & MOD_CONTROL) out += L"Ctrl+";
    if (modifiers & MOD_ALT)     out += L"Alt+";
    if (modifiers & MOD_SHIFT)   out += L"Shift+";
    if (modifiers & MOD_WIN)     out += L"Win+";

    if (vk >= VK_F1 && vk <= VK_F24)
    {
        out += L'F';
        out += std::to_wstring(vk - VK_F1 + 1);
    }
    else
    {
        out += static_cast<wchar_t>(vk);
    }

    return out;
}

// =============================================================================
// FilePath — %APPDATA%\BetterMagnifier\settings.ini
// =============================================================================
// SHGetKnownFolderPath modern API (Vista+); eski SHGetFolderPath deprecated.
// The returned buffer is owned by the caller and must go back through
// CoTaskMemFree.
// =============================================================================
#ifdef _DEBUG
namespace {
std::filesystem::path g_pathOverride;
}

void SetSettingsPathOverride(const std::filesystem::path& p)
{
    g_pathOverride = p;
}
#endif

std::filesystem::path SettingsStore::FilePath()
{
#ifdef _DEBUG
    if (!g_pathOverride.empty())
        return g_pathOverride;
#endif

    PWSTR appData = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData);

    if (FAILED(hr) || !appData)
    {
        if (appData)
            CoTaskMemFree(appData);

        LOG_ERROR("SHGetKnownFolderPath failed: 0x{:08X}", static_cast<unsigned long>(hr));
        return {};
    }

    std::filesystem::path p =
        std::filesystem::path(appData) / L"BetterMagnifier" / L"settings.ini";
    CoTaskMemFree(appData);
    return p;
}

// =============================================================================
// Load
// =============================================================================
bool SettingsStore::Load()
{
    m_general = GeneralSettings{};   // varsayilanlara don
    m_monitors.clear();

    const auto path = FilePath();
    if (path.empty())
        return false;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        LOG_INFO("No settings file, using defaults");
        return true;   // First run is not a failure
    }

    const std::wstring file = path.wstring();

    // ParseHotkey leaves its outputs alone on failure and m_general already
    // holds the defaults, so a malformed entry falls back on its own.
    {
        wchar_t buf[64]{};
        GetPrivateProfileStringW(L"General", L"ToggleHotkey", L"", buf, 64, file.c_str());
        if (buf[0] != L'\0' &&
            !ParseHotkey(buf, m_general.toggleModifiers, m_general.toggleVk))
        {
            LOG_WARN("ToggleHotkey is unparseable, keeping the default");
        }

        buf[0] = L'\0';
        GetPrivateProfileStringW(L"General", L"FreezeHotkey", L"", buf, 64, file.c_str());
        if (buf[0] != L'\0' &&
            !ParseHotkey(buf, m_general.freezeModifiers, m_general.freezeVk))
        {
            LOG_WARN("FreezeHotkey is unparseable, keeping the default");
        }
    }

    // ── Bayraklar ──
    // GetPrivateProfileIntW takes the default as a parameter, so a missing key
    // eksik anahtar otomatik varsayilana duser.
    // Defaults to on: someone running this wants it instead of the OS Magnifier.
    m_general.hijackMagnifierKeys =
        GetPrivateProfileIntW(L"General", L"HijackMagnifierKeys", 1, file.c_str()) != 0;
    m_general.startWithWindows =
        GetPrivateProfileIntW(L"General", L"StartWithWindows", 0, file.c_str()) != 0;
    m_general.rememberZoomLevel =
        GetPrivateProfileIntW(L"General", L"RememberZoomLevel", 1, file.c_str()) != 0;

    // ── Takip modu ──
    {
        // EdgePush is the default now. It is what the pointer work was built
        // for, and with our own cursor it no longer costs the click alignment
        // that made Mouse the safe choice.
        wchar_t buf[32]{};
        GetPrivateProfileStringW(L"General", L"FollowMode", L"EdgePush",
                                 buf, 32, file.c_str());

        m_general.followMode =
            EqualsCI(buf, L"Mouse")         ? FollowMode::Mouse         :
            EqualsCI(buf, L"MouseAndFocus") ? FollowMode::MouseAndFocus :
                                              FollowMode::EdgePush;
    }

    // ── Edge push and pointer ──
    // Every one clamped on read. A hand-edited INI is a supported way to use
    // this file, and a nonsensical value must fall back on its own rather than
    // poison the rest of the section.
    m_general.edgeBandFraction = std::clamp(
        ReadFloat(file, L"General", L"EdgeBandFraction", 0.12f), 0.02f, 0.45f);

    m_general.pointerScaling =
        GetPrivateProfileIntW(L"General", L"PointerScaling", 1, file.c_str()) != 0;

    m_general.pointerSpeed = std::clamp(
        ReadFloat(file, L"General", L"PointerSpeed", 1.0f), 0.1f, 5.0f);

    m_general.pointerCompensation = std::clamp(
        ReadFloat(file, L"General", L"PointerCompensation", 0.2f), 0.0f, 1.0f);

    m_general.cursorScale = std::clamp(
        ReadFloat(file, L"General", L"CursorScale", 1.0f), 0.5f, 4.0f);

    m_general.lockPointerToMonitor =
        GetPrivateProfileIntW(L"General", L"LockPointerToMonitor", 1, file.c_str()) != 0;

    // ── Per-monitor section'lari ──
    // GetPrivateProfileSectionNamesW returns every section name in one buffer,
    // separated by '\0' and terminated by a double '\0'.
    {
        std::vector<wchar_t> names(8192);
        const DWORD len = GetPrivateProfileSectionNamesW(
            names.data(), static_cast<DWORD>(names.size()), file.c_str());

        const wchar_t* p   = names.data();
        const wchar_t* end = names.data() + len;

        while (p < end && *p != L'\0')
        {
            const std::wstring section(p);
            p += section.size() + 1;

            if (section == L"General")
                continue;

            MonitorSettings ms{};
            ms.minZoom  = ReadFloat(file, section, L"MinZoom",  1.0f);
            ms.maxZoom  = ReadFloat(file, section, L"MaxZoom",  10.0f);
            ms.zoomStep = ReadFloat(file, section, L"ZoomStep", 0.25f);
            ms.lastZoom = ReadFloat(file, section, L"LastZoom", 2.0f);

            // Nonsense falls back per field. This file is meant to be
            // hand-editable, and MaxZoom=0 should not brick the application.
            if (ms.minZoom  <= 0.0f)       ms.minZoom  = 1.0f;
            if (ms.maxZoom  <= ms.minZoom) ms.maxZoom  = 10.0f;
            if (ms.zoomStep <= 0.0f)       ms.zoomStep = 0.25f;
            if (ms.lastZoom < ms.minZoom || ms.lastZoom > ms.maxZoom)
                ms.lastZoom = ms.minZoom;

            m_monitors[section] = ms;
        }
    }

    LOG_INFO("Settings loaded: {} monitor entries", m_monitors.size());
    return true;
}

// =============================================================================
// Save
// =============================================================================
bool SettingsStore::Save() const
{
    const auto path = FilePath();
    if (path.empty())
        return false;

    // WritePrivateProfileStringW does NOT create the directory; we have to.
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        LOG_ERROR("Ayar klasoru olusturulamadi: {}", ec.message());
        return false;
    }

    const std::wstring file = path.wstring();

    const std::wstring toggle = FormatHotkey(m_general.toggleModifiers, m_general.toggleVk);
    const std::wstring freeze = FormatHotkey(m_general.freezeModifiers, m_general.freezeVk);

    bool ok = true;
    ok = WritePrivateProfileStringW(L"General", L"ToggleHotkey",
            toggle.c_str(), file.c_str()) != 0 && ok;
    ok = WritePrivateProfileStringW(L"General", L"FreezeHotkey",
            freeze.c_str(), file.c_str()) != 0 && ok;
    ok = WriteInt(file, L"General", L"HijackMagnifierKeys",
            m_general.hijackMagnifierKeys ? 1 : 0) && ok;
    ok = WriteInt(file, L"General", L"StartWithWindows",
            m_general.startWithWindows ? 1 : 0) && ok;
    ok = WriteInt(file, L"General", L"RememberZoomLevel",
            m_general.rememberZoomLevel ? 1 : 0) && ok;
    ok = WriteFloat(file, L"General", L"EdgeBandFraction",
                    m_general.edgeBandFraction) && ok;
    ok = WriteInt(file, L"General", L"PointerScaling",
                  m_general.pointerScaling ? 1 : 0) && ok;
    ok = WriteFloat(file, L"General", L"PointerSpeed",
                    m_general.pointerSpeed) && ok;
    ok = WriteFloat(file, L"General", L"PointerCompensation",
                    m_general.pointerCompensation) && ok;
    ok = WriteFloat(file, L"General", L"CursorScale",
                    m_general.cursorScale) && ok;
    ok = WriteInt(file, L"General", L"LockPointerToMonitor",
                  m_general.lockPointerToMonitor ? 1 : 0) && ok;

    ok = WritePrivateProfileStringW(L"General", L"FollowMode",
            (m_general.followMode == FollowMode::Mouse)         ? L"Mouse" :
            (m_general.followMode == FollowMode::MouseAndFocus) ? L"MouseAndFocus"
                                                                : L"EdgePush",
            file.c_str()) != 0 && ok;

    for (const auto& [device, ms] : m_monitors)
    {
        ok = WriteFloat(file, device, L"MinZoom",  ms.minZoom)  && ok;
        ok = WriteFloat(file, device, L"MaxZoom",  ms.maxZoom)  && ok;
        ok = WriteFloat(file, device, L"ZoomStep", ms.zoomStep) && ok;
        ok = WriteFloat(file, device, L"LastZoom", ms.lastZoom) && ok;
    }

    if (!ok)
        LOG_ERROR("At least one write failed while saving the settings");

    return ok;
}

MonitorSettings SettingsStore::Monitor(const std::wstring& deviceName) const
{
    const auto it = m_monitors.find(deviceName);
    return (it != m_monitors.end()) ? it->second : MonitorSettings{};
}

void SettingsStore::SetMonitor(const std::wstring& deviceName, const MonitorSettings& s)
{
    m_monitors[deviceName] = s;
}

#ifdef _DEBUG
// =============================================================================
// Self-check
// =============================================================================
// No framework and no fixtures. If the logic breaks, a Debug build stops at
// the assertion instead of misbehaving quietly later.
// =============================================================================
void SettingsStoreSelfCheck()
{
    LOG_INFO("SettingsStore self-check starting");

    // ── 1. ParseHotkey: temel durum ──
    {
        UINT mods = 0, vk = 0;
        BM_SELFCHECK(ParseHotkey(L"Ctrl+Alt+Z", mods, vk));
        BM_SELFCHECK(mods == (MOD_CONTROL | MOD_ALT));
        BM_SELFCHECK(vk == 'Z');
    }

    // ── 2. ParseHotkey: dort modifier birlikte ──
    {
        UINT mods = 0, vk = 0;
        BM_SELFCHECK(ParseHotkey(L"Ctrl+Alt+Shift+Win+K", mods, vk));
        BM_SELFCHECK(mods == (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN));
        BM_SELFCHECK(vk == 'K');
    }

    // ── 3. ParseHotkey: buyuk/kucuk harf onemsiz ──
    {
        UINT mods = 0, vk = 0;
        BM_SELFCHECK(ParseHotkey(L"ctrl+ALT+z", mods, vk));
        BM_SELFCHECK(mods == (MOD_CONTROL | MOD_ALT));
        BM_SELFCHECK(vk == 'Z');
    }

    // ── 4. ParseHotkey: fonksiyon tuslari ──
    {
        UINT mods = 0, vk = 0;
        BM_SELFCHECK(ParseHotkey(L"Ctrl+F12", mods, vk));
        BM_SELFCHECK(mods == MOD_CONTROL);
        BM_SELFCHECK(vk == VK_F12);
    }

    // ParseHotkey must not touch its outputs on bad input. This matters for the
    // panel: typing nonsense should leave the working binding working.
    {
        UINT mods = 0xDEAD, vk = 0xBEEF;
        BM_SELFCHECK(!ParseHotkey(L"", mods, vk));
        BM_SELFCHECK(mods == 0xDEAD && vk == 0xBEEF);

        BM_SELFCHECK(!ParseHotkey(L"Ctrl+", mods, vk));
        BM_SELFCHECK(mods == 0xDEAD && vk == 0xBEEF);

        BM_SELFCHECK(!ParseHotkey(L"Bogus+Z", mods, vk));
        BM_SELFCHECK(mods == 0xDEAD && vk == 0xBEEF);

        BM_SELFCHECK(!ParseHotkey(L"Ctrl+Alt", mods, vk));   // no key, only modifiers
        BM_SELFCHECK(mods == 0xDEAD && vk == 0xBEEF);

        BM_SELFCHECK(!ParseHotkey(L"Ctrl+F99", mods, vk));   // F24'ten buyuk
        BM_SELFCHECK(mods == 0xDEAD && vk == 0xBEEF);
    }

    // ── 6. FormatHotkey: sabit modifier sirasi ──
    {
        BM_SELFCHECK(FormatHotkey(MOD_CONTROL | MOD_ALT, 'Z') == L"Ctrl+Alt+Z");
        BM_SELFCHECK(FormatHotkey(MOD_ALT | MOD_CONTROL, 'Z') == L"Ctrl+Alt+Z");
        BM_SELFCHECK(FormatHotkey(MOD_WIN, 'Z') == L"Win+Z");
        BM_SELFCHECK(FormatHotkey(MOD_CONTROL, VK_F5) == L"Ctrl+F5");
    }

    // Round trip: format then parse returns the same pair.
    {
        const UINT origMods = MOD_CONTROL | MOD_SHIFT;
        const UINT origVk   = 'Q';
        const std::wstring text = FormatHotkey(origMods, origVk);

        UINT mods = 0, vk = 0;
        BM_SELFCHECK(ParseHotkey(text, mods, vk));
        BM_SELFCHECK(mods == origMods);
        BM_SELFCHECK(vk == origVk);
    }

    // ── 8. FilePath: under %APPDATA%, with the right file name ──
    {
        const auto p = SettingsStore::FilePath();
        BM_SELFCHECK(!p.empty());
        BM_SELFCHECK(p.filename() == L"settings.ini");
        BM_SELFCHECK(p.parent_path().filename() == L"BetterMagnifier");
    }

    // ── 9 & 10. Load/Save ──
    //
    // Redirected to a temp file. The suite used to move the real settings.ini
    // aside and put it back, which is fine until an assertion aborts the
    // process mid-run: the real file then stays parked under the backup name,
    // the next run's rename fails because the destination exists, and the suite
    // reads its own leftovers while the user's settings sit in a file nothing
    // will ever open again. That happened. A test that can eat the user's
    // config when it fails is not worth having, so it no longer goes near it.
    {
        std::error_code ec;
        const auto temp = std::filesystem::temp_directory_path(ec)
                        / L"BetterMagnifier-selfcheck" / L"settings.ini";

        std::filesystem::create_directories(temp.parent_path(), ec);
        std::filesystem::remove(temp, ec);      // leftovers from an aborted run
        SetSettingsPathOverride(temp);

        // No file: defaults, and Load still succeeds.
        SettingsStore fresh;
        BM_SELFCHECK(fresh.Load());
        BM_SELFCHECK(fresh.General().toggleVk == 'Z');
        BM_SELFCHECK(fresh.General().toggleModifiers == (MOD_CONTROL | MOD_ALT));
        BM_SELFCHECK(fresh.General().hijackMagnifierKeys == true);   // defaults to on
        BM_SELFCHECK(fresh.General().followMode == FollowMode::EdgePush);  // the current default
        BM_SELFCHECK(fresh.General().rememberZoomLevel == true);

        // Pointer ve edge-push varsayilanlari
        BM_SELFCHECK(fresh.General().pointerScaling == true);
        BM_SELFCHECK(fresh.General().lockPointerToMonitor == true);
        BM_SELFCHECK(std::abs(fresh.General().pointerCompensation - 0.2f) < 1e-4f);
        BM_SELFCHECK(std::abs(fresh.General().pointerSpeed - 1.0f) < 1e-4f);
        BM_SELFCHECK(std::abs(fresh.General().edgeBandFraction - 0.12f) < 1e-4f);

        // An unknown monitor yields the defaults
        const auto m = fresh.Monitor(L"\\\\.\\NOSUCHDISPLAY");
        BM_SELFCHECK(m.minZoom == 1.0f);
        BM_SELFCHECK(m.maxZoom == 10.0f);

        // ── 10. Save -> Load turu degerleri koruyor ──
        SettingsStore w;
        w.MutableGeneral().toggleModifiers  = MOD_CONTROL | MOD_SHIFT;
        w.MutableGeneral().toggleVk         = 'M';
        w.MutableGeneral().hijackMagnifierKeys = false;
        w.MutableGeneral().followMode       = FollowMode::Mouse;
        w.MutableGeneral().rememberZoomLevel = false;
        w.MutableGeneral().edgeBandFraction     = 0.2f;
        w.MutableGeneral().pointerScaling       = false;
        w.MutableGeneral().pointerSpeed         = 1.5f;
        w.MutableGeneral().pointerCompensation  = 0.35f;
        w.MutableGeneral().cursorScale          = 1.25f;
        w.MutableGeneral().lockPointerToMonitor = false;
        w.SetMonitor(L"\\\\.\\DISPLAY1", MonitorSettings{ 1.5f, 8.0f, 0.5f, 3.25f });
        BM_SELFCHECK(w.Save());

        SettingsStore r;
        BM_SELFCHECK(r.Load());
        BM_SELFCHECK(r.General().toggleModifiers == (MOD_CONTROL | MOD_SHIFT));
        BM_SELFCHECK(r.General().toggleVk == 'M');
        BM_SELFCHECK(r.General().hijackMagnifierKeys == false);
        BM_SELFCHECK(r.General().followMode == FollowMode::Mouse);
        BM_SELFCHECK(r.General().rememberZoomLevel == false);
        BM_SELFCHECK(std::abs(r.General().edgeBandFraction - 0.2f) < 1e-4f);
        BM_SELFCHECK(r.General().pointerScaling == false);
        BM_SELFCHECK(std::abs(r.General().pointerSpeed - 1.5f) < 1e-4f);
        BM_SELFCHECK(std::abs(r.General().pointerCompensation - 0.35f) < 1e-4f);
        BM_SELFCHECK(std::abs(r.General().cursorScale - 1.25f) < 1e-4f);
        BM_SELFCHECK(r.General().lockPointerToMonitor == false);

        // Sacma degerler tek tek varsayilana dusuyor, dosyanin geri kalanini
        // zehirlemiyor.
        SettingsStore outOfRange;
        outOfRange.MutableGeneral().edgeBandFraction    = 5.0f;
        outOfRange.MutableGeneral().pointerSpeed        = 99.0f;
        outOfRange.MutableGeneral().pointerCompensation = -3.0f;
        BM_SELFCHECK(outOfRange.Save());

        SettingsStore clamped;
        BM_SELFCHECK(clamped.Load());
        BM_SELFCHECK(clamped.General().edgeBandFraction > 0.0f &&
                     clamped.General().edgeBandFraction <= 0.45f);
        BM_SELFCHECK(clamped.General().pointerSpeed > 0.0f &&
                     clamped.General().pointerSpeed <= 5.0f);
        BM_SELFCHECK(clamped.General().pointerCompensation >= 0.0f &&
                     clamped.General().pointerCompensation <= 1.0f);

        const auto rm = r.Monitor(L"\\\\.\\DISPLAY1");
        BM_SELFCHECK(rm.minZoom  == 1.5f);
        BM_SELFCHECK(rm.maxZoom  == 8.0f);
        BM_SELFCHECK(rm.zoomStep == 0.5f);
        BM_SELFCHECK(rm.lastZoom == 3.25f);

        // ── 11. Mantiksiz degerler varsayilana duser ──
        SettingsStore bad;
        bad.SetMonitor(L"\\\\.\\DISPLAY9", MonitorSettings{ -5.0f, -1.0f, 0.0f, 999.0f });
        BM_SELFCHECK(bad.Save());

        SettingsStore fixed;
        BM_SELFCHECK(fixed.Load());
        const auto fm = fixed.Monitor(L"\\\\.\\DISPLAY9");
        BM_SELFCHECK(fm.minZoom  == 1.0f);    // negative -> default
        BM_SELFCHECK(fm.maxZoom  == 10.0f);   // below the minimum -> default
        BM_SELFCHECK(fm.zoomStep == 0.25f);   // zero -> default
        BM_SELFCHECK(fm.lastZoom >= fm.minZoom && fm.lastZoom <= fm.maxZoom);

        // Cleanup is a courtesy now, not a correctness requirement: an aborted
        // run leaves a stray temp file and nothing else, and the next run
        // removes it before starting.
        std::filesystem::remove(temp, ec);
        SetSettingsPathOverride({});
    }

    LOG_INFO("SettingsStore self-check passed");
}
#endif // _DEBUG

} // namespace BetterMagnifier
