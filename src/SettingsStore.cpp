// =============================================================================
// SettingsStore.cpp — INI kalicilik implementasyonu
// =============================================================================

#include "pch.h"
#include "SettingsStore.h"
#include "Logger.h"

#include <shlobj.h>       // SHGetKnownFolderPath
#include <cassert>
#include <cwctype>        // towlower, towupper
#include <cwchar>         // std::wcstof

namespace BetterMagnifier {

// =============================================================================
// Yardimcilar (dosya disina cikmayan saf fonksiyonlar)
// =============================================================================
namespace {

// "ctrl" -> MOD_CONTROL. Taninmazsa 0 doner.
UINT ModifierFromName(std::wstring_view name)
{
    // _wcsicmp buyuk/kucuk harf duyarsiz karsilastirma yapar ama
    // string_view null-terminated olmadigi icin elle karsilastiriyoruz.
    auto equalsCI = [name](std::wstring_view other) {
        if (name.size() != other.size())
            return false;
        for (size_t i = 0; i < name.size(); ++i)
        {
            if (towlower(name[i]) != towlower(other[i]))
                return false;
        }
        return true;
    };

    if (equalsCI(L"ctrl"))  return MOD_CONTROL;
    if (equalsCI(L"alt"))   return MOD_ALT;
    if (equalsCI(L"shift")) return MOD_SHIFT;
    if (equalsCI(L"win"))   return MOD_WIN;
    return 0;
}

// "Z" -> 'Z', "F12" -> VK_F12. Taninmazsa 0 doner.
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

// INI'de float tipi yok — metin olarak yazip std::wcstof ile okuyoruz.
// Neden std::to_wstring degil? O locale'e bagli, virgul/nokta karisir.
// std::format sabit "." kullanir, INI'de tasinabilirlik icin sart.
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
// Hotkey metin donusumu
// =============================================================================
bool ParseHotkey(std::wstring_view text, UINT& modifiers, UINT& vk)
{
    if (text.empty())
        return false;

    // '+' ile parcala. Son parca tus, oncekiler modifier.
    // Python analojisi: parts = text.split("+")
    UINT parsedMods = 0;
    size_t start = 0;

    while (true)
    {
        const size_t plus = text.find(L'+', start);
        const std::wstring_view piece = (plus == std::wstring_view::npos)
            ? text.substr(start)
            : text.substr(start, plus - start);

        if (piece.empty())
            return false;   // "Ctrl+" veya "Ctrl++Z" gibi bozuk girdi

        if (plus == std::wstring_view::npos)
        {
            // Son parca — tus olmali
            const UINT parsedVk = VirtualKeyFromName(piece);
            if (parsedVk == 0)
                return false;

            // Basari: SIMDI cikti parametrelerine yaz. Erken yazmiyoruz
            // ki basarisizlikta cagiranin degerleri bozulmasin.
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

std::wstring FormatHotkey(UINT modifiers, UINT vk)
{
    // Sabit sira: Ctrl, Alt, Shift, Win. Round-trip kararliligi icin sart —
    // ayni (modifiers, vk) her zaman ayni metni uretmeli.
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
// SHGetKnownFolderPath modern API (Vista+). Eski SHGetFolderPath deprecated.
// CoTaskMemFree ile serbest birakmak ZORUNLU — yoksa leak.
// Python analojisi: os.path.join(os.getenv("APPDATA"), "BetterMagnifier", ...)
// =============================================================================
std::filesystem::path SettingsStore::FilePath()
{
    PWSTR appData = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData);

    if (FAILED(hr) || !appData)
    {
        if (appData) CoTaskMemFree(appData);
        LOG_ERROR("SHGetKnownFolderPath basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
        return {};
    }

    std::filesystem::path p = std::filesystem::path(appData) / L"BetterMagnifier" / L"settings.ini";
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
        LOG_INFO("Ayar dosyasi yok, varsayilanlar kullanilacak");
        return true;   // Ilk calistirma — hata degil
    }

    const std::wstring file = path.wstring();

    // ── Hotkey'ler ──
    // Bozuk deger varsayilana duser: ParseHotkey basarisizsa cikti
    // parametrelerine dokunmuyor, m_general zaten varsayilanda.
    {
        wchar_t buf[64]{};
        GetPrivateProfileStringW(L"General", L"ToggleHotkey", L"", buf, 64, file.c_str());
        if (buf[0] != L'\0')
        {
            if (!ParseHotkey(buf, m_general.toggleModifiers, m_general.toggleVk))
                LOG_WARN("ToggleHotkey bozuk, varsayilan kullanilacak");
        }

        buf[0] = L'\0';
        GetPrivateProfileStringW(L"General", L"FreezeHotkey", L"", buf, 64, file.c_str());
        if (buf[0] != L'\0')
        {
            if (!ParseHotkey(buf, m_general.freezeModifiers, m_general.freezeVk))
                LOG_WARN("FreezeHotkey bozuk, varsayilan kullanilacak");
        }
    }

    // ── Bayraklar ──
    // GetPrivateProfileIntW varsayilan degeri parametre olarak aliyor,
    // yani eksik anahtar otomatik varsayilana duser.
    m_general.hijackWinZ =
        GetPrivateProfileIntW(L"General", L"HijackWinZ", 0, file.c_str()) != 0;
    m_general.startWithWindows =
        GetPrivateProfileIntW(L"General", L"StartWithWindows", 0, file.c_str()) != 0;
    m_general.rememberZoomLevel =
        GetPrivateProfileIntW(L"General", L"RememberZoomLevel", 1, file.c_str()) != 0;

    // ── Takip modu ──
    {
        wchar_t buf[32]{};
        GetPrivateProfileStringW(L"General", L"FollowMode", L"MouseAndFocus", buf, 32, file.c_str());
        m_general.followMode = (_wcsicmp(buf, L"Mouse") == 0)
            ? FollowMode::Mouse
            : FollowMode::MouseAndFocus;   // taninmayan deger -> varsayilan
    }

    // ── Per-monitor section'lari ──
    // GetPrivateProfileSectionNamesW tum section isimlerini '\0' ile ayrilmis
    // tek bir buffer'da veriyor, sonu cift '\0'.
    // Python analojisi: config.sections()
    {
        std::vector<wchar_t> names(8192);
        const DWORD len = GetPrivateProfileSectionNamesW(
            names.data(), static_cast<DWORD>(names.size()), file.c_str());

        const wchar_t* p = names.data();
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

            // Mantiksiz degerler varsayilana duser
            if (ms.minZoom  <= 0.0f)          ms.minZoom  = 1.0f;
            if (ms.maxZoom  <= ms.minZoom)    ms.maxZoom  = 10.0f;
            if (ms.zoomStep <= 0.0f)          ms.zoomStep = 0.25f;
            if (ms.lastZoom < ms.minZoom || ms.lastZoom > ms.maxZoom)
                ms.lastZoom = ms.minZoom;

            m_monitors[section] = ms;
        }
    }

    LOG_INFO("Ayarlar yuklendi: {} monitor kaydi", m_monitors.size());
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

    // Klasor yoksa olustur — WritePrivateProfileStringW klasor olusturmuyor.
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
    ok = (WritePrivateProfileStringW(L"General", L"ToggleHotkey", toggle.c_str(), file.c_str()) != 0) && ok;
    ok = (WritePrivateProfileStringW(L"General", L"FreezeHotkey", freeze.c_str(), file.c_str()) != 0) && ok;
    ok = WriteInt(file, L"General", L"HijackWinZ",        m_general.hijackWinZ ? 1 : 0) && ok;
    ok = WriteInt(file, L"General", L"StartWithWindows",  m_general.startWithWindows ? 1 : 0) && ok;
    ok = WriteInt(file, L"General", L"RememberZoomLevel", m_general.rememberZoomLevel ? 1 : 0) && ok;
    ok = (WritePrivateProfileStringW(L"General", L"FollowMode",
            (m_general.followMode == FollowMode::Mouse) ? L"Mouse" : L"MouseAndFocus",
            file.c_str()) != 0) && ok;

    for (const auto& [device, ms] : m_monitors)
    {
        ok = WriteFloat(file, device, L"MinZoom",  ms.minZoom)  && ok;
        ok = WriteFloat(file, device, L"MaxZoom",  ms.maxZoom)  && ok;
        ok = WriteFloat(file, device, L"ZoomStep", ms.zoomStep) && ok;
        ok = WriteFloat(file, device, L"LastZoom", ms.lastZoom) && ok;
    }

    if (!ok)
        LOG_ERROR("Ayarlar kaydedilirken en az bir yazma basarisiz oldu");

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
// Self-Check — assert tabanli
// =============================================================================
// Framework yok, fixture yok. Mantik bozulursa uygulama Debug'da aninda duser.
// Python analojisi: if __name__ == "__main__" icindeki assert'ler.
// =============================================================================
void SettingsStoreSelfCheck()
{
    LOG_INFO("SettingsStore self-check basliyor...");

    // ── 1. ParseHotkey: temel durum ──
    {
        UINT mods = 0, vk = 0;
        assert(ParseHotkey(L"Ctrl+Alt+Z", mods, vk));
        assert(mods == (MOD_CONTROL | MOD_ALT));
        assert(vk == 'Z');
    }

    // ── 2. ParseHotkey: dort modifier birlikte ──
    {
        UINT mods = 0, vk = 0;
        assert(ParseHotkey(L"Ctrl+Alt+Shift+Win+K", mods, vk));
        assert(mods == (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN));
        assert(vk == 'K');
    }

    // ── 3. ParseHotkey: buyuk/kucuk harf onemsiz ──
    {
        UINT mods = 0, vk = 0;
        assert(ParseHotkey(L"ctrl+ALT+z", mods, vk));
        assert(mods == (MOD_CONTROL | MOD_ALT));
        assert(vk == 'Z');
    }

    // ── 4. ParseHotkey: fonksiyon tuslari ──
    {
        UINT mods = 0, vk = 0;
        assert(ParseHotkey(L"Ctrl+F12", mods, vk));
        assert(mods == MOD_CONTROL);
        assert(vk == VK_F12);
    }

    // ── 5. ParseHotkey: bozuk girdi ciktiya DOKUNMAZ ──
    {
        UINT mods = 0xDEAD, vk = 0xBEEF;
        assert(!ParseHotkey(L"", mods, vk));
        assert(mods == 0xDEAD && vk == 0xBEEF);

        assert(!ParseHotkey(L"Ctrl+", mods, vk));
        assert(mods == 0xDEAD && vk == 0xBEEF);

        assert(!ParseHotkey(L"Bogus+Z", mods, vk));
        assert(mods == 0xDEAD && vk == 0xBEEF);

        assert(!ParseHotkey(L"Ctrl+Alt", mods, vk));   // son parca tus degil
        assert(mods == 0xDEAD && vk == 0xBEEF);
    }

    // ── 6. FormatHotkey: sabit modifier sirasi ──
    {
        assert(FormatHotkey(MOD_CONTROL | MOD_ALT, 'Z') == L"Ctrl+Alt+Z");
        assert(FormatHotkey(MOD_ALT | MOD_CONTROL, 'Z') == L"Ctrl+Alt+Z");  // sira onemsiz girdide
        assert(FormatHotkey(MOD_WIN, 'Z') == L"Win+Z");
        assert(FormatHotkey(MOD_CONTROL, VK_F5) == L"Ctrl+F5");
    }

    // ── 7. Round-trip: format -> parse ayni degeri verir ──
    {
        const UINT origMods = MOD_CONTROL | MOD_SHIFT;
        const UINT origVk   = 'Q';
        const std::wstring text = FormatHotkey(origMods, origVk);

        UINT mods = 0, vk = 0;
        assert(ParseHotkey(text, mods, vk));
        assert(mods == origMods);
        assert(vk == origVk);
    }

    // ── 8. FilePath: %APPDATA% altinda, dogru dosya adi ──
    {
        const auto p = SettingsStore::FilePath();
        assert(!p.empty());
        assert(p.filename() == L"settings.ini");
        assert(p.parent_path().filename() == L"BetterMagnifier");
    }

    // ── 9. Load: dosya yoksa varsayilanlar, true doner ──
    // Gercek dosyayi bozmamak icin gecici bir yola tasiyip geri koyuyoruz.
    {
        const auto real = SettingsStore::FilePath();
        const auto backup = real.parent_path() / L"settings.ini.selfcheck-backup";

        std::error_code ec;
        const bool hadFile = std::filesystem::exists(real, ec);
        if (hadFile)
            std::filesystem::rename(real, backup, ec);

        SettingsStore fresh;
        assert(fresh.Load());
        assert(fresh.General().toggleVk == 'Z');
        assert(fresh.General().toggleModifiers == (MOD_CONTROL | MOD_ALT));
        assert(fresh.General().hijackWinZ == false);
        assert(fresh.General().followMode == FollowMode::MouseAndFocus);

        // Bilinmeyen monitor -> varsayilan
        const auto m = fresh.Monitor(L"\\\\.\\NOSUCHDISPLAY");
        assert(m.minZoom == 1.0f);
        assert(m.maxZoom == 10.0f);

        // ── 10. Save -> Load turu degerleri koruyor ──
        SettingsStore w;
        w.MutableGeneral().toggleModifiers = MOD_CONTROL | MOD_SHIFT;
        w.MutableGeneral().toggleVk        = 'M';
        w.MutableGeneral().hijackWinZ      = true;
        w.MutableGeneral().followMode      = FollowMode::Mouse;
        w.SetMonitor(L"\\\\.\\DISPLAY1", MonitorSettings{1.5f, 8.0f, 0.5f, 3.25f});
        assert(w.Save());

        SettingsStore r;
        assert(r.Load());
        assert(r.General().toggleModifiers == (MOD_CONTROL | MOD_SHIFT));
        assert(r.General().toggleVk == 'M');
        assert(r.General().hijackWinZ == true);
        assert(r.General().followMode == FollowMode::Mouse);

        const auto rm = r.Monitor(L"\\\\.\\DISPLAY1");
        assert(rm.minZoom  == 1.5f);
        assert(rm.maxZoom  == 8.0f);
        assert(rm.zoomStep == 0.5f);
        assert(rm.lastZoom == 3.25f);

        // Temizlik: self-check dosyasini sil, gercegi geri koy
        std::filesystem::remove(real, ec);
        if (hadFile)
            std::filesystem::rename(backup, real, ec);
    }

    LOG_INFO("SettingsStore self-check GECTI");
}
#endif // _DEBUG

} // namespace BetterMagnifier
