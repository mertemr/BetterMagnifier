#pragma once

// =============================================================================
// SettingsStore.h — INI tabanli ayar kaliciligi
// =============================================================================
// %APPDATA%\BetterMagnifier\settings.ini
//
// Neden INI, JSON degil?
//   Win32'de INI okuma/yazma isletim sisteminde HAZIR:
//   WritePrivateProfileStringW / GetPrivateProfileStringW. Parser yazmak yok,
//   bagimlilik eklemek yok. Ayarlarimiz duz (nested yapi yok), per-monitor
//   ayarlar da dogal olarak section oluyor: [\\.\DISPLAY1]
//
// Python analojisi: configparser.ConfigParser() — ayni dosya formati,
// ayni section/key mantigi. Fark: Python'da dosyayi bir kez acip parse
// ediyorsun, Win32'de her cagri dosyaya tek basina gidiyor (yavas ama biz
// nadir cagiriyoruz — acilista bir kez, ayar degisince bir kez).
//
// THREAD SAHIPLIGI: GUI thread yazar, render thread WM_APP_SETTINGS_CHANGED
// aldiginda okur. Ikisi ayni anda dokunmaz cunku GUI once yazar SONRA mesaj
// postalar. Kendi icinde kilit tutmuyor — bu siraya guveniyor.
// =============================================================================

#ifndef BETTER_MAGNIFIER_SETTINGS_STORE_H
#define BETTER_MAGNIFIER_SETTINGS_STORE_H

#include <windows.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <filesystem>

namespace BetterMagnifier {

// Zoom bolgesi neyi takip ediyor?
enum class FollowMode
{
    Mouse,          // Sadece fare
    MouseAndFocus,  // Fare + klavye odagi (EVENT_OBJECT_FOCUS)
};

// ─────────────────────────────────────────────────────────────────────────────
// Genel ayarlar — monitorden bagimsiz
// ─────────────────────────────────────────────────────────────────────────────
struct GeneralSettings
{
    // Hotkey'ler MOD_* bayraklari + virtual key kodu olarak tutulur.
    // RegisterHotKey tam olarak bu ikiliyi istiyor.
    UINT       toggleModifiers   = MOD_CONTROL | MOD_ALT;
    UINT       toggleVk          = 'Z';
    UINT       freezeModifiers   = MOD_CONTROL | MOD_ALT;
    UINT       freezeVk          = 'X';

    // Win+Z'yi WH_KEYBOARD_LL ile ele gecir. VARSAYILAN KAPALI:
    // acildiginda Windows 11 Snap Layouts calismaz hale gelir.
    bool       hijackWinZ        = false;

    FollowMode followMode        = FollowMode::MouseAndFocus;
    bool       startWithWindows  = false;
    bool       rememberZoomLevel = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// Monitor basina ayarlar — device name ile anahtarlanir ("\\\\.\\DISPLAY1")
// ─────────────────────────────────────────────────────────────────────────────
struct MonitorSettings
{
    float minZoom  = 1.0f;
    float maxZoom  = 10.0f;
    float zoomStep = 0.25f;
    float lastZoom = 2.0f;   // rememberZoomLevel aciksa zoom acilirken kullanilir
};

// ─────────────────────────────────────────────────────────────────────────────
// Hotkey metin donusumu (saf mantik — test edilebilir tek parca)
// ─────────────────────────────────────────────────────────────────────────────
// "Ctrl+Alt+Z" <-> (MOD_CONTROL|MOD_ALT, 'Z')
//
// Desteklenen modifier isimleri: Ctrl, Alt, Shift, Win (buyuk/kucuk harf onemsiz)
// Desteklenen tuslar: A-Z, 0-9, F1-F24
//
// Basarisizlikta false doner ve modifiers/vk'ya DOKUNMAZ — cagiran taraf
// varsayilanini koruyabilir. Bu davranis GUI icin onemli: kullanici gecersiz
// metin yazdiginda mevcut hotkey calismaya devam etmeli.
bool ParseHotkey(std::wstring_view text, UINT& modifiers, UINT& vk);

// Ters yon. Modifier sirasi HER ZAMAN Ctrl, Alt, Shift, Win — boylece
// yaz-oku turu ayni metni uretir (round-trip kararli).
std::wstring FormatHotkey(UINT modifiers, UINT vk);

// ─────────────────────────────────────────────────────────────────────────────
// SettingsStore
// ─────────────────────────────────────────────────────────────────────────────
class SettingsStore
{
public:
    SettingsStore() = default;

    // Dosyayi oku. Dosya YOKSA varsayilanlarla doner ve true verir —
    // ilk calistirma hata degil.
    // Bozuk deger (parse edilemeyen hotkey, mantiksiz zoom) varsayilana duser.
    bool Load();

    // Dosyaya yaz. Klasor yoksa olusturur.
    bool Save() const;

    const GeneralSettings& General() const { return m_general; }
    GeneralSettings&       MutableGeneral() { return m_general; }

    // Bilinmeyen monitor icin varsayilan MonitorSettings doner.
    MonitorSettings Monitor(const std::wstring& deviceName) const;
    void SetMonitor(const std::wstring& deviceName, const MonitorSettings& s);

    // %APPDATA%\BetterMagnifier\settings.ini
    static std::filesystem::path FilePath();

private:
    GeneralSettings m_general;
    std::unordered_map<std::wstring, MonitorSettings> m_monitors;
};

#ifdef _DEBUG
// Assert tabanli self-check. main.cpp Debug build'de cagirir.
// Basarisiz assert uygulamayi dusurur — sessiz regresyon olmaz.
// Bu projede test framework'u yok; saf mantigi olan tek bilesen bu.
void SettingsStoreSelfCheck();
#endif

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_SETTINGS_STORE_H
