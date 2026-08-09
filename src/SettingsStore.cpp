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
// Buyuk/kucuk harf duyarsiz string_view karsilastirmasi
// ─────────────────────────────────────────────────────────────────────────────
// _wcsicmp null-terminated bekliyor, string_view oyle olmak zorunda degil —
// elle karsilastiriyoruz.
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
// ─────────────────────────────────────────────────────────────────────────────
// INI'de float tipi yok — metin uzerinden gidiyoruz.
// Neden std::to_wstring degil? O locale'e bagli, ondalik ayraci virgul
// olabilir (Turkce locale!) ve sonra wcstof onu okuyamaz. std::format
// sabit "." kullanir — dosya tasinabilir kalir.
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
// Python analojisi: parts = text.split("+"); *mods, key = parts
//
// ONEMLI: cikti parametrelerine SADECE tam basaridan sonra yaziyoruz.
// Yariyolda yazsak, gecersiz girdide cagiranin mevcut hotkey'i bozulurdu.
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
            return false;   // "Ctrl+" veya "Ctrl++Z" gibi bozuk girdi

        if (plus == std::wstring_view::npos)
        {
            // Son parca — tus olmali
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
// Sabit sira: Ctrl, Alt, Shift, Win. Round-trip kararliligi icin sart —
// ayni (modifiers, vk) her zaman ayni metni uretmeli, yoksa dosyaya yazip
// geri okumak degeri kaydirir.
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
// CoTaskMemFree ile serbest birakmak ZORUNLU — yoksa leak.
//
// Python analojisi: os.path.join(os.getenv("APPDATA"), "BetterMagnifier", ...)
// =============================================================================
std::filesystem::path SettingsStore::FilePath()
{
    PWSTR appData = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData);

    if (FAILED(hr) || !appData)
    {
        if (appData)
            CoTaskMemFree(appData);

        LOG_ERROR("SHGetKnownFolderPath basarisiz: 0x{:08X}", static_cast<unsigned long>(hr));
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
        LOG_INFO("Ayar dosyasi yok, varsayilanlar kullanilacak");
        return true;   // Ilk calistirma — hata degil
    }

    const std::wstring file = path.wstring();

    // ── Hotkey'ler ──
    // ParseHotkey basarisizsa cikti parametrelerine dokunmuyor, m_general
    // zaten varsayilanda — yani bozuk deger sessizce varsayilana duser.
    {
        wchar_t buf[64]{};
        GetPrivateProfileStringW(L"General", L"ToggleHotkey", L"", buf, 64, file.c_str());
        if (buf[0] != L'\0' &&
            !ParseHotkey(buf, m_general.toggleModifiers, m_general.toggleVk))
        {
            LOG_WARN("ToggleHotkey bozuk, varsayilan kullanilacak");
        }

        buf[0] = L'\0';
        GetPrivateProfileStringW(L"General", L"FreezeHotkey", L"", buf, 64, file.c_str());
        if (buf[0] != L'\0' &&
            !ParseHotkey(buf, m_general.freezeModifiers, m_general.freezeVk))
        {
            LOG_WARN("FreezeHotkey bozuk, varsayilan kullanilacak");
        }
    }

    // ── Bayraklar ──
    // GetPrivateProfileIntW varsayilan degeri parametre olarak aliyor —
    // eksik anahtar otomatik varsayilana duser.
    // Varsayilan 1 (ACIK) — kullanici Windows'un magnifier'i yerine bunu istiyor.
    m_general.hijackMagnifierKeys =
        GetPrivateProfileIntW(L"General", L"HijackMagnifierKeys", 1, file.c_str()) != 0;
    m_general.startWithWindows =
        GetPrivateProfileIntW(L"General", L"StartWithWindows", 0, file.c_str()) != 0;
    m_general.rememberZoomLevel =
        GetPrivateProfileIntW(L"General", L"RememberZoomLevel", 1, file.c_str()) != 0;

    // ── Takip modu ──
    {
        wchar_t buf[32]{};
        GetPrivateProfileStringW(L"General", L"FollowMode", L"Mouse",
                                 buf, 32, file.c_str());
        // Taninmayan deger -> varsayilan (MouseAndFocus)
        m_general.followMode = EqualsCI(buf, L"Mouse")
            ? FollowMode::Mouse
            : FollowMode::MouseAndFocus;
    }

    // ── Per-monitor section'lari ──
    // GetPrivateProfileSectionNamesW tum section isimlerini '\0' ile ayrilmis
    // tek bir buffer'da veriyor, sonu cift '\0'.
    // Python analojisi: config.sections()
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

            // Mantiksiz degerler varsayilana duser. Elle duzenlenebilir bir
            // dosyada bu sart — kullanici MaxZoom=0 yazarsa uygulama
            // kullanilamaz hale gelmemeli.
            if (ms.minZoom  <= 0.0f)       ms.minZoom  = 1.0f;
            if (ms.maxZoom  <= ms.minZoom) ms.maxZoom  = 10.0f;
            if (ms.zoomStep <= 0.0f)       ms.zoomStep = 0.25f;
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

    // WritePrivateProfileStringW klasor OLUSTURMUYOR — elle yapmak lazim.
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
    ok = WritePrivateProfileStringW(L"General", L"FollowMode",
            (m_general.followMode == FollowMode::Mouse) ? L"Mouse" : L"MouseAndFocus",
            file.c_str()) != 0 && ok;

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

    // ── 5. ParseHotkey: bozuk girdi ciktiya DOKUNMAZ ──
    // GUI icin kritik: kullanici sacma bir sey yazdiginda mevcut hotkey
    // calismaya devam etmeli.
    {
        UINT mods = 0xDEAD, vk = 0xBEEF;
        BM_SELFCHECK(!ParseHotkey(L"", mods, vk));
        BM_SELFCHECK(mods == 0xDEAD && vk == 0xBEEF);

        BM_SELFCHECK(!ParseHotkey(L"Ctrl+", mods, vk));
        BM_SELFCHECK(mods == 0xDEAD && vk == 0xBEEF);

        BM_SELFCHECK(!ParseHotkey(L"Bogus+Z", mods, vk));
        BM_SELFCHECK(mods == 0xDEAD && vk == 0xBEEF);

        BM_SELFCHECK(!ParseHotkey(L"Ctrl+Alt", mods, vk));   // son parca tus degil
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

    // ── 7. Round-trip: format -> parse ayni degeri verir ──
    {
        const UINT origMods = MOD_CONTROL | MOD_SHIFT;
        const UINT origVk   = 'Q';
        const std::wstring text = FormatHotkey(origMods, origVk);

        UINT mods = 0, vk = 0;
        BM_SELFCHECK(ParseHotkey(text, mods, vk));
        BM_SELFCHECK(mods == origMods);
        BM_SELFCHECK(vk == origVk);
    }

    // ── 8. FilePath: %APPDATA% altinda, dogru dosya adi ──
    {
        const auto p = SettingsStore::FilePath();
        BM_SELFCHECK(!p.empty());
        BM_SELFCHECK(p.filename() == L"settings.ini");
        BM_SELFCHECK(p.parent_path().filename() == L"BetterMagnifier");
    }

    // ── 9 & 10. Load/Save ──
    // Gercek ayar dosyasini bozmamak icin gecici bir yola tasiyip geri koyuyoruz.
    {
        const auto real = SettingsStore::FilePath();
        const auto backup = real.parent_path() / L"settings.ini.selfcheck-backup";

        std::error_code ec;
        std::filesystem::create_directories(real.parent_path(), ec);

        const bool hadFile = std::filesystem::exists(real, ec);
        if (hadFile)
            std::filesystem::rename(real, backup, ec);

        // ── 9. Dosya yoksa varsayilanlar, true doner ──
        SettingsStore fresh;
        BM_SELFCHECK(fresh.Load());
        BM_SELFCHECK(fresh.General().toggleVk == 'Z');
        BM_SELFCHECK(fresh.General().toggleModifiers == (MOD_CONTROL | MOD_ALT));
        BM_SELFCHECK(fresh.General().hijackMagnifierKeys == true);   // varsayilan ACIK
        BM_SELFCHECK(fresh.General().followMode == FollowMode::Mouse);   // varsayilan
        BM_SELFCHECK(fresh.General().rememberZoomLevel == true);

        // Bilinmeyen monitor -> varsayilan
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
        w.SetMonitor(L"\\\\.\\DISPLAY1", MonitorSettings{ 1.5f, 8.0f, 0.5f, 3.25f });
        BM_SELFCHECK(w.Save());

        SettingsStore r;
        BM_SELFCHECK(r.Load());
        BM_SELFCHECK(r.General().toggleModifiers == (MOD_CONTROL | MOD_SHIFT));
        BM_SELFCHECK(r.General().toggleVk == 'M');
        BM_SELFCHECK(r.General().hijackMagnifierKeys == false);
        BM_SELFCHECK(r.General().followMode == FollowMode::Mouse);
        BM_SELFCHECK(r.General().rememberZoomLevel == false);

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
        BM_SELFCHECK(fm.minZoom  == 1.0f);    // negatif -> varsayilan
        BM_SELFCHECK(fm.maxZoom  == 10.0f);   // min'den kucuk -> varsayilan
        BM_SELFCHECK(fm.zoomStep == 0.25f);   // sifir -> varsayilan
        BM_SELFCHECK(fm.lastZoom >= fm.minZoom && fm.lastZoom <= fm.maxZoom);

        // Temizlik: self-check dosyasini sil, gercegi geri koy
        std::filesystem::remove(real, ec);
        if (hadFile)
            std::filesystem::rename(backup, real, ec);
    }

    LOG_INFO("SettingsStore self-check GECTI");
}
#endif // _DEBUG

} // namespace BetterMagnifier
