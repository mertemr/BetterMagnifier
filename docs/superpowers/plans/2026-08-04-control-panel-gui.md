# BetterMagnifier Kontrol Paneli Implementation Plan

> ## DURUM — 2026-08-05 (plan tamamlandı)
>
> | Task | Durum |
> |---|---|
> | 1 — XAML island spike | **BAŞARILI** → [`spike/xaml-island-thread/FINDINGS.md`](../../../spike/xaml-island-thread/FINDINGS.md) |
> | 2 — AppMessages + StatusSnapshot | Tamam, doğrulandı |
> | 3 — SettingsStore | Tamam, 11 assert geçti |
> | 4 — InputThread | Tamam, thread ayrımı log'dan doğrulandı |
> | 5 — ControlPanel iskeleti | Tamam, `Control panel opened` log'da |
> | 6 — Durum sekmesi | Tamam, elle test bekliyor |
> | 7 — Ayarlar sekmesi | Tamam, elle test bekliyor |
> | 8 — Odak takibi | **Bu plan dışında yapıldı** — `FollowMode::MouseAndFocus` |
> | 9 — `Win+Z` ele geçirme | **Kapsam değişti** — `hijackMagnifierKeys` (Win+artı/eksi, Ctrl+Alt+tekerlek, Win+orta tık) |
>
> **Task 5-7'de plandan bilinçli üç sapma var**, üçü de spike düzeltildikten
> sonra öğrenildi ve gerekli:
>
> 1. `Application::Start` ÇAĞRILIYOR ve callback'i bir `Application` örneği
>    oluşturuyor. Doğrudan `Application app{}` kurmak `RPC_E_WRONG_THREAD`
>    veriyor. Bunun sonucu: mesaj loop'u Start'a ait, GUI thread'e iş
>    `PostThreadMessage` ile değil `DispatcherQueue.TryEnqueue` ile geçiyor.
> 2. `XamlControlsResources` ATANMIYOR — atamak tema sözlüğünü siliyor.
> 3. `MddBootstrapInitialize` ÇAĞRILMIYOR — `WindowsPackageType=None`
>    auto-initializer'ı hallediyor.
>
> Ayrıca planın Step 2'sindeki `ItemDefinitionGroup` + `%(Filename)` koşulu
> MSBuild'de yasak (MSB4190); yerine `BeforeTargets="ClCompile"` bir target var.
> `WasdkLibDir` de `Microsoft.Cpp.props`'tan SONRA tanımlanmak zorunda, yoksa
> `NuGetPackageRoot` henüz boş.
>
> Güncel durum ve kalan işler: [`docs/STATUS.md`](../../STATUS.md).
>
> **Pinlenen sürüm: Windows App SDK `1.8.250916003`** (2.3.1 değil).
>
> ### Ortam notu — toolset
>
> Bu makinede VS2022 yok, **VS 18.8 Community** var ve kurulu tek toolset `v145`.
> Projeler artık `v143` pinlemiyor, `$(DefaultPlatformToolset)` kullanıyor —
> her iki VS'te de derleniyor. Plandaki MSBuild yolları VS2022'yi gösteriyor;
> bu makinede karşılığı:
>
> ```bash
> "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.vcxproj /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
> ```

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** BetterMagnifier'a per-monitor zoom durumunu canlı gösteren ve tüm ayarları yönetebilen küçük bir WinUI 3 kontrol paneli eklemek.

**Architecture:** Üç thread. Render thread (main, MTA) D3D11/DXGI/overlay sahibi ve mevcut mesaj penceresini işletir. Input thread low-level klavye/fare hook'larını ve `EVENT_OBJECT_FOCUS`'u sahiplenir, olayları mesaj penceresine `PostMessage` eder. GUI thread STA'da yaşar, `DesktopWindowXamlSource` ile XAML island barındırır. GUI→motor iletişimi tamamen `PostMessage`, motor→GUI iletişimi lock-free atomic snapshot (10 Hz okunur). Render hot path'inde hiç kilit yok.

**Tech Stack:** C++20, Win32, D3D11/DXGI, Windows App SDK (WinUI 3 XAML Islands), C++/WinRT, Win32 INI profile API, VS2022 v143, x64.

**Spec:** `docs/superpowers/specs/2026-08-04-control-panel-gui-design.md`

## Global Constraints

Her task'ın gereksinimleri bu bölümü kapsar.

- **Windows App SDK minimum sürüm 1.5.** Islands 1.4'te deneysellikten çıktı ama `Application.DispatcherShutdownMode` davranışına bel bağlıyoruz (son XAML penceresi kapanınca thread event loop'u otomatik çıkmıyor) — o 1.5'te geldi. Hedef: uygulama anındaki en güncel sürüm, Task 1'de pinlenir.
- **`src/main.cpp` MTA kalır.** `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` değiştirilmeyecek.
- **Magnifier çekirdeği Windows App Runtime olmadan tam çalışır.** Bootstrapper init paneli ilk açışta tembel çalışır; başarısız olursa log + MessageBox, tray toggle ve hotkey'ler çalışmaya devam eder.
- **Yeni harici bağımlılık sadece Windows App SDK.** Ayarlar Win32 INI API ile, JSON parser veya başka kütüphane eklenmeyecek.
- **`WarningLevel=Level4` + `TreatWarningAsError=true`.** Tüm yeni kod uyarısız derlenmeli. Vendor/generated dosyalar için dosya bazında `<TreatWarningAsError>false</TreatWarningAsError>`.
- **Kod yorumları Türkçe, Win32/COM/XAML konseptlerinin yanında Python analojisi.** Bu repo konvansiyonu; kod aynı zamanda öğrenme notu.
- **Commit formatı:** `type(scope): description`, imperative mood, subject 50 karakter altı, gövdede `*` bullet. `Co-Authored-By` satırı EKLENMEYECEK.
- **Yeni her `.cpp`/`.h` hem `BetterMagnifier.vcxproj` hem `BetterMagnifier.vcxproj.filters` içine eklenir.** Aksi halde derlenmez — bu projede daha önce tam olarak bu hata yapıldı, 6 dosya aylarca derlenmedi.

## Doğrulama Döngüsü

Bu projede birim test framework'ü yok. Her task şu döngüyle doğrulanır:

**Derleme kapısı** (`TreatWarningAsError` açık olduğu için gerçek bir kapı):

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Task 5'ten sonra (NuGet devreye girince) aynı komut `/restore` ile:

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /restore /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

**Çalıştır ve log oku** (uygulama artık kendi kapanmıyor, WM_QUIT ile kapatılır):

```bash
powershell -Command "Add-Type -Namespace W -Name N -MemberDefinition '[DllImport(\"user32.dll\", ExactSpelling=true, EntryPoint=\"PostThreadMessageW\")] public static extern bool PostThreadMessageW(uint tid, uint msg, IntPtr wp, IntPtr lp);'; Remove-Item .\bin\Debug-x64\logs\*.log -Force -ErrorAction SilentlyContinue; $p = Start-Process -FilePath '.\bin\Debug-x64\BetterMagnifier.exe' -PassThru; Start-Sleep -Seconds 4; $p.Refresh(); [void][W.N]::PostThreadMessageW([uint32]$p.Threads[0].Id, 0x0012, [IntPtr]::Zero, [IntPtr]::Zero); Start-Sleep -Seconds 3; if ($p.HasExited) { \"EXIT: $($p.ExitCode)\" } else { 'HALA AYAKTA'; Stop-Process -Id $p.Id -Force }; Get-ChildItem .\bin\Debug-x64\logs\*.log | Sort-Object LastWriteTime | Select-Object -Last 1 | Get-Content"
```

**Assert self-check** (sadece `SettingsStore`, Task 3): Debug build'de `main.cpp` içinden çağrılır, başarısız assert uygulamayı düşürür.

---

### Task 1: Spike — ikincil STA thread'de XAML island

Spec bölüm 9'daki açık riski kapatır. Başarısız olursa tasarım değişir, bu yüzden ilk iş.

**Files:**
- Create: `spike/xaml-island-thread/spike.cpp`
- Create: `spike/xaml-island-thread/spike.vcxproj`
- Modify: `BetterMagnifier.sln` (spike projesini ekle)

**Interfaces:**
- Consumes: hiçbir şey (bağımsız spike)
- Produces: doğrulanmış bilgi — `DesktopWindowXamlSource` ikincil STA thread'de ana thread MTA kalırken çalışıyor mu; ve pinlenmiş Windows App SDK sürüm numarası

- [ ] **Step 1: Mevcut en güncel Windows App SDK sürümünü bul**

```bash
nuget list Microsoft.WindowsAppSDK -Source https://api.nuget.org/v3/index.json
```

`nuget.exe` yoksa alternatif:

```bash
curl -s "https://api.nuget.org/v3-flatcontainer/microsoft.windowsappsdk/index.json"
```

Dönen listeden 1.5'ten büyük, pre-release olmayan en yüksek sürümü seç. Bu sürüm numarasını not et — plandaki `PINNED_SDK_VERSION` yerine bunu yazacaksın.

- [ ] **Step 2: Spike projesini oluştur**

`spike/xaml-island-thread/spike.vcxproj` — konsol uygulaması, C++20, x64, C++/WinRT açık:

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion>
    <ProjectGuid>{a1b2c3d4-0000-4000-8000-000000000001}</ProjectGuid>
    <RootNamespace>spike</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <UseDebugLibraries>true</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ItemDefinitionGroup>
    <ClCompile>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <PreprocessorDefinitions>_DEBUG;UNICODE;_UNICODE;WIN32_LEAN_AND_MEAN;NOMINMAX;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
    <Link>
      <SubSystem>Console</SubSystem>
    </Link>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="spike.cpp" />
  </ItemGroup>
  <ItemGroup>
    <PackageReference Include="Microsoft.WindowsAppSDK" Version="PINNED_SDK_VERSION" />
    <PackageReference Include="Microsoft.Windows.CppWinRT" Version="2.0.240111.5" />
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
```

`PINNED_SDK_VERSION` yerine Step 1'de bulduğun sürümü yaz. `Microsoft.Windows.CppWinRT` sürümü de NuGet'ten en güncelle güncellenebilir.

- [ ] **Step 3: Spike kodunu yaz — ana thread MTA, GUI thread STA**

`spike/xaml-island-thread/spike.cpp`:

```cpp
// Spike: DesktopWindowXamlSource ikincil STA thread'de calisiyor mu?
// Ana thread MTA kalirken (BetterMagnifier'in main.cpp'si gibi) ayri bir
// STA thread'de XAML island ayaga kalkabiliyor mu?

#include <windows.h>
#include <objbase.h>
#include <thread>
#include <atomic>
#include <cstdio>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <MddBootstrap.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Hosting;
using namespace winrt::Microsoft::UI::Dispatching;

static std::atomic<int> g_result{-1};   // -1 = calismadi, 0 = basarili, >0 = HRESULT

static LRESULT CALLBACK HostWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void GuiThreadMain()
{
    try
    {
        // 1. STA apartment — XAML sart kosuyor.
        // Python analojisi: asyncio'da her thread'in kendi event loop'u olmasi gibi,
        // XAML de kendi thread'inde tek basina hukum surmek istiyor.
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        // 2. Windows App SDK bootstrapper — paketlenmemis uygulamada sart.
        // Herhangi bir WinRT/XAML tipine DOKUNMADAN once cagrilmali.
        const UINT32 majorMinor = 0x00010005;   // 1.5
        HRESULT hr = MddBootstrapInitialize(majorMinor, nullptr, PACKAGE_VERSION{});
        if (FAILED(hr)) { g_result = static_cast<int>(hr); return; }

        // 3. DispatcherQueue — XAML runtime bunu bu thread'de bekliyor.
        auto controller = DispatcherQueueController::CreateOnCurrentThread();

        // 4. XAML runtime'i baslat. Islands senaryosunda Application::Start()
        //    CAGIRILMAZ (o mesaj loop'unu ele gecirir); yerine bos bir
        //    Application ornegi kurup DispatcherShutdownMode ayarliyoruz.
        Application app{};
        app.DispatcherShutdownMode(DispatcherShutdownMode::OnExplicitShutdown);
        app.Resources(XamlControlsResources{});

        // 5. Host Win32 penceresi — DesktopWindowXamlSource mevcut bir HWND istiyor.
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HostWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SpikeIslandHost";
        RegisterClassExW(&wc);

        HWND host = CreateWindowExW(0, L"SpikeIslandHost", L"Spike Island",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!host) { g_result = static_cast<int>(HRESULT_FROM_WIN32(GetLastError())); return; }

        // 6. Island'i host pencereye bagla.
        DesktopWindowXamlSource source{};
        source.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(host));
        source.SiteBridge().ResizePolicy(
            winrt::Microsoft::UI::Content::ContentSizePolicy::ResizeContentToParentWindow);

        TextBlock label{};
        label.Text(L"Island ikincil STA thread'de ayakta");
        source.Content(label);
        source.SiteBridge().Show();

        ShowWindow(host, SW_SHOW);
        g_result = 0;   // Buraya geldiysek spike BASARILI

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        source.Close();
        controller.ShutdownQueueAsync();
        MddBootstrapShutdown();
    }
    catch (winrt::hresult_error const& e)
    {
        g_result = static_cast<int>(e.code().value);
    }
}

int main()
{
    // Ana thread MTA — BetterMagnifier'in main.cpp'si ile ayni.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { std::printf("MTA init basarisiz: 0x%08X\n", hr); return 1; }

    std::thread gui(GuiThreadMain);

    // Sonucu bekle (en fazla 10 saniye)
    for (int i = 0; i < 100 && g_result.load() == -1; ++i)
        Sleep(100);

    const int r = g_result.load();
    if (r == 0)
        std::printf("SPIKE BASARILI: island ikincil STA thread'de ayakta, ana thread MTA\n");
    else if (r == -1)
        std::printf("SPIKE ZAMAN ASIMI: island 10 saniyede ayaga kalkmadi\n");
    else
        std::printf("SPIKE BASARISIZ: HRESULT 0x%08X\n", static_cast<unsigned>(r));

    if (gui.joinable())
    {
        // Pencere kapatilinca thread cikacak; kullanici kapatmazsa detach.
        gui.detach();
    }

    CoUninitialize();
    return (r == 0) ? 0 : 1;
}
```

- [ ] **Step 4: Spike projesini solution'a ekle ve derle**

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" spike\xaml-island-thread\spike.vcxproj /restore /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Expected: derleme başarılı. Başarısız olursa hata mesajı, spike'ın verdiği ilk bilgidir — API isimleri veya namespace'ler pinlediğin SDK sürümünde farklı olabilir, `winrt/Microsoft.UI.Content.h` include'ı gerekebilir.

- [ ] **Step 5: Spike'ı çalıştır**

```bash
.\spike\xaml-island-thread\x64\Debug\spike.exe
```

Expected (başarı): `SPIKE BASARILI: island ikincil STA thread'de ayakta, ana thread MTA` ve içinde "Island ikincil STA thread'de ayakta" yazan bir pencere açılır.

Expected (başarısızlık): `SPIKE BASARISIZ: HRESULT 0x...`. En olası HRESULT'lar:
- `0x80070490` (ELEMENT_NOT_FOUND) — Windows App Runtime kurulu değil, `Microsoft.WindowsAppRuntime.Redist` yükle
- `0x8001010E` (RPC_E_WRONG_THREAD) — apartment sorunu, tasarım geçersiz

- [ ] **Step 6: Sonucu spec'e işle ve commit**

Spike başarılıysa spec bölüm 9'un başlığını güncelle:

```bash
git add docs/superpowers/specs/2026-08-04-control-panel-gui-design.md spike/
git commit -m @'
spike(gui): verify xaml island on secondary STA thread

* Main thread stays MTA, GUI thread runs STA with its own message loop
* DesktopWindowXamlSource attached to a Win32 host HWND
* Pins Windows App SDK to PINNED_SDK_VERSION
* Closes the open risk in the control panel design spec
'@
```

Spike BAŞARISIZ olursa: buradan sonraki task'lara geçme. Geri dönüş yolları (spec bölüm 9): paneli ayrı process'e taşımak (`WM_COPYDATA` veya named pipe IPC) ya da GUI teknolojisini yeniden değerlendirmek. Kullanıcıya sonucu bildir ve karar al.

---

### Task 2: Mesaj sabitleri ve durum snapshot'ı

**Files:**
- Create: `src/AppMessages.h`
- Create: `src/StatusSnapshot.h`
- Modify: `src/App.h` (üye ekle, accessor ekle), `src/App.cpp` (`Update()` snapshot yazar, FPS ölçümü)
- Modify: `src/TrayIcon.h:39` (`kTrayCallbackMsg` artık `AppMessages.h`'den gelir)
- Modify: `BetterMagnifier.vcxproj`, `BetterMagnifier.vcxproj.filters`

**Interfaces:**
- Consumes: hiçbir şey
- Produces:
  - `BetterMagnifier::WM_APP_TRAY`, `WM_APP_SETTINGS_CHANGED`, `WM_APP_SET_ZOOM`, `WM_APP_TOGGLE_ZOOM`, `WM_APP_TOGGLE_FREEZE`, `WM_APP_SCROLL_ZOOM`, `WM_APP_FOCUS_CHANGED`, `WM_APP_SHOW_PANEL` — hepsi `inline constexpr UINT`
  - `BetterMagnifier::MonitorStatus` — `zoomLevel` (`std::atomic<float>`), `isActive`/`isFrozen`/`captureOk`/`captureExcluded` (`std::atomic<bool>`), `fps` (`std::atomic<float>`)
  - `BetterMagnifier::StatusSnapshot` — `static constexpr size_t kMaxMonitors = 8`; `MonitorStatus& Monitor(size_t)`; `const MonitorStatus& Monitor(size_t) const`; `std::atomic<size_t> monitorCount`
  - `StatusSnapshot* App::Status()` — GUI thread'in okuyacağı pointer

- [ ] **Step 1: `src/AppMessages.h` yaz**

```cpp
#pragma once

// =============================================================================
// AppMessages.h — Thread'ler arasi mesaj sabitleri
// =============================================================================
// GUI thread ve input thread, render thread'e SADECE bu mesajlarla konusur.
// Paylasilan mutable state yok, kilit yok — Win32 mesaj kuyrugu bizim
// thread-safe kuyrugumuz.
//
// Python analojisi: queue.Queue() ile thread'ler arasi is gecirmek.
// Win32'de mesaj kuyrugu zaten her thread'de var, ayrica kurmaya gerek yok.
// =============================================================================

#ifndef BETTER_MAGNIFIER_APP_MESSAGES_H
#define BETTER_MAGNIFIER_APP_MESSAGES_H

#include <windows.h>

namespace BetterMagnifier {

// Tray icon callback'i — TrayIcon::Create bu degeri uCallbackMessage'a koyar
inline constexpr UINT WM_APP_TRAY             = WM_APP + 1;

// GUI -> motor: ayarlar degisti, SettingsStore'dan yeniden oku
inline constexpr UINT WM_APP_SETTINGS_CHANGED = WM_APP + 2;

// GUI -> motor: zoom seviyesini ayarla
//   wParam = monitor indeksi (size_t)
//   lParam = zoom * 1000 (int)  ornek: 2.50x -> 2500
inline constexpr UINT WM_APP_SET_ZOOM         = WM_APP + 3;

// GUI/hotkey -> motor: zoom ac-kapa
//   wParam = monitor indeksi, veya kFocusedMonitor = fare neredeyse orada
inline constexpr UINT WM_APP_TOGGLE_ZOOM      = WM_APP + 4;

// GUI/hotkey -> motor: freeze ac-kapa (wParam ayni)
inline constexpr UINT WM_APP_TOGGLE_FREEZE    = WM_APP + 5;

// Input thread -> motor: mouse wheel ile zoom degisimi
//   wParam = wheel delta (int, pozitif = zoom in)
//   lParam = kullanilmiyor. Render thread GetCursorPos() ile konumu kendisi
//   okur — olay ile isleme arasi birkac ms, ayni monitorde kalir.
inline constexpr UINT WM_APP_SCROLL_ZOOM      = WM_APP + 6;

// Input thread -> motor: klavye odagi degisti, focal point'i oraya kaydir
//   wParam = kullanilmiyor
//   lParam = odaklanan pencerenin HWND'si
inline constexpr UINT WM_APP_FOCUS_CHANGED    = WM_APP + 7;

// Tray -> motor: kontrol panelini goster
inline constexpr UINT WM_APP_SHOW_PANEL       = WM_APP + 8;

// wParam sentinel'i: "belirli bir monitor degil, farenin uzerinde oldugu monitor"
inline constexpr WPARAM kFocusedMonitor = static_cast<WPARAM>(-1);

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_APP_MESSAGES_H
```

- [ ] **Step 2: `src/StatusSnapshot.h` yaz**

```cpp
#pragma once

// =============================================================================
// StatusSnapshot.h — Motor -> GUI canli durum aktarimi
// =============================================================================
// Render thread her frame buraya yazar, GUI thread 10 Hz okur.
//
// Neden mutex degil atomic?
//   Render thread'in hot path'inde kilit almasi frame suresini kestirilemez
//   yapar — projenin butun amaci laggsizlik. std::atomic<float> ve
//   std::atomic<bool> x64'te lock-free (kontrol: is_always_lock_free).
//
// Neden 10 Hz okuma yeter?
//   Ayar panelinde 60 Hz gostergeye kimse bakmiyor. 10 Hz'de cekisme sifira
//   iner, insan gozu farki gormez.
//
// Tutarlilik notu: alanlar tek tek atomic, yapinin TAMAMI atomic degil.
// Yani GUI ayni monitorun zoomLevel'ini yeni, isActive'ini bir frame eski
// okuyabilir. Gosterge icin kabul edilebilir — kritik karar alinmiyor.
//
// Python analojisi: threading.Event / itertools yerine, her alan icin ayri
// bir thread-safe kutucuk. Yazan beklemez, okuyan beklemez.
// =============================================================================

#ifndef BETTER_MAGNIFIER_STATUS_SNAPSHOT_H
#define BETTER_MAGNIFIER_STATUS_SNAPSHOT_H

#include <atomic>
#include <array>
#include <cstddef>

namespace BetterMagnifier {

struct MonitorStatus
{
    std::atomic<float> zoomLevel{1.0f};
    std::atomic<bool>  isActive{false};
    std::atomic<bool>  isFrozen{false};

    // DXGICapture::IsInitialized() && !NeedsReinit()
    std::atomic<bool>  captureOk{false};

    // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) basarili miydi?
    // false ise overlay kendini yakalar — feedback loop riski, panelde uyari.
    std::atomic<bool>  captureExcluded{false};

    std::atomic<float> fps{0.0f};
};

class StatusSnapshot
{
public:
    static constexpr size_t kMaxMonitors = 8;

    StatusSnapshot() = default;
    StatusSnapshot(const StatusSnapshot&) = delete;
    StatusSnapshot& operator=(const StatusSnapshot&) = delete;

    // Bounds-check'li erisim. Sinir disi index son elemana duser —
    // GUI thread'in yaris kosulunda cokmemesi icin (monitor sayisi
    // WM_DISPLAYCHANGE ile degisebilir, GUI bir tick geride olabilir).
    MonitorStatus& Monitor(size_t i)
    {
        return m_monitors[i < kMaxMonitors ? i : kMaxMonitors - 1];
    }

    const MonitorStatus& Monitor(size_t i) const
    {
        return m_monitors[i < kMaxMonitors ? i : kMaxMonitors - 1];
    }

    std::atomic<size_t> monitorCount{0};

private:
    std::array<MonitorStatus, kMaxMonitors> m_monitors{};
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_STATUS_SNAPSHOT_H
```

- [ ] **Step 3: `TrayIcon.h`'yi `AppMessages.h`'ye bağla**

`src/TrayIcon.h` içinde `kTrayCallbackMsg` tanımını kaldır, tek kaynağa bağla. Şu satırı:

```cpp
    static constexpr UINT kTrayCallbackMsg = WM_APP + 1;
```

şununla değiştir:

```cpp
    // Tek kaynak: AppMessages.h. Iki yerde WM_APP+1 yazmak, birini
    // degistirip digerini unutmaya davetiye.
    static constexpr UINT kTrayCallbackMsg = WM_APP_TRAY;
```

ve dosyanın include bloğuna ekle:

```cpp
#include "AppMessages.h"
```

- [ ] **Step 4: `App.h`'ye snapshot üyesi ve accessor ekle**

`src/App.h` include bloğuna ekle:

```cpp
#include "StatusSnapshot.h"
```

`public:` bölümüne, `Shutdown()` altına ekle:

```cpp
    // GUI thread bu pointer'i okur. App yasadigi surece gecerli.
    StatusSnapshot* Status() { return &m_status; }
```

`private:` bölümündeki component'lerin yanına ekle:

```cpp
    StatusSnapshot              m_status;

    // FPS olcumu (snapshot icin) — monitor basina son frame zamani
    std::array<std::chrono::steady_clock::time_point, StatusSnapshot::kMaxMonitors> m_lastFrameTime{};
```

`App.h` include bloğuna `<array>` ve `<chrono>` ekle.

- [ ] **Step 5: `App::Update()` içinde snapshot'ı yaz**

`src/App.cpp` içindeki `Update()` fonksiyonunda, `anyActive = true;` satırından SONRA hiçbir şey eklemeyeceğiz — snapshot yazımı döngünün en başına, monitör null kontrolünden sonra gelmeli ki pasif monitörler de doğru raporlansın.

`Update()` içindeki döngüde, `MonitorInfo* mon = ...; if (!mon) continue;` satırlarından hemen sonra ekle:

```cpp
        // ── Snapshot'i guncelle (GUI thread bunu 10 Hz okuyor) ──
        // Pasif monitorler de raporlanmali, bu yuzden zoom kontrolunden ONCE.
        auto& st = m_status.Monitor(i);
        st.zoomLevel.store(mon->zoom.zoomLevel, std::memory_order_relaxed);
        st.isActive.store(mon->zoom.isActive, std::memory_order_relaxed);
        st.isFrozen.store(mon->zoom.isFrozen, std::memory_order_relaxed);
        st.captureOk.store(
            m_captures[i].IsInitialized() && !m_captures[i].NeedsReinit(),
            std::memory_order_relaxed);
```

`Update()` fonksiyonunun EN BAŞINA (`POINT cursor{};` satırından önce) ekle:

```cpp
    m_status.monitorCount.store(m_overlays.size(), std::memory_order_relaxed);
```

- [ ] **Step 6: FPS ölçümünü `RenderMonitor()` içine ekle**

`src/App.cpp` `RenderMonitor()` içinde, `m_renderer.Present(monitorIndex, true);` satırından hemen SONRA ekle:

```cpp
        // ── FPS olcumu ──
        // Present'ten sonra olcuyoruz, cunku vSync bekleyisi de frame
        // suresinin parcasi. Iki frame arasi sureyi 1/dt ile FPS'e ceviriyoruz.
        // Python analojisi: time.perf_counter() farki, ama steady_clock
        // monotonic garantisi veriyor (sistem saati geri alinsa bile bozulmaz).
        const auto now = std::chrono::steady_clock::now();
        auto& lastTime = m_lastFrameTime[monitorIndex < StatusSnapshot::kMaxMonitors
                                        ? monitorIndex : StatusSnapshot::kMaxMonitors - 1];

        if (lastTime.time_since_epoch().count() != 0)
        {
            const auto dt = std::chrono::duration<float>(now - lastTime).count();
            if (dt > 0.0f)
            {
                // Ustel yumusatma — ham 1/dt cok zipliyor, gostergede okunmaz.
                const float instant = 1.0f / dt;
                auto& fpsSlot = m_status.Monitor(monitorIndex).fps;
                const float prev = fpsSlot.load(std::memory_order_relaxed);
                const float smoothed = (prev <= 0.0f) ? instant : (prev * 0.9f + instant * 0.1f);
                fpsSlot.store(smoothed, std::memory_order_relaxed);
            }
        }
        lastTime = now;
```

- [ ] **Step 7: Zoom pasifken FPS'i sıfırla**

`Update()` içinde, zoom pasif dalında (`if (!mon->zoom.isActive)` bloğunun içinde, `Hide()` çağrısının yanına) ekle:

```cpp
            // Zoom kapaliyken FPS anlamsiz — panelde "—" gorunmesi icin sifirla
            st.fps.store(0.0f, std::memory_order_relaxed);
            m_lastFrameTime[i < StatusSnapshot::kMaxMonitors ? i : StatusSnapshot::kMaxMonitors - 1] = {};
```

- [ ] **Step 8: `captureExcluded`'i overlay'den snapshot'a taşı**

`src/OverlayWindow.h` `public:` bölümüne ekle:

```cpp
    // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) basarili miydi?
    // false ise overlay kendini yakalar (feedback loop). Panelde uyari gosterilir.
    bool IsExcludedFromCapture() const { return m_excludedFromCapture; }
```

`private:` bölümüne ekle:

```cpp
    bool    m_excludedFromCapture = false;
```

`src/OverlayWindow.cpp` `Create()` içindeki `SetWindowDisplayAffinity` bloğunu şuna çevir:

```cpp
    if (SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE))
    {
        m_excludedFromCapture = true;
    }
    else
    {
        m_excludedFromCapture = false;
        LOG_WARN("SetWindowDisplayAffinity basarisiz ({}) — Windows 10 2004+ gerekiyor, "
                 "feedback loop olusabilir", GetLastError());
    }
```

Move constructor ve move assignment'a `m_excludedFromCapture` alanını ekle (mevcut `m_visible` alanının yanına, aynı desende).

`src/App.cpp` `Update()` içindeki snapshot yazma bloğuna ekle:

```cpp
        st.captureExcluded.store(m_overlays[i].IsExcludedFromCapture(),
                                 std::memory_order_relaxed);
```

- [ ] **Step 9: Yeni header'ları vcxproj'a ekle**

`BetterMagnifier.vcxproj` içindeki header `ItemGroup`'una:

```xml
    <ClInclude Include="src\AppMessages.h" />
    <ClInclude Include="src\StatusSnapshot.h" />
```

`BetterMagnifier.vcxproj.filters` içindeki header `ItemGroup`'una:

```xml
    <ClInclude Include="src\AppMessages.h">
      <Filter>Header Files</Filter>
    </ClInclude>
    <ClInclude Include="src\StatusSnapshot.h">
      <Filter>Header Files</Filter>
    </ClInclude>
```

- [ ] **Step 10: Derle**

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Expected: PASS, sıfır uyarı. Olası hata: `std::atomic<float>` kopyalanamaz olduğu için `MonitorStatus`'u değere göre döndürmeye çalışırsan derleme patlar — her yerde referans kullan.

- [ ] **Step 11: Çalıştır ve snapshot'ın dolduğunu doğrula**

Doğrulama Döngüsü bölümündeki çalıştır-ve-log-oku komutunu kullan.

Expected: mevcut log çıktısı değişmez (snapshot henüz kimse tarafından okunmuyor), tek WARN/ERROR yok, exit code 0. Snapshot'ın gerçekten dolduğunu Task 6'da panel canlı gösterince göreceğiz — bu task'ta doğrulanan şey, snapshot yazımının frame loop'unu bozmadığı.

- [ ] **Step 12: Commit**

```bash
git add src/AppMessages.h src/StatusSnapshot.h src/App.h src/App.cpp src/TrayIcon.h src/OverlayWindow.h src/OverlayWindow.cpp BetterMagnifier.vcxproj BetterMagnifier.vcxproj.filters
git commit -m @'
feat(app): add status snapshot and message constants

* AppMessages.h centralizes cross-thread WM_APP_* constants
* StatusSnapshot exposes lock-free per-monitor state for the GUI thread
* App::Update writes zoom, active, frozen, capture health every frame
* RenderMonitor measures smoothed FPS per monitor
* OverlayWindow reports whether capture exclusion succeeded
'@
```

---

### Task 3: SettingsStore — INI kalıcılığı ve hotkey ayrıştırma

Spec bölüm 4 "Ayar deposu". Bu task'ın saf mantığı (`ParseHotkey`/`FormatHotkey`) projedeki tek gerçekten test edilebilir parça — assert self-check buraya konur.

**Files:**
- Create: `src/SettingsStore.h`
- Create: `src/SettingsStore.cpp`
- Modify: `src/main.cpp` (Debug build'de self-check çağrısı)
- Modify: `BetterMagnifier.vcxproj`, `BetterMagnifier.vcxproj.filters`

**Interfaces:**
- Consumes: hiçbir şey
- Produces:
  - `enum class FollowMode { Mouse, MouseAndFocus }`
  - `struct GeneralSettings` — `UINT toggleModifiers`, `UINT toggleVk`, `UINT freezeModifiers`, `UINT freezeVk`, `bool hijackWinZ`, `FollowMode followMode`, `bool startWithWindows`, `bool rememberZoomLevel`
  - `struct MonitorSettings` — `float minZoom`, `float maxZoom`, `float zoomStep`, `float lastZoom`
  - `class SettingsStore` — `bool Load()`, `bool Save() const`, `const GeneralSettings& General() const`, `GeneralSettings& MutableGeneral()`, `MonitorSettings Monitor(const std::wstring&) const`, `void SetMonitor(const std::wstring&, const MonitorSettings&)`, `static std::filesystem::path FilePath()`
  - `bool ParseHotkey(std::wstring_view, UINT& modifiers, UINT& vk)` — serbest fonksiyon
  - `std::wstring FormatHotkey(UINT modifiers, UINT vk)` — serbest fonksiyon
  - `void SettingsStoreSelfCheck()` — sadece `_DEBUG`'da tanımlı

- [ ] **Step 1: `src/SettingsStore.h` yaz**

```cpp
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
// ayni section/key mantigi. Fark: Python'da dosyayi acip parse ediyorsun,
// Win32'de her cagri dosyaya tek basina gidiyor (yavas ama biz nadir cagiriyoruz).
//
// THREAD SAHIPLIGI: Bu sinif GUI thread tarafindan yazilir, render thread
// WM_APP_SETTINGS_CHANGED aldiginda okur. Ikisi ayni anda dokunmaz cunku
// GUI once yazar SONRA mesaj postalar. Yine de Load/Save disinda paylasilan
// state tutmuyor — kopya semantigi ile gecis yapiliyor.
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
    UINT       toggleModifiers  = MOD_CONTROL | MOD_ALT;
    UINT       toggleVk         = 'Z';
    UINT       freezeModifiers  = MOD_CONTROL | MOD_ALT;
    UINT       freezeVk         = 'X';

    // Win+Z'yi WH_KEYBOARD_LL ile ele gecir. VARSAYILAN KAPALI:
    // acildiginda Windows 11 Snap Layouts calismaz hale gelir.
    bool       hijackWinZ       = false;

    FollowMode followMode       = FollowMode::MouseAndFocus;
    bool       startWithWindows = false;
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
// varsayilanini koruyabilir.
bool ParseHotkey(std::wstring_view text, UINT& modifiers, UINT& vk);

// Ters yon. Modifier sirasi her zaman Ctrl, Alt, Shift, Win — boylece
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
    // Bozuk deger (parse edilemeyen hotkey, negatif zoom) varsayilana duser.
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
void SettingsStoreSelfCheck();
#endif

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_SETTINGS_STORE_H
```

- [ ] **Step 2: Self-check'i yaz (implementasyondan ÖNCE — kırmızıdan başlıyoruz)**

`src/SettingsStore.cpp` dosyasını sadece self-check ve boş gövdelerle oluştur:

```cpp
// =============================================================================
// SettingsStore.cpp — INI kalicilik implementasyonu
// =============================================================================

#include "pch.h"
#include "SettingsStore.h"
#include "Logger.h"

#include <shlobj.h>       // SHGetKnownFolderPath
#include <cassert>

namespace BetterMagnifier {

bool ParseHotkey(std::wstring_view, UINT&, UINT&) { return false; }
std::wstring FormatHotkey(UINT, UINT) { return {}; }

bool SettingsStore::Load() { return false; }
bool SettingsStore::Save() const { return false; }
MonitorSettings SettingsStore::Monitor(const std::wstring&) const { return {}; }
void SettingsStore::SetMonitor(const std::wstring&, const MonitorSettings&) {}
std::filesystem::path SettingsStore::FilePath() { return {}; }

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
```

- [ ] **Step 3: Self-check'i main.cpp'ye bağla ve kırmızı olduğunu gör**

`src/main.cpp` include bloğuna ekle:

```cpp
#include "SettingsStore.h"
```

`main.cpp` içinde, `LOG_WARN` smoke test satırlarının olduğu yere — App başlatılmadan ÖNCE — ekle:

```cpp
    // ── Debug self-check ──
    // Saf mantigi olan tek bilesenimiz. Bozulursa burada dusuyoruz,
    // uygulamanin ortasinda tuhaf davranis olarak degil.
#ifdef _DEBUG
    BetterMagnifier::SettingsStoreSelfCheck();
#endif
```

Yeni dosyaları vcxproj'a ekle:

`BetterMagnifier.vcxproj` — source `ItemGroup`'una `<ClCompile Include="src\SettingsStore.cpp" />`, header `ItemGroup`'una `<ClInclude Include="src\SettingsStore.h" />`. Aynı ikisini `.filters`'a uygun `<Filter>` etiketleriyle ekle.

Derle ve çalıştır:

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo && .\bin\Debug-x64\BetterMagnifier.exe
```

Expected: FAIL — assertion penceresi açılır, `assert(ParseHotkey(L"Ctrl+Alt+Z", mods, vk))` satırında düşer. Boş gövdeler `false` döndüğü için ilk assert patlıyor. **Bu beklenen sonuç** — testin gerçekten bir şey ölçtüğünü kanıtlıyor.

- [ ] **Step 4: `ParseHotkey` ve `FormatHotkey`'i implement et**

`src/SettingsStore.cpp` içindeki iki boş gövdeyi değiştir:

```cpp
// =============================================================================
// Hotkey metin donusumu
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

} // anonymous namespace

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
```

`<cwctype>` include'ı ekle (`towlower`, `towupper` için).

- [ ] **Step 5: Testi çalıştır — hotkey assert'leri geçmeli, FilePath'te düşmeli**

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo && .\bin\Debug-x64\BetterMagnifier.exe
```

Expected: 1-7 arası assert'ler geçer, 8. blokta `assert(!p.empty())` düşer — `FilePath()` hâlâ boş döndürüyor. İlerleme kanıtı.

- [ ] **Step 6: `FilePath`, `Load`, `Save`, `Monitor`, `SetMonitor`'ı implement et**

```cpp
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
    ok &= WritePrivateProfileStringW(L"General", L"ToggleHotkey", toggle.c_str(), file.c_str()) != 0;
    ok &= WritePrivateProfileStringW(L"General", L"FreezeHotkey", freeze.c_str(), file.c_str()) != 0;
    ok &= WriteInt(file, L"General", L"HijackWinZ",        m_general.hijackWinZ ? 1 : 0);
    ok &= WriteInt(file, L"General", L"StartWithWindows",  m_general.startWithWindows ? 1 : 0);
    ok &= WriteInt(file, L"General", L"RememberZoomLevel", m_general.rememberZoomLevel ? 1 : 0);
    ok &= WritePrivateProfileStringW(L"General", L"FollowMode",
            (m_general.followMode == FollowMode::Mouse) ? L"Mouse" : L"MouseAndFocus",
            file.c_str()) != 0;

    for (const auto& [device, ms] : m_monitors)
    {
        ok &= WriteFloat(file, device, L"MinZoom",  ms.minZoom);
        ok &= WriteFloat(file, device, L"MaxZoom",  ms.maxZoom);
        ok &= WriteFloat(file, device, L"ZoomStep", ms.zoomStep);
        ok &= WriteFloat(file, device, L"LastZoom", ms.lastZoom);
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
```

Ve anonim namespace'e üç yardımcı ekle (INI float desteği yok, metin üzerinden gidiyoruz):

```cpp
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
```

Bu üç yardımcı `Load`/`Save`'den ÖNCE tanımlanmalı (anonim namespace bloğunun içinde, `ModifierFromName` yanında). `<cwchar>` include'ı ekle (`std::wcstof`).

- [ ] **Step 7: Testi çalıştır — tüm assert'ler geçmeli**

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Expected: derleme PASS, sıfır uyarı.

Sonra Doğrulama Döngüsü'ndeki çalıştır-ve-log-oku komutunu kullan.

Expected log satırları:
```
SettingsStore self-check basliyor...
SettingsStore self-check GECTI
```
Assertion penceresi açılmamalı, exit code 0 olmalı.

- [ ] **Step 8: INI dosyasının gerçekten yazıldığını gözle doğrula**

Self-check kendi dosyasını siliyor, o yüzden elle bir kayıt üret:

```bash
powershell -Command "$p = Join-Path $env:APPDATA 'BetterMagnifier\settings.ini'; if (Test-Path $p) { Get-Content $p } else { 'Dosya yok - self-check temizligi dogru calisti' }"
```

Expected: "Dosya yok" (self-check temizliğini yaptı) — bu doğru davranış. Dosya varsa self-check temizlik adımı çalışmamış demektir, `std::filesystem::remove` çağrısını kontrol et.

- [ ] **Step 9: Commit**

```bash
git add src/SettingsStore.h src/SettingsStore.cpp src/main.cpp BetterMagnifier.vcxproj BetterMagnifier.vcxproj.filters
git commit -m @'
feat(settings): add INI-backed settings store

* GeneralSettings and per-monitor MonitorSettings persisted to %APPDATA%
* ParseHotkey and FormatHotkey with stable round-trip modifier ordering
* Missing file, unknown keys and out-of-range values fall back to defaults
* Assert-based self-check runs on Debug startup, no test framework needed
'@
```

---

### Task 4: InputThread — hook'ları render thread'den çıkar

Spec bölüm 3.1. Mevcut latent bug'ı kapatır: `WH_MOUSE_LL` şu anda ana thread'de, render loop'unun arkasında kuyruğa giriyor.

**Files:**
- Create: `src/InputThread.h`
- Create: `src/InputThread.cpp`
- Modify: `src/HotkeyManager.h`, `src/HotkeyManager.cpp` (hook kodunu ve scroll callback'ini kaldır)
- Modify: `src/App.h`, `src/App.cpp` (`InputThread` üyesi, `WM_APP_SCROLL_ZOOM` işleyicisi)
- Modify: `BetterMagnifier.vcxproj`, `BetterMagnifier.vcxproj.filters`

**Interfaces:**
- Consumes: `AppMessages.h`'den `WM_APP_SCROLL_ZOOM`; `SettingsStore.h`'den `GeneralSettings`
- Produces:
  - `class InputThread` — `bool Start(HWND targetHwnd)`, `void Stop()`, `bool IsRunning() const`
  - `HotkeyManager` artık `SetScrollCallback` SUNMUYOR — çağıranlar `WM_APP_SCROLL_ZOOM` kullanır

- [ ] **Step 1: `src/InputThread.h` yaz**

```cpp
#pragma once

// =============================================================================
// InputThread.h — Low-level hook'lar icin ayri thread
// =============================================================================
//
// NEDEN AYRI THREAD (bu dosyanin butun varlik sebebi):
//   Low-level hook'lar (WH_MOUSE_LL, WH_KEYBOARD_LL) onlari KURAN thread'in
//   mesaj kuyrugunda cagrilir. Render thread'imiz Present(vSync) ile bir frame
//   boyunca blokluyor. Hook orada olursa sistemdeki HER fare/tus olayi bizim
//   frame'imizin arkasinda bekler — makinede her yerde girdi gecikmesi.
//
//   Ustune Windows'un LowLevelHooksTimeout'u (varsayilan 300 ms) asilirsa
//   hook'u sessizce devre disi birakiyor.
//
//   Bu thread hicbir agir is yapmaz: hook callback'i sadece PostMessage eder
//   ve doner. Gercek isi render thread yapar.
//
// Python analojisi: pynput'un listener'ini ayri bir thread'de calistirmak.
// Fark: Win32'de hook'un yasadigi thread'in GetMessage loop'u olmak ZORUNDA,
// yoksa callback hic cagrilmaz.
// =============================================================================

#ifndef BETTER_MAGNIFIER_INPUT_THREAD_H
#define BETTER_MAGNIFIER_INPUT_THREAD_H

#include <windows.h>
#include <thread>
#include <atomic>

namespace BetterMagnifier {

class InputThread
{
public:
    InputThread() = default;
    ~InputThread();

    InputThread(const InputThread&) = delete;
    InputThread& operator=(const InputThread&) = delete;

    // Thread'i baslat ve hook'lari kur.
    // targetHwnd: olaylarin PostMessage ile gonderilecegi pencere (mesaj penceresi).
    // Hook'lar thread ICINDE kurulur — Start() donmeden once kurulum
    // tamamlanmis olur (senkron bekleme var).
    bool Start(HWND targetHwnd);

    // Thread'e WM_QUIT postala, hook'lari kaldir, join et.
    // Idempotent — iki kez cagirmak guvenli.
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    void ThreadMain();
    bool InstallHooks();
    void RemoveHooks();

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};

    // Thread'e mesaj postalamak icin — Stop() bunu kullanir
    std::atomic<DWORD> m_threadId{0};

    HWND   m_target     = nullptr;
    HHOOK  m_mouseHook  = nullptr;

    // Win32 hook callback'leri static olmak zorunda (calling convention).
    // Bu yuzden global instance pointer'i tutuyoruz.
    // Tek InputThread varsayimi — birden fazla olursa bu kirilir, ama
    // uygulamada tek tane var ve olmasi da gerekmiyor.
    static InputThread* s_instance;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_INPUT_THREAD_H
```

- [ ] **Step 2: `src/InputThread.cpp` yaz**

```cpp
// =============================================================================
// InputThread.cpp
// =============================================================================

#include "pch.h"
#include "InputThread.h"
#include "AppMessages.h"
#include "Logger.h"

#include <future>

namespace BetterMagnifier {

InputThread* InputThread::s_instance = nullptr;

InputThread::~InputThread()
{
    Stop();
}

// =============================================================================
// Start — thread'i baslat, hook kurulumunu bekle
// =============================================================================
//
// Neden std::promise ile bekliyoruz?
//   Hook'lar thread ICINDE kurulmali (o thread'in kuyruguna baglanacaklar).
//   Ama Start()'in cagiran tarafa "hook'lar hazir" veya "kurulamadi" demesi
//   lazim. promise/future tam bu is icin: thread sonucu yaziyor, Start okuyor.
//
// Python analojisi: threading.Event() + bir sonuc degiskeni, ya da
// concurrent.futures.Future.
// =============================================================================
bool InputThread::Start(HWND targetHwnd)
{
    if (m_running.load(std::memory_order_acquire))
        return true;

    if (!targetHwnd)
    {
        LOG_ERROR("InputThread::Start — targetHwnd null!");
        return false;
    }

    m_target   = targetHwnd;
    s_instance = this;

    std::promise<bool> ready;
    std::future<bool> readyFuture = ready.get_future();

    m_thread = std::thread([this, p = std::move(ready)]() mutable {
        m_threadId.store(GetCurrentThreadId(), std::memory_order_release);

        const bool ok = InstallHooks();
        p.set_value(ok);

        if (!ok)
            return;

        m_running.store(true, std::memory_order_release);
        ThreadMain();
        m_running.store(false, std::memory_order_release);

        RemoveHooks();
    });

    const bool ok = readyFuture.get();
    if (!ok)
    {
        LOG_ERROR("InputThread hook kurulumu basarisiz");
        if (m_thread.joinable())
            m_thread.join();
        s_instance = nullptr;
        return false;
    }

    LOG_INFO("InputThread baslatildi (thread id: {})",
        m_threadId.load(std::memory_order_acquire));
    return true;
}

// =============================================================================
// InstallHooks — thread ICINDE cagrilir
// =============================================================================
bool InputThread::InstallHooks()
{
    m_mouseHook = SetWindowsHookExW(
        WH_MOUSE_LL,
        LowLevelMouseProc,
        GetModuleHandleW(nullptr),
        0   // 0 = global (tum thread'ler)
    );

    if (!m_mouseHook)
    {
        LOG_ERROR("WH_MOUSE_LL kurulamadi: {}", GetLastError());
        return false;
    }

    LOG_INFO("  Mouse hook aktif (input thread'de)");
    return true;
}

void InputThread::RemoveHooks()
{
    if (m_mouseHook)
    {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
        LOG_DEBUG("Mouse hook kaldirildi");
    }
}

// =============================================================================
// ThreadMain — hook'larin yasamasi icin gereken mesaj loop'u
// =============================================================================
//
// Bu loop hicbir pencereye ait degil (thread-only mesajlar). Tek isi
// hook callback'lerinin cagrilabilmesi icin thread'i "mesaj pompalayan"
// halde tutmak. GetMessage bloklar — CPU %0.
//
// PeekMessage KULLANMIYORUZ: burada render yapmiyoruz, bloklamak dogrusu.
// =============================================================================
void InputThread::ThreadMain()
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// =============================================================================
// Stop
// =============================================================================
void InputThread::Stop()
{
    const DWORD tid = m_threadId.load(std::memory_order_acquire);

    if (tid != 0)
    {
        // Thread-only WM_QUIT — GetMessage 0 dondurur, loop cikar.
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }

    if (m_thread.joinable())
        m_thread.join();

    m_threadId.store(0, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    s_instance = nullptr;
}

// =============================================================================
// LowLevelMouseProc — HIZLI DONMELI
// =============================================================================
//
// Bu callback sistemdeki her fare olayinda cagriliyor. Icinde is yapmak
// yasak: sadece ilgilendigimiz olayi PostMessage ile render thread'e atip
// hemen donuyoruz.
//
// PostMessage (SendMessage DEGIL) kullanmak kritik: SendMessage hedef
// thread'in mesaji islemesini BEKLER — render thread Present'te blokluysa
// bu hook'u kilitler ve LowLevelHooksTimeout'a takilir.
// =============================================================================
LRESULT CALLBACK InputThread::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance && s_instance->m_target && wParam == WM_MOUSEWHEEL)
    {
        auto* data = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (data)
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(data->mouseData);

            // Konumu gondermiyoruz — render thread GetCursorPos() ile kendisi
            // okuyor. Olay ile isleme arasi birkac ms, fare ayni monitorde kalir.
            PostMessageW(s_instance->m_target, WM_APP_SCROLL_ZOOM,
                         static_cast<WPARAM>(delta), 0);
        }
    }

    // Chain'i MUTLAKA devam ettir — yoksa diger uygulamalar fare olaylarini
    // alamaz. return 1 sadece olayi YUTMAK istedigimizde (Task 9, Win+Z).
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace BetterMagnifier
```

- [ ] **Step 3: `HotkeyManager`'dan hook kodunu çıkar**

`src/HotkeyManager.h`:
- `using ScrollCallback = std::function<void(int delta, POINT mousePos)>;` satırını sil
- `void SetScrollCallback(ScrollCallback cb) { m_onScroll = std::move(cb); }` satırını sil
- `ScrollCallback m_onScroll;` üyesini sil
- `static LRESULT CALLBACK LowLevelMouseProc(...)`, `void StartMouseHook()`, `void StopMouseHook()` bildirimlerini sil
- `HHOOK m_mouseHook = nullptr;` üyesini sil
- `static HotkeyManager* s_instance;` üyesini sil (artık static callback yok)
- `#include <thread>` ve `#include <atomic>` include'larını sil (kullanılmıyor)

Sınıf yorumunu güncelle:

```cpp
// =============================================================================
// HotkeyManager.h — Global Hotkeys (RegisterHotKey)
// =============================================================================
// Sadece RegisterHotKey ile calisir. Low-level fare/klavye hook'lari
// InputThread'e tasindi — onlar render thread'de olamaz (bkz. InputThread.h).
//
// RegisterHotKey PENCEREYE bagli: WM_HOTKEY mesaji hangi pencereye
// kaydettiysen ona gider. Bu yuzden bu sinif render thread'de kaliyor,
// mesaj penceresinin sahibi orada.
// =============================================================================
```

`src/HotkeyManager.cpp`:
- `HotkeyManager* HotkeyManager::s_instance = nullptr;` satırını sil
- `Initialize()` içindeki `s_instance = this;` satırını sil
- `Initialize()` içindeki `StartMouseHook();` çağrısını sil
- `Shutdown()` içindeki `StopMouseHook();` ve `s_instance = nullptr;` satırlarını sil
- `StartMouseHook()`, `StopMouseHook()`, `LowLevelMouseProc()` fonksiyon gövdelerini tamamen sil (dosya sonundaki blok)

Hotkey kayıtlarını `GeneralSettings`'ten alacak şekilde `Initialize` imzasını değiştir:

`HotkeyManager.h`:
```cpp
    // settings: hangi tus kombinasyonlari kaydedilecek
    bool Initialize(HWND hwnd, const GeneralSettings& settings);

    // Ayarlar degisince cagir — eskileri kaldirip yenilerini kaydeder.
    // Basarisiz kayitlari raporlar: bit 0 = toggle basarisiz, bit 1 = freeze basarisiz.
    UINT Reregister(const GeneralSettings& settings);
```

ve `#include "SettingsStore.h"` ekle.

`HotkeyManager.cpp` içinde `Initialize`'ı şuna çevir:

```cpp
bool HotkeyManager::Initialize(HWND hwnd, const GeneralSettings& settings)
{
    if (m_initialized)
        return true;

    m_hwnd = hwnd;
    LOG_INFO("HotkeyManager baslatiliyor...");

    const UINT failed = Reregister(settings);
    m_initialized = true;

    if (failed != 0)
        LOG_WARN("Bazi hotkey'ler kaydedilemedi (bayrak: 0b{:b})", failed);

    LOG_INFO("HotkeyManager basariyla baslatildi");
    return true;
}

// =============================================================================
// Reregister — ayarlar degisince hotkey'leri yeniden kaydet
// =============================================================================
//
// NEDEN MOD_WIN KULLANMIYORUZ (varsayilanda):
//   Win+Z Windows 11'de Snap Layouts'a rezerve — RegisterHotKey basarisiz
//   doner, sistem kisayollarini override edemeyiz. Ctrl+Alt+<harf> guvenli alan.
//   Kullanici isterse GUI'den Win+... secebilir, ama sonucu gorur (kirmizi satir).
// =============================================================================
UINT HotkeyManager::Reregister(const GeneralSettings& settings)
{
    if (!m_hwnd)
        return 0b11;

    // Eskileri kaldir — kayitli degillerse UnregisterHotKey sessizce basarisiz olur,
    // sorun degil.
    UnregisterHotKey(m_hwnd, kHotkeyToggleZoom);
    UnregisterHotKey(m_hwnd, kHotkeyFreeze);

    UINT failed = 0;

    // MOD_NOREPEAT: basili tutunca tekrarlamasin (toggle icin sart)
    if (!RegisterHotKey(m_hwnd, kHotkeyToggleZoom,
                        settings.toggleModifiers | MOD_NOREPEAT, settings.toggleVk))
    {
        LOG_ERROR("Toggle hotkey kaydedilemedi ({}): {}",
            GetLastError(), ToUtf8(FormatHotkey(settings.toggleModifiers, settings.toggleVk)));
        failed |= 0b01;
    }
    else
    {
        LOG_INFO("  Hotkey: {} = Toggle Zoom",
            ToUtf8(FormatHotkey(settings.toggleModifiers, settings.toggleVk)));
    }

    if (!RegisterHotKey(m_hwnd, kHotkeyFreeze,
                        settings.freezeModifiers | MOD_NOREPEAT, settings.freezeVk))
    {
        LOG_ERROR("Freeze hotkey kaydedilemedi ({}): {}",
            GetLastError(), ToUtf8(FormatHotkey(settings.freezeModifiers, settings.freezeVk)));
        failed |= 0b10;
    }
    else
    {
        LOG_INFO("  Hotkey: {} = Freeze/Pin",
            ToUtf8(FormatHotkey(settings.freezeModifiers, settings.freezeVk)));
    }

    return failed;
}
```

`HotkeyManager.h`'ye `UINT m_lastFailedMask = 0;` üyesi ve `UINT LastFailedMask() const { return m_lastFailedMask; }` accessor'u ekle; `Reregister` sonunda `m_lastFailedMask = failed;` yaz. GUI bunu Task 7'de okuyacak.

- [ ] **Step 4: `App`'e `SettingsStore` ve `InputThread` bağla**

`src/App.h`:
- include'lara `#include "SettingsStore.h"`, `#include "InputThread.h"`, `#include "AppMessages.h"` ekle
- `public:` bölümüne ekle:

```cpp
    SettingsStore* Settings() { return &m_settings; }
```

- `private:` bölümüne ekle:

```cpp
    SettingsStore   m_settings;
    InputThread     m_inputThread;
```

- `private:` metodlarına ekle:

```cpp
    // Ayarlar degistiginde uygula (WM_APP_SETTINGS_CHANGED)
    void ApplySettings();

    // Fare/verilen indekse gore hedef monitoru bul. Bulamazsa false.
    bool ResolveMonitorIndex(WPARAM wparam, size_t& outIndex) const;
```

`src/App.cpp` `InitializeComponents()` içinde:
- En başa (MonitorManager'dan ÖNCE) ekle:

```cpp
    // ── 0. Ayarlar ──
    // Diger her sey ayarlara bagli olabilir, en once yukleniyor.
    m_settings.Load();
```

- `m_hotkeyManager.Initialize(m_messageHwnd);` satırını şununla değiştir:

```cpp
    m_hotkeyManager.Initialize(m_messageHwnd, m_settings.General());
```

- `m_trayIcon.Create(...)` satırından SONRA ekle:

```cpp
    // ── 5. Input thread ──
    // Hook'lar burada, render thread'de DEGIL (bkz. InputThread.h).
    // Basarisiz olursa scroll zoom calismaz ama uygulama ayakta kalir.
    if (!m_inputThread.Start(m_messageHwnd))
        LOG_WARN("InputThread baslatilamadi — mouse wheel zoom devre disi");
```

- [ ] **Step 5: `SetupCallbacks`'ten scroll callback'ini kaldır**

`src/App.cpp` `SetupCallbacks()` içindeki şu satırı sil:

```cpp
    m_hotkeyManager.SetScrollCallback([this](int delta, POINT pos) { OnScroll(delta, pos); });
```

- [ ] **Step 6: `MessageWndProc`'a yeni mesaj işleyicilerini ekle**

`src/App.cpp` `MessageWndProc` içindeki `switch`'e ekle (mevcut `case TrayIcon::kTrayCallbackMsg:` yanına):

```cpp
    case WM_APP_SCROLL_ZOOM:
        if (s_instance)
        {
            // Input thread'den geldi. Konumu simdi okuyoruz — olaydan
            // birkac ms sonra, ayni monitorde.
            POINT pt{};
            GetCursorPos(&pt);
            s_instance->OnScroll(static_cast<int>(static_cast<intptr_t>(wParam)), pt);
        }
        return 0;

    case WM_APP_SETTINGS_CHANGED:
        if (s_instance)
            s_instance->ApplySettings();
        return 0;

    case WM_APP_SET_ZOOM:
        if (s_instance)
        {
            size_t index = 0;
            if (s_instance->ResolveMonitorIndex(wParam, index))
            {
                const float zoom = static_cast<float>(static_cast<int>(lParam)) / 1000.0f;
                s_instance->m_monitorManager.SetZoom(index, zoom);
            }
        }
        return 0;

    case WM_APP_TOGGLE_ZOOM:
        if (s_instance)
            s_instance->OnToggleZoom();
        return 0;

    case WM_APP_TOGGLE_FREEZE:
        if (s_instance)
            s_instance->OnFreeze();
        return 0;
```

- [ ] **Step 7: `ResolveMonitorIndex` ve `ApplySettings`'i implement et**

`src/App.cpp` içine, `OnToggleZoom`'dan önce ekle:

```cpp
// =============================================================================
// ResolveMonitorIndex — wParam'i monitor indeksine cevir
// =============================================================================
// kFocusedMonitor sentinel'i = "farenin uzerinde oldugu monitor".
// Diger degerler dogrudan indeks. Sinir disi indeks false doner.
// =============================================================================
bool App::ResolveMonitorIndex(WPARAM wparam, size_t& outIndex) const
{
    const auto& monitors = m_monitorManager.GetMonitors();

    if (wparam == kFocusedMonitor)
    {
        POINT cursor{};
        GetCursorPos(&cursor);

        for (size_t i = 0; i < monitors.size(); ++i)
        {
            if (PtInRect(&monitors[i].bounds, cursor))
            {
                outIndex = i;
                return true;
            }
        }
        return false;
    }

    if (wparam < monitors.size())
    {
        outIndex = static_cast<size_t>(wparam);
        return true;
    }

    return false;
}

// =============================================================================
// ApplySettings — GUI ayarlari degistirdi, motora uygula
// =============================================================================
// GUI once SettingsStore'u yazdi SONRA bu mesaji postaladi, yani buradaki
// okuma guvenli — yaris yok.
// =============================================================================
void App::ApplySettings()
{
    LOG_INFO("Ayarlar uygulaniyor...");

    const auto& g = m_settings.General();

    // Hotkey'leri yeniden kaydet
    m_hotkeyManager.Reregister(g);

    // Per-monitor zoom sinirlarini uygula
    for (size_t i = 0; i < m_monitorManager.GetMonitorCount(); ++i)
    {
        MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        const auto ms = m_settings.Monitor(mon->deviceName);

        // Mevcut zoom yeni sinirlarin disinda kaldiysa iceri cek
        if (mon->zoom.zoomLevel > ms.maxZoom)
            m_monitorManager.SetZoom(i, ms.maxZoom);
        else if (mon->zoom.zoomLevel < ms.minZoom)
            m_monitorManager.SetZoom(i, ms.minZoom);
    }
}
```

- [ ] **Step 8: `Shutdown`'da input thread'i durdur**

`src/App.cpp` `Shutdown()` içinde, `m_hotkeyManager.Shutdown();` satırından ÖNCE ekle:

```cpp
    // 1a. Input thread'i once durdur — hook'lar kalkmadan mesaj penceresini
    // yikmak, yolda olan bir PostMessage'in olu HWND'ye gitmesi demek.
    m_inputThread.Stop();
```

Ve `Shutdown()` sonuna, `s_instance = nullptr;`'dan ÖNCE ekle:

```cpp
    // Ayarlari kaydet — son zoom seviyeleri dahil
    if (m_settings.General().rememberZoomLevel)
    {
        for (size_t i = 0; i < m_monitorManager.GetMonitorCount(); ++i)
        {
            const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
            if (!mon)
                continue;

            auto ms = m_settings.Monitor(mon->deviceName);
            ms.lastZoom = mon->zoom.zoomLevel;
            m_settings.SetMonitor(mon->deviceName, ms);
        }
    }
    m_settings.Save();
```

- [ ] **Step 9: Zoom açılırken `lastZoom`'u kullan**

`src/App.cpp` `OnToggleZoom()` içindeki sabit `2.0f`'ı ayardan gelen değere çevir. Şu satırı:

```cpp
                m_monitorManager.SetZoom(i, 2.0f);
```

şununla değiştir:

```cpp
                // Zoom acilinca hangi seviyeden baslasin?
                // rememberZoomLevel aciksa son kullanilan seviye, degilse
                // minZoom'un iki kati (1.0x'te acmak anlamsiz).
                const auto ms = m_settings.Monitor(mon->deviceName);
                const float startZoom = m_settings.General().rememberZoomLevel
                    ? std::clamp(ms.lastZoom, ms.minZoom, ms.maxZoom)
                    : std::clamp(ms.minZoom * 2.0f, ms.minZoom, ms.maxZoom);
                m_monitorManager.SetZoom(i, startZoom);
```

- [ ] **Step 10: `OnScroll`'u ayardaki zoom adımına bağla**

`src/App.cpp` `OnScroll()` içindeki şu satırı:

```cpp
            const float step = (delta > 0) ? ZoomState::kZoomStep : -ZoomState::kZoomStep;
```

şununla değiştir:

```cpp
            const auto ms = m_settings.Monitor(monitors[i].deviceName);
            const float step = (delta > 0) ? ms.zoomStep : -ms.zoomStep;
```

- [ ] **Step 11: Yeni dosyaları vcxproj'a ekle ve derle**

`BetterMagnifier.vcxproj` source `ItemGroup`'una `<ClCompile Include="src\InputThread.cpp" />`, header `ItemGroup`'una `<ClInclude Include="src\InputThread.h" />`. Aynısını `.filters`'a.

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Expected: PASS. Olası hatalar:
- `HotkeyManager`'da silinmeyi unutulan `m_onScroll` referansı → linker/compiler hatası, kalan referansı sil
- `App::OnScroll` içinde `monitors` değişkeni tanımlı değilse (`GetMonitors()` çağrısı) — mevcut kodda var, kontrol et

- [ ] **Step 12: Çalıştır ve input thread'in ayrı thread'de olduğunu doğrula**

Doğrulama Döngüsü'ndeki komutu kullan.

Expected log satırları:
```
  Mouse hook aktif (input thread'de)
InputThread baslatildi (thread id: NNNN)
```

**Kritik doğrulama:** `InputThread baslatildi` satırındaki thread id, log'daki `[T:...]` değerinden FARKLI olmalı. Aynıysa hook hâlâ ana thread'de — `Start()` içindeki lambda'nın gerçekten yeni thread'de çalıştığını kontrol et.

Kapanışta:
```
Mouse hook kaldirildi
```
ve exit code 0.

- [ ] **Step 13: Elle davranış testi — scroll zoom hâlâ çalışıyor**

Uygulamayı elle çalıştır (`.\bin\Debug-x64\BetterMagnifier.exe`), `Ctrl+Alt+Z` ile zoom aç, fare tekerleğini çevir, log'a bak:

```bash
powershell -Command "Get-ChildItem .\bin\Debug-x64\logs\*.log | Sort-Object LastWriteTime | Select-Object -Last 1 | Select-String -Pattern 'zoom:'"
```

Expected: `Monitor N zoom: X.XX` satırları, tekerlek yönüne göre artıp azalıyor. Tray'den Exit ile kapat.

- [ ] **Step 14: Commit**

```bash
git add src/InputThread.h src/InputThread.cpp src/HotkeyManager.h src/HotkeyManager.cpp src/App.h src/App.cpp BetterMagnifier.vcxproj BetterMagnifier.vcxproj.filters
git commit -m @'
refactor(input): move low-level hooks to a dedicated thread

* WH_MOUSE_LL no longer shares the thread that blocks on Present(vSync)
* Hook callback only PostMessages WM_APP_SCROLL_ZOOM and returns
* HotkeyManager keeps RegisterHotKey, gains Reregister for settings changes
* Hotkeys, zoom step and start level now come from SettingsStore
* App saves last zoom levels on shutdown
'@
```

---

### Task 5: ControlPanel iskeleti — GUI thread, bootstrapper, XAML island, iki boş sekme

Spec bölüm 3.3, 5.2, 5.4. Task 1'deki spike'ın kanıtladığı deseni üretim koduna taşır.

**Files:**
- Create: `src/ControlPanel.h`
- Create: `src/ControlPanel.cpp`
- Modify: `src/App.h`, `src/App.cpp` (`ControlPanel` üyesi, `WM_APP_SHOW_PANEL` işleyicisi)
- Modify: `src/TrayIcon.h`, `src/TrayIcon.cpp` (menüye "Ayarlar" maddesi)
- Modify: `BetterMagnifier.vcxproj` (PackageReference, C++/WinRT), `BetterMagnifier.vcxproj.filters`

**Interfaces:**
- Consumes: `SettingsStore*` ve `StatusSnapshot*` (App'ten pointer), `HWND` (mesaj penceresi), `AppMessages.h`
- Produces:
  - `class ControlPanel` — `void Show(HWND engineHwnd, SettingsStore* settings, StatusSnapshot* status)`, `void Stop()`, `void NotifyDisplayChange()`, `bool IsRunning() const`
  - `TrayIcon::kMenuSettings` = `1003`

- [ ] **Step 1: vcxproj'a Windows App SDK ve C++/WinRT ekle**

`BetterMagnifier.vcxproj` içine, `<Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />` satırından ÖNCE ekle:

```xml
  <!-- Windows App SDK — WinUI 3 XAML Islands.
       Sadece kontrol paneli icin. Cekirdek magnifier bu runtime olmadan da
       calisir (bootstrapper tembel cagriliyor, bkz. ControlPanel.cpp). -->
  <ItemGroup>
    <PackageReference Include="Microsoft.WindowsAppSDK" Version="PINNED_SDK_VERSION" />
    <PackageReference Include="Microsoft.Windows.CppWinRT" Version="2.0.240111.5" />
  </ItemGroup>
```

`PINNED_SDK_VERSION` yerine Task 1 Step 1'de bulduğun sürümü yaz.

Her dört `<ItemDefinitionGroup>`'un `<ClCompile>` bloğuna ekle (C++/WinRT `co_await` kullanıyor):

```xml
      <AdditionalOptions>/await:strict %(AdditionalOptions)</AdditionalOptions>
```

- [ ] **Step 2: Generated C++/WinRT dosyalarını warning-as-error'dan muaf tut**

`BetterMagnifier.vcxproj` içine, PackageReference `ItemGroup`'undan sonra ekle:

```xml
  <!-- C++/WinRT projeksiyon dosyalari uretilmis kod; Level4 + warning-as-error
       ile derlenmezler. Sadece bu dosyalar icin gevsetiyoruz, kendi kodumuz
       hala uyarisiz derlenmek zorunda. -->
  <ItemDefinitionGroup>
    <ClCompile Condition="'%(Filename)' == 'module'">
      <TreatWarningAsError>false</TreatWarningAsError>
    </ClCompile>
  </ItemDefinitionGroup>
```

- [ ] **Step 3: `src/ControlPanel.h` yaz**

```cpp
#pragma once

// =============================================================================
// ControlPanel.h — WinUI 3 kontrol paneli (kendi STA thread'inde)
// =============================================================================
//
// NEDEN AYRI THREAD:
//   main.cpp ana thread'i MTA yapiyor (COINIT_MULTITHREADED). XAML ise UI
//   thread'inde STA istiyor. Ana thread'i STA'ya cevirmek secenek degil:
//   render loop'u Present(vSync) ile blokluyor, XAML dispatcher'i ac kalir
//   ve panel her frame donar.
//
// NEDEN XAML ISLANDS, standalone WinUI 3 Window degil:
//   WinUI 3'te Application::Start() cagiran thread'in mesaj loop'una SAHIP
//   olur. Bizim thread'lerimizin kendi loop'lari var. Mevcut Win32
//   uygulamasina XAML gommenin yolu DesktopWindowXamlSource.
//
// NEDEN XAML MARKUP DEGIL, KODLA KURULUM:
//   Derlenmis .xaml dosyalari vcxproj'a WinUI XAML compiler target'lari
//   eklemeyi gerektiriyor; Islands senaryosunda duz bir Win32 vcxproj'da
//   sancili yol. Panelde ~15 kontrol var, kodla kurmak yonetilebilir.
//   Fluent gorunumu kaybetmiyoruz: XamlControlsResources yuklenince
//   varsayilan WinUI stilleri devreye giriyor.
//
// THREAD SINIRI:
//   GUI -> motor : PostMessage(m_engineHwnd, WM_APP_*)
//   motor -> GUI : StatusSnapshot atomic okuma, 10 Hz DispatcherTimer ile
//   Baska hicbir paylasilan mutable state YOK.
//
// Python analojisi: tkinter'i ayri bir thread'de calistirmak — ama Win32'de
// bu gercekten dogru cozum, cunku her thread'in kendi mesaj kuyrugu var.
// =============================================================================

#ifndef BETTER_MAGNIFIER_CONTROL_PANEL_H
#define BETTER_MAGNIFIER_CONTROL_PANEL_H

#include <windows.h>
#include <thread>
#include <atomic>

namespace BetterMagnifier {

class SettingsStore;
class StatusSnapshot;

class ControlPanel
{
public:
    ControlPanel() = default;
    ~ControlPanel();

    ControlPanel(const ControlPanel&) = delete;
    ControlPanel& operator=(const ControlPanel&) = delete;

    // Paneli goster. Ilk cagrida GUI thread'i olusturur ve bootstrapper'i
    // calistirir; sonraki cagrilarda mevcut pencereyi one getirir.
    //
    // engineHwnd : GUI'nin PostMessage edecegi mesaj penceresi
    // settings   : GUI yazar, motor WM_APP_SETTINGS_CHANGED ile okur
    // status     : GUI 10 Hz okur, motor her frame yazar
    //
    // Bootstrapper basarisiz olursa (Windows App Runtime kurulu degil)
    // log + MessageBox gosterir ve sessizce doner. Cekirdek etkilenmez.
    void Show(HWND engineHwnd, SettingsStore* settings, StatusSnapshot* status);

    // Monitor listesi degisti — panel acikca kartlari yeniden kur.
    // Thread-safe: GUI thread'e mesaj postalar.
    void NotifyDisplayChange();

    // GUI thread'i durdur ve join et. Idempotent.
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    void ThreadMain();

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    std::atomic<DWORD> m_threadId{0};

    // Bootstrapper bir kez basarisiz olduysa tekrar denemeyip her seferinde
    // MessageBox gostermemek icin.
    std::atomic<bool>  m_bootstrapFailed{false};

    HWND            m_engineHwnd = nullptr;
    SettingsStore*  m_settings   = nullptr;
    StatusSnapshot* m_status     = nullptr;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_CONTROL_PANEL_H
```

- [ ] **Step 4: `src/ControlPanel.cpp` — thread iskeleti, bootstrapper, host pencere, island**

```cpp
// =============================================================================
// ControlPanel.cpp
// =============================================================================

#include "pch.h"
#include "ControlPanel.h"
#include "AppMessages.h"
#include "SettingsStore.h"
#include "StatusSnapshot.h"
#include "Logger.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <MddBootstrap.h>
#include <dwmapi.h>

#include <future>

namespace BetterMagnifier {

namespace {

// Host pencere sinifi ve boyutu
constexpr wchar_t kHostClassName[] = L"BetterMagnifierPanelHost";
constexpr int kPanelWidth  = 520;
constexpr int kPanelHeight = 640;

// GUI thread'e ozel mesajlar (thread-only, pencereye ait degil)
constexpr UINT kMsgShowPanel     = WM_APP + 100;
constexpr UINT kMsgDisplayChange = WM_APP + 101;

// Bootstrapper icin minimum surum: 1.5 (0x0001'0005).
// Spec 5.3: DispatcherShutdownMode davranisi 1.5'te geldi, ona bel bagliyoruz.
constexpr UINT32 kMinSdkMajorMinor = 0x00010005;

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CLOSE:
        // Pencereyi YIKMIYORUZ, sadece gizliyoruz.
        // Panel tekrar acilinca ayni pencere one gelir — XAML agacini
        // yeniden kurmaya gerek kalmaz.
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_GETMINMAXINFO:
    {
        // Minimum boyut — kartlar sikismasin
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 420;
        mmi->ptMinTrackSize.y = 480;
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Koyu baslik cubugu. Windows 11'de belgelenmis, Windows 10 2004+'ta calisiyor.
// Basarisiz olursa baslik cubugu acik kalir — kozmetik, kritik degil.
void ApplyDarkTitleBar(HWND hwnd)
{
    BOOL dark = TRUE;
    const HRESULT hr = DwmSetWindowAttribute(
        hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    if (FAILED(hr))
        LOG_DEBUG("Koyu baslik cubugu ayarlanamadi: 0x{:08X}", static_cast<unsigned long>(hr));
}

} // anonymous namespace

ControlPanel::~ControlPanel()
{
    Stop();
}

// =============================================================================
// Show — tembel baslatma
// =============================================================================
void ControlPanel::Show(HWND engineHwnd, SettingsStore* settings, StatusSnapshot* status)
{
    if (m_bootstrapFailed.load(std::memory_order_acquire))
    {
        LOG_WARN("Kontrol paneli daha once baslatilamadi, tekrar denenmiyor");
        return;
    }

    // Thread zaten ayaktaysa sadece pencereyi one getir
    if (m_running.load(std::memory_order_acquire))
    {
        const DWORD tid = m_threadId.load(std::memory_order_acquire);
        if (tid != 0)
            PostThreadMessageW(tid, kMsgShowPanel, 0, 0);
        return;
    }

    m_engineHwnd = engineHwnd;
    m_settings   = settings;
    m_status     = status;

    std::promise<bool> ready;
    std::future<bool> readyFuture = ready.get_future();

    m_thread = std::thread([this, p = std::move(ready)]() mutable {
        m_threadId.store(GetCurrentThreadId(), std::memory_order_release);

        bool ok = false;
        try
        {
            // ── 1. STA apartment — XAML sart kosuyor ──
            winrt::init_apartment(winrt::apartment_type::single_threaded);

            // ── 2. Bootstrapper ──
            // Paketlenmemis uygulamada, HERHANGI bir WinRT/XAML tipine
            // dokunmadan ONCE cagrilmali.
            const HRESULT hr = MddBootstrapInitialize(kMinSdkMajorMinor, nullptr, PACKAGE_VERSION{});
            if (FAILED(hr))
            {
                LOG_ERROR("MddBootstrapInitialize basarisiz: 0x{:08X} — "
                          "Windows App Runtime kurulu degil olabilir",
                          static_cast<unsigned long>(hr));
                p.set_value(false);
                return;
            }

            ok = true;
            p.set_value(true);
        }
        catch (winrt::hresult_error const& e)
        {
            LOG_ERROR("GUI thread baslatilamadi: 0x{:08X}",
                static_cast<unsigned long>(e.code().value));
            p.set_value(false);
            return;
        }

        if (!ok)
            return;

        m_running.store(true, std::memory_order_release);

        try
        {
            ThreadMain();
        }
        catch (winrt::hresult_error const& e)
        {
            LOG_ERROR("GUI thread coktu: 0x{:08X} — cekirdek etkilenmedi",
                static_cast<unsigned long>(e.code().value));
        }

        m_running.store(false, std::memory_order_release);
        MddBootstrapShutdown();
    });

    if (!readyFuture.get())
    {
        m_bootstrapFailed.store(true, std::memory_order_release);

        if (m_thread.joinable())
            m_thread.join();

        // Cekirdek CALISMAYA DEVAM EDIYOR — bu sadece panel.
        MessageBoxW(nullptr,
            L"Ayar paneli açılamadı.\n\n"
            L"Kontrol paneli için Windows App Runtime gerekli. "
            L"Büyütme, kısayol tuşları ve tepsi menüsü çalışmaya devam ediyor.\n\n"
            L"Detaylar logs klasöründe.",
            L"BetterMagnifier", MB_ICONWARNING | MB_OK);
        return;
    }

    LOG_INFO("Kontrol paneli GUI thread'i baslatildi (thread id: {})",
        m_threadId.load(std::memory_order_acquire));
}

// =============================================================================
// ThreadMain — XAML runtime, host pencere, island, mesaj loop
// =============================================================================
void ControlPanel::ThreadMain()
{
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using namespace winrt::Microsoft::UI::Xaml::Hosting;
    using namespace winrt::Microsoft::UI::Dispatching;

    // ── DispatcherQueue ──
    // XAML runtime bu thread'de bir dispatcher bekliyor.
    // DispatcherTimer (Task 6, 10 Hz canli okuma) da buna bagli.
    auto dispatcherController = DispatcherQueueController::CreateOnCurrentThread();

    // ── XAML runtime ──
    // Application::Start() CAGIRMIYORUZ — o mesaj loop'unu ele gecirir.
    // Islands senaryosunda bos bir Application ornegi kurup
    // DispatcherShutdownMode'u OnExplicitShutdown yapiyoruz: son XAML
    // penceresi kapanınca thread'in event loop'u CIKMASIN (spec 3.3).
    Application app{};
    app.DispatcherShutdownMode(DispatcherShutdownMode::OnExplicitShutdown);
    app.Resources(XamlControlsResources{});

    // ── Host Win32 penceresi ──
    // DesktopWindowXamlSource mevcut bir HWND istiyor.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = HostWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kHostClassName;
    RegisterClassExW(&wc);

    // WS_MAXIMIZEBOX YOK — ayar penceresi tam ekran olmayacak.
    const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX);

    HWND host = CreateWindowExW(
        0, kHostClassName, L"BetterMagnifier",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, kPanelWidth, kPanelHeight,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!host)
    {
        LOG_ERROR("Panel host penceresi olusturulamadi: {}", GetLastError());
        return;
    }

    ApplyDarkTitleBar(host);

    // ── XAML island'i host'a bagla ──
    DesktopWindowXamlSource source{};
    source.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(host));
    source.SiteBridge().ResizePolicy(
        winrt::Microsoft::UI::Content::ContentSizePolicy::ResizeContentToParentWindow);

    // ── Iki sekmeli iskelet ──
    // Icerikleri Task 6 ve Task 7'de dolduruyoruz.
    NavigationView nav{};   // kullanilmiyor; TabView daha uygun
    TabView tabs{};
    tabs.IsAddTabButtonVisible(false);
    tabs.CanReorderTabs(false);
    tabs.CanDragTabs(false);

    TabViewItem statusTab{};
    statusTab.Header(winrt::box_value(L"Durum"));
    statusTab.IsClosable(false);
    {
        ScrollViewer sv{};
        StackPanel panel{};
        panel.Padding(ThicknessHelper::FromUniformLength(16));
        panel.Spacing(12);
        sv.Content(panel);
        statusTab.Content(sv);
        // Task 6 bu StackPanel'i monitor kartlariyla dolduracak.
    }

    TabViewItem settingsTab{};
    settingsTab.Header(winrt::box_value(L"Ayarlar"));
    settingsTab.IsClosable(false);
    {
        ScrollViewer sv{};
        StackPanel panel{};
        panel.Padding(ThicknessHelper::FromUniformLength(16));
        panel.Spacing(12);
        sv.Content(panel);
        settingsTab.Content(sv);
        // Task 7 bu StackPanel'i ayar kontrolleriyle dolduracak.
    }

    tabs.TabItems().Append(statusTab);
    tabs.TabItems().Append(settingsTab);

    Grid root{};
    root.Children().Append(tabs);
    source.Content(root);
    source.SiteBridge().Show();

    ShowWindow(host, SW_SHOW);
    SetForegroundWindow(host);

    LOG_INFO("Kontrol paneli acildi");

    // ── Mesaj loop'u ──
    // Bu thread'in kendi loop'u. Hem host pencerenin mesajlari hem de
    // thread-only mesajlarimiz (kMsgShowPanel, kMsgDisplayChange) buradan gecer.
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.hwnd == nullptr)
        {
            // Thread-only mesaj — DispatchMessage bunlari bir pencereye
            // yonlendiremez, elle isliyoruz.
            if (msg.message == kMsgShowPanel)
            {
                ShowWindow(host, SW_SHOW);
                SetForegroundWindow(host);
                continue;
            }
            if (msg.message == kMsgDisplayChange)
            {
                // Task 6 kartlari burada yeniden kuracak.
                LOG_DEBUG("Panel: display degisikligi bildirimi alindi");
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LOG_INFO("Kontrol paneli kapaniyor");

    source.Close();
    DestroyWindow(host);
    UnregisterClassW(kHostClassName, wc.hInstance);
    dispatcherController.ShutdownQueueAsync();
}

// =============================================================================
// NotifyDisplayChange
// =============================================================================
void ControlPanel::NotifyDisplayChange()
{
    const DWORD tid = m_threadId.load(std::memory_order_acquire);
    if (m_running.load(std::memory_order_acquire) && tid != 0)
        PostThreadMessageW(tid, kMsgDisplayChange, 0, 0);
}

// =============================================================================
// Stop
// =============================================================================
void ControlPanel::Stop()
{
    const DWORD tid = m_threadId.load(std::memory_order_acquire);

    if (tid != 0)
        PostThreadMessageW(tid, WM_QUIT, 0, 0);

    if (m_thread.joinable())
        m_thread.join();

    m_threadId.store(0, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
}

} // namespace BetterMagnifier
```

`NavigationView nav{};` satırını sil — yorumu da beraber. Yanlışlıkla bırakılmış ölü değişken, `TreatWarningAsError` ile C4101 (unreferenced local variable) hatasına düşer.

`dwmapi.lib` linkine eklenmeli: `BetterMagnifier.vcxproj` içindeki dört `<AdditionalDependencies>` satırına `dwmapi.lib;` ekle.

- [ ] **Step 5: Tray menüsüne "Ayarlar" ekle**

`src/TrayIcon.h` menü ID'lerine ekle:

```cpp
    static constexpr UINT kMenuSettings = 1003;
```

ve callback setter'ı:

```cpp
    void SetSettingsCallback(std::function<void()> cb) { m_onSettings = std::move(cb); }
```

ve üye:

```cpp
    std::function<void()> m_onSettings;
```

`src/TrayIcon.cpp` `ShowContextMenu()` içinde menü kurulumunu şuna çevir:

```cpp
    AppendMenuW(hMenu, MF_STRING, kMenuToggle,   L"Toggle Zoom (Ctrl+Alt+Z)");
    AppendMenuW(hMenu, MF_STRING, kMenuSettings, L"Ayarlar...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, kMenuExit,     L"Exit");
```

ve `switch (cmd)` bloğuna ekle:

```cpp
    case kMenuSettings:
        if (m_onSettings) m_onSettings();
        break;
```

`HandleMessage()` içindeki `WM_LBUTTONDBLCLK` davranışını koru (toggle zoom).

- [ ] **Step 6: `App`'e `ControlPanel` bağla**

`src/App.h`:
- `#include "ControlPanel.h"` ekle
- `private:` üyelere ekle: `ControlPanel m_controlPanel;`
- `private:` metodlara ekle: `void OnShowPanel();`

`src/App.cpp` `SetupCallbacks()` içine ekle:

```cpp
    m_trayIcon.SetSettingsCallback([this] { OnShowPanel(); });
```

`src/App.cpp` içine, `OnDisplayChange`'den önce ekle:

```cpp
// =============================================================================
// OnShowPanel — kontrol panelini goster
// =============================================================================
// Panel kendi STA thread'inde yasiyor. Ilk cagrida thread + bootstrapper
// kurulur; Windows App Runtime yoksa panel acilmaz ama biz calismaya
// devam ederiz (spec 5.4).
// =============================================================================
void App::OnShowPanel()
{
    m_controlPanel.Show(m_messageHwnd, &m_settings, &m_status);
}
```

`MessageWndProc`'a ekle:

```cpp
    case WM_APP_SHOW_PANEL:
        if (s_instance)
            s_instance->OnShowPanel();
        return 0;
```

`OnDisplayChange()` sonuna ekle:

```cpp
    // Panel acikca kartlari yeniden kurmasi icin haber ver
    m_controlPanel.NotifyDisplayChange();
```

`Shutdown()` içinde, `m_inputThread.Stop();` satırının yanına (ondan ÖNCE) ekle:

```cpp
    // 1. GUI thread'i once durdur — panel motor pointer'larini tutuyor,
    // onlar gecersizlesmeden once thread'in gitmesi lazim.
    m_controlPanel.Stop();
```

- [ ] **Step 7: Derle (restore ile)**

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /restore /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Expected: NuGet restore çalışır, C++/WinRT projeksiyonları üretilir, derleme PASS.

Olası hatalar ve çözümleri:
- `MddBootstrap.h` bulunamıyor → PackageReference restore edilmemiş, `/restore` bayrağını kontrol et
- `winrt/Microsoft.UI.Content.h` bulunamıyor → C++/WinRT projeksiyon üretimi bu namespace'i kapsamıyor; `<CppWinRTNamespaceMergeDepth>` gerekebilir veya include yolunu SDK sürümüne göre düzelt
- `ThicknessHelper` bulunamıyor → `winrt/Microsoft.UI.Xaml.h` yeterli olmalı; değilse `#include <winrt/Microsoft.UI.Xaml.Media.h>` ekli mi kontrol et
- Level4 uyarısı generated dosyada → Step 2'deki muafiyet koşulunu genişlet

- [ ] **Step 8: Çalıştır — panel açılıyor mu**

Uygulamayı elle çalıştır:

```bash
.\bin\Debug-x64\BetterMagnifier.exe
```

Tray ikonuna sağ tık → "Ayarlar...".

Expected (Windows App Runtime kurulu): koyu başlık çubuklu, iki sekmeli ("Durum", "Ayarlar") boş bir pencere açılır. Log:
```
Kontrol paneli GUI thread'i baslatildi (thread id: NNNN)
Kontrol paneli acildi
```

Expected (runtime kurulu değil): MessageBox "Ayar paneli açılamadı..." çıkar, uygulama çalışmaya devam eder, `Ctrl+Alt+Z` hâlâ çalışır. Log:
```
MddBootstrapInitialize basarisiz: 0x... — Windows App Runtime kurulu degil olabilir
```

**Bu ikinci senaryo tasarımın sert kuralının testi** (spec 5.4). Runtime kuruluysa elle test etmek için `MddBootstrapInitialize`'a geçici olarak `0x00090009` gibi olmayan bir sürüm ver, davranışı gör, sonra geri al.

- [ ] **Step 9: Panel kapat-aç ve temiz kapanışı doğrula**

Paneli ✕ ile kapat, tray'den tekrar "Ayarlar..." aç.

Expected: aynı pencere yeniden görünür, log'da ikinci bir "Kontrol paneli acildi" satırı YOK (thread yaşıyor, pencere sadece gizlenip gösterildi). Spec 3.3'teki `DispatcherShutdownMode` davranışının kanıtı.

Sonra tray → Exit.

Expected log:
```
Kontrol paneli kapaniyor
App kapatiliyor...
...
App kapatildi
```
exit code 0, assertion/crash yok.

- [ ] **Step 10: Commit**

```bash
git add src/ControlPanel.h src/ControlPanel.cpp src/App.h src/App.cpp src/TrayIcon.h src/TrayIcon.cpp BetterMagnifier.vcxproj BetterMagnifier.vcxproj.filters
git commit -m @'
feat(gui): add WinUI 3 control panel shell

* ControlPanel owns its own STA thread with a DispatcherQueue
* DesktopWindowXamlSource hosts XAML in a Win32 host window
* Bootstrapper runs lazily; core keeps working without Windows App Runtime
* Two empty tabs, dark title bar, closing hides instead of destroying
* Tray context menu gains a Settings entry
'@
```

---

### Task 6: Durum sekmesi — monitör kartları, canlı okuma, zoom slider

Spec bölüm 6 "Durum sekmesi". `StatusSnapshot`'ı görünür kılar.

**Files:**
- Modify: `src/ControlPanel.h` (kart state'i, yardımcı metodlar)
- Modify: `src/ControlPanel.cpp` (kart kurulumu, 10 Hz timer, slider olayları)

**Interfaces:**
- Consumes: `StatusSnapshot::Monitor(i)`, `SettingsStore::Monitor(deviceName)`, `WM_APP_SET_ZOOM`, `WM_APP_TOGGLE_ZOOM`, `WM_APP_TOGGLE_FREEZE`
- Produces: `ControlPanel` içinde `struct MonitorCard` (private) — sonraki task'lar bunun alanlarını kullanmaz, kapsüllü kalır

- [ ] **Step 1: Monitör bilgisini GUI thread'e taşıyacak yolu ekle**

Kartların başlığı için statik bilgi lazım (cihaz adı, çözünürlük, Hz, DPI, primary). Bunlar `MonitorInfo`'da ama o render thread'in sahipliğinde. `StatusSnapshot`'a statik alanları ekliyoruz — bir kez yazılıp okunuyorlar, atomic gerektirmeyecek kadar nadir değişiyorlar ama tutarlılık için yine atomic tutuyoruz.

`src/StatusSnapshot.h` `MonitorStatus`'a ekle:

```cpp
    // ── Statik bilgi (WM_DISPLAYCHANGE'de guncellenir) ──
    // Panel basliginda gosterilir. wchar_t dizisi cunku std::wstring
    // atomic olamaz; sabit boyut yeterli ("\\\\.\\DISPLAY1" gibi kisa).
    static constexpr size_t kNameCapacity = 64;
    wchar_t              deviceName[kNameCapacity]{};
    std::atomic<int>     width{0};
    std::atomic<int>     height{0};
    std::atomic<int>     refreshRate{0};
    std::atomic<int>     dpiPercent{100};
    std::atomic<bool>    isPrimary{false};
```

`deviceName` atomic değil — sadece `WM_DISPLAYCHANGE` anında render thread yazıyor, GUI okuyor. Yarış teorik olarak var ama sonucu bozuk bir etiket, çökme değil. Bunu yorumda açıkça belirt:

```cpp
    // deviceName atomic DEGIL: sadece WM_DISPLAYCHANGE aninda yazilir.
    // Kotu senaryoda GUI yarim yazilmis bir isim okur — etiket bozuk gorunur,
    // bir sonraki 10 Hz tick'te duzelir. Cokme riski yok (sabit boyutlu dizi,
    // her zaman null-terminated yazilir).
```

`src/App.cpp` `InitializeComponents()` sonuna ve `OnDisplayChange()` sonuna çağrılacak bir yardımcı ekle. `App.h` `private:` metodlarına:

```cpp
    // Snapshot'in statik monitor bilgilerini doldur (init ve display change'de)
    void PublishMonitorInfo();
```

`src/App.cpp`:

```cpp
// =============================================================================
// PublishMonitorInfo — statik monitor bilgilerini snapshot'a yaz
// =============================================================================
// Panel bunlari basliklarda gosteriyor. Sadece init ve WM_DISPLAYCHANGE'de
// cagriliyor — her frame degil.
// =============================================================================
void App::PublishMonitorInfo()
{
    const size_t count = m_monitorManager.GetMonitorCount();
    m_status.monitorCount.store(count, std::memory_order_release);

    for (size_t i = 0; i < count && i < StatusSnapshot::kMaxMonitors; ++i)
    {
        const MonitorInfo* mon = m_monitorManager.GetMonitor(i);
        if (!mon)
            continue;

        auto& st = m_status.Monitor(i);

        // wcsncpy_s ile guvenli kopya — her zaman null-terminated
        wcsncpy_s(st.deviceName, MonitorStatus::kNameCapacity,
                  mon->deviceName.c_str(), _TRUNCATE);

        st.width.store(mon->Width(), std::memory_order_relaxed);
        st.height.store(mon->Height(), std::memory_order_relaxed);
        st.refreshRate.store(static_cast<int>(mon->refreshRate), std::memory_order_relaxed);
        st.dpiPercent.store(static_cast<int>(mon->ScaleFactor() * 100.0f), std::memory_order_relaxed);
        st.isPrimary.store(mon->isPrimary, std::memory_order_relaxed);
    }
}
```

`InitializeComponents()` sonunda `return true;`'dan önce çağır: `PublishMonitorInfo();`
`OnDisplayChange()` içinde, `m_controlPanel.NotifyDisplayChange();`'den ÖNCE çağır: `PublishMonitorInfo();`

- [ ] **Step 2: `ControlPanel.h`'ye kart state'i ekle**

`ControlPanel.h` `private:` bölümüne, `ThreadMain()` bildiriminin yanına ekle. XAML tiplerini header'a sokmamak için pimpl benzeri bir yaklaşım kullanıyoruz — kart yapısı `.cpp`'de tanımlı, header'da sadece opak bir işaretçi:

```cpp
    // XAML tipleri header'a sizmasin — ControlPanel.h'yi include eden
    // App.h butun WinUI projeksiyonlarini cekmek zorunda kalmasin.
    // (Derleme suresi ve bagimlilik hijyeni.)
    struct Impl;
    std::unique_ptr<Impl> m_impl;
```

`#include <memory>` ekle. Destructor `.cpp`'de tanımlı olduğu için `unique_ptr<Impl>` inkomplet tipe sahip olabilir.

- [ ] **Step 3: `Impl` ve `MonitorCard` yapılarını `.cpp`'de tanımla**

`src/ControlPanel.cpp` içinde, `namespace BetterMagnifier {` sonrasına ekle:

```cpp
// =============================================================================
// Impl — XAML nesnelerini tutan iç yapı
// =============================================================================
// Neden header'da degil: WinUI projeksiyonlarini ControlPanel.h'ye sokmak,
// onu include eden App.h'yi ve dolayisiyla butun projeyi WinUI'ye bagimli
// yapardi. Derleme suresi ve bagimlilik hijyeni.
//
// Python analojisi: __slots__ ile ic detayi sakli tutmak — ama burada amac
// bellek degil, derleme bagimliligi.
// =============================================================================
struct MonitorCard
{
    winrt::Microsoft::UI::Xaml::Controls::TextBlock     title{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock     details{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch  activeToggle{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Slider        zoomSlider{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock     zoomLabel{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock     fpsLabel{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock     captureLabel{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ToggleButton  freezeButton{nullptr};

    // Slider'i programatik guncellerken ValueChanged olayinin geri tepmesini
    // onlemek icin. Yoksa: timer slider'i set eder -> olay tetiklenir ->
    // motora zoom mesaji gider -> sonsuz dongu.
    bool suppressSliderEvent = false;
};

struct ControlPanel::Impl
{
    HWND hostWindow = nullptr;
    winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource source{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel statusPanel{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel settingsPanel{nullptr};
    winrt::Microsoft::UI::Xaml::DispatcherTimer      liveTimer{nullptr};

    std::vector<MonitorCard> cards;
};
```

`#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>` zaten var (ToggleButton için gerekli). `<vector>` pch'de var.

`ControlPanel::~ControlPanel()`'ı ve constructor'ı düzelt — `Impl` inkomplet tipten tam tipe geçtiği için destructor `.cpp`'de olmalı (zaten öyle):

```cpp
ControlPanel::~ControlPanel()
{
    Stop();
    // m_impl burada yikiliyor — Impl artik tam tip.
}
```

`Show()` içinde thread başlatmadan önce `m_impl = std::make_unique<Impl>();` satırını ekle.

- [ ] **Step 4: Kart kurulum fonksiyonunu yaz**

`src/ControlPanel.cpp` anonim namespace'ine ekle:

```cpp
// Tek monitor icin kart olustur. Olay baglantilari cagiran tarafta yapiliyor
// cunku engineHwnd ve indeks lazim.
MonitorCard BuildMonitorCard(
    winrt::Microsoft::UI::Xaml::Controls::StackPanel const& parent,
    float minZoom, float maxZoom, float zoomStep)
{
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;

    MonitorCard card;

    // Kart govdesi — Fluent'in kart yuzeyi
    Border border{};
    border.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
    border.Padding(ThicknessHelper::FromUniformLength(12));
    border.BorderThickness(ThicknessHelper::FromUniformLength(1));

    StackPanel body{};
    body.Spacing(6);

    // Baslik: cihaz adi + PRIMARY etiketi
    card.title = TextBlock{};
    card.title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    body.Children().Append(card.title);

    // Detay: cozunurluk, Hz, DPI
    card.details = TextBlock{};
    card.details.Opacity(0.7);
    card.details.FontSize(12.0);
    body.Children().Append(card.details);

    // Kontrol satiri: toggle + slider + zoom etiketi
    Grid controls{};
    {
        ColumnDefinition c0{}; c0.Width(GridLengthHelper::Auto());
        ColumnDefinition c1{}; c1.Width(GridLengthHelper::FromValueAndType(1.0, GridUnitType::Star));
        ColumnDefinition c2{}; c2.Width(GridLengthHelper::Auto());
        controls.ColumnDefinitions().Append(c0);
        controls.ColumnDefinitions().Append(c1);
        controls.ColumnDefinitions().Append(c2);
        controls.ColumnSpacing(8);
    }

    card.activeToggle = ToggleSwitch{};
    card.activeToggle.OnContent(winrt::box_value(L"Aktif"));
    card.activeToggle.OffContent(winrt::box_value(L"Pasif"));
    Grid::SetColumn(card.activeToggle, 0);
    controls.Children().Append(card.activeToggle);

    card.zoomSlider = Slider{};
    card.zoomSlider.Minimum(minZoom);
    card.zoomSlider.Maximum(maxZoom);
    card.zoomSlider.StepFrequency(zoomStep);
    card.zoomSlider.Value(minZoom);
    // Slider'in kendi degeri gostermesini kapatiyoruz — kendi etiketimiz var
    card.zoomSlider.ThumbToolTipValueConverter(nullptr);
    Grid::SetColumn(card.zoomSlider, 1);
    controls.Children().Append(card.zoomSlider);

    card.zoomLabel = TextBlock{};
    card.zoomLabel.MinWidth(52.0);
    card.zoomLabel.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(card.zoomLabel, 2);
    controls.Children().Append(card.zoomLabel);

    body.Children().Append(controls);

    // Alt satir: capture durumu + FPS + freeze
    Grid footer{};
    {
        ColumnDefinition f0{}; f0.Width(GridLengthHelper::FromValueAndType(1.0, GridUnitType::Star));
        ColumnDefinition f1{}; f1.Width(GridLengthHelper::Auto());
        ColumnDefinition f2{}; f2.Width(GridLengthHelper::Auto());
        footer.ColumnDefinitions().Append(f0);
        footer.ColumnDefinitions().Append(f1);
        footer.ColumnDefinitions().Append(f2);
        footer.ColumnSpacing(8);
    }

    card.captureLabel = TextBlock{};
    card.captureLabel.FontSize(12.0);
    card.captureLabel.Opacity(0.7);
    card.captureLabel.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(card.captureLabel, 0);
    footer.Children().Append(card.captureLabel);

    card.fpsLabel = TextBlock{};
    card.fpsLabel.FontSize(12.0);
    card.fpsLabel.Opacity(0.7);
    card.fpsLabel.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(card.fpsLabel, 1);
    footer.Children().Append(card.fpsLabel);

    card.freezeButton = ToggleButton{};
    card.freezeButton.Content(winrt::box_value(L"Freeze"));
    Grid::SetColumn(card.freezeButton, 2);
    footer.Children().Append(card.freezeButton);

    body.Children().Append(footer);

    border.Child(body);
    parent.Children().Append(border);

    return card;
}
```

`#include <winrt/Windows.UI.Text.h>` ekle (`FontWeights` için).

- [ ] **Step 5: Kartları kur ve olayları bağla**

`ThreadMain()` içinde, `statusTab`'ın `StackPanel`'ini `m_impl->statusPanel`'e kaydet, sonra kartları kur. `statusTab` bloğunu şuna çevir:

```cpp
    TabViewItem statusTab{};
    statusTab.Header(winrt::box_value(L"Durum"));
    statusTab.IsClosable(false);
    {
        ScrollViewer sv{};
        m_impl->statusPanel = StackPanel{};
        m_impl->statusPanel.Padding(ThicknessHelper::FromUniformLength(16));
        m_impl->statusPanel.Spacing(12);
        sv.Content(m_impl->statusPanel);
        statusTab.Content(sv);
    }
```

`ShowWindow(host, SW_SHOW);` çağrısından ÖNCE ekle:

```cpp
    m_impl->hostWindow = host;
    m_impl->source     = source;

    RebuildMonitorCards();
    StartLiveTimer();
```

`ControlPanel.h` `private:` metodlarına ekle:

```cpp
    // Snapshot'taki monitor sayisina gore kartlari sifirdan kur.
    // SADECE GUI thread'den cagrilir.
    void RebuildMonitorCards();

    // 10 Hz canli okuma timer'ini baslat. SADECE GUI thread'den.
    void StartLiveTimer();

    // Timer tick'i — snapshot'i oku, kartlari guncelle.
    void UpdateLiveValues();
```

`src/ControlPanel.cpp`:

```cpp
// =============================================================================
// RebuildMonitorCards — monitor sayisi degisince kartlari sifirdan kur
// =============================================================================
void ControlPanel::RebuildMonitorCards()
{
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;

    if (!m_impl || !m_impl->statusPanel || !m_status)
        return;

    m_impl->statusPanel.Children().Clear();
    m_impl->cards.clear();

    const size_t count = m_status->monitorCount.load(std::memory_order_acquire);

    if (count == 0)
    {
        TextBlock empty{};
        empty.Text(L"Monitör bulunamadı.");
        m_impl->statusPanel.Children().Append(empty);
        return;
    }

    m_impl->cards.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        const auto& st = m_status->Monitor(i);

        // Zoom sinirlarini ayarlardan al — slider aralligi buna gore
        const std::wstring device(st.deviceName);
        const auto ms = m_settings ? m_settings->Monitor(device) : MonitorSettings{};

        MonitorCard card = BuildMonitorCard(m_impl->statusPanel,
                                           ms.minZoom, ms.maxZoom, ms.zoomStep);

        // ── Olay baglantilari ──
        // Lambda'lar indeksi DEGERLE yakaliyor. engineHwnd de degerle.
        // Motora giden her sey PostMessage — GUI thread motor state'ine
        // dogrudan asla dokunmuyor.
        const HWND engine = m_engineHwnd;
        const size_t index = i;

        card.activeToggle.Toggled(
            [engine, index](winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
            {
                PostMessageW(engine, WM_APP_TOGGLE_ZOOM, static_cast<WPARAM>(index), 0);
            });

        card.freezeButton.Click(
            [engine, index](winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
            {
                PostMessageW(engine, WM_APP_TOGGLE_FREEZE, static_cast<WPARAM>(index), 0);
            });

        // Slider: geri tepmeyi onlemek icin suppress bayragini kontrol et.
        // cards vector'u reserve edildi ama yine de pointer degil INDEKS
        // yakaliyoruz — vector buyurse pointer gecersizlesir, indeks kalir.
        card.zoomSlider.ValueChanged(
            [this, engine, index](winrt::Windows::Foundation::IInspectable const&,
                                  RangeBaseValueChangedEventArgs const& args)
            {
                if (index >= m_impl->cards.size())
                    return;
                if (m_impl->cards[index].suppressSliderEvent)
                    return;

                const int scaled = static_cast<int>(args.NewValue() * 1000.0);
                PostMessageW(engine, WM_APP_SET_ZOOM,
                             static_cast<WPARAM>(index),
                             static_cast<LPARAM>(scaled));
            });

        m_impl->cards.push_back(card);
    }

    // Statik etiketleri hemen doldur — timer'in ilk tick'ini beklemesin
    UpdateLiveValues();
}

// =============================================================================
// StartLiveTimer — 10 Hz canli okuma
// =============================================================================
// Neden 10 Hz, 60 Hz degil?
//   Ayar panelinde 60 Hz gostergeye kimse bakmiyor. 10 Hz'de snapshot
//   okuma cekismesi sifira iner, insan gozu farki gormez.
//
// DispatcherTimer XAML thread'inde calisir — UI'ye dokunmak guvenli.
// Python analojisi: tkinter'da root.after(100, callback) dongusu.
// =============================================================================
void ControlPanel::StartLiveTimer()
{
    using namespace winrt::Microsoft::UI::Xaml;

    if (!m_impl)
        return;

    m_impl->liveTimer = DispatcherTimer{};
    m_impl->liveTimer.Interval(std::chrono::milliseconds(100));   // 10 Hz

    m_impl->liveTimer.Tick(
        [this](winrt::Windows::Foundation::IInspectable const&,
               winrt::Windows::Foundation::IInspectable const&)
        {
            UpdateLiveValues();
        });

    m_impl->liveTimer.Start();
}

// =============================================================================
// UpdateLiveValues — snapshot'i oku, kartlara yaz
// =============================================================================
void ControlPanel::UpdateLiveValues()
{
    if (!m_impl || !m_status)
        return;

    // Pencere gizliyse guncelleme yapma — bosa CPU.
    if (m_impl->hostWindow && !IsWindowVisible(m_impl->hostWindow))
        return;

    const size_t count = m_status->monitorCount.load(std::memory_order_acquire);

    // Monitor sayisi kartlardan farkliysa kartlari yeniden kurmak lazim,
    // ama bunu timer icinde yapmak reentrancy riski — sadece logla,
    // WM_DISPLAYCHANGE zaten NotifyDisplayChange ile geliyor.
    const size_t n = (count < m_impl->cards.size()) ? count : m_impl->cards.size();

    for (size_t i = 0; i < n; ++i)
    {
        const auto& st = m_status->Monitor(i);
        auto& card = m_impl->cards[i];

        // ── Baslik ──
        std::wstring title(st.deviceName);
        if (st.isPrimary.load(std::memory_order_relaxed))
            title += L"   PRIMARY";
        card.title.Text(title);

        // ── Detay ──
        card.details.Text(std::format(L"{}×{} · {}Hz · %{}",
            st.width.load(std::memory_order_relaxed),
            st.height.load(std::memory_order_relaxed),
            st.refreshRate.load(std::memory_order_relaxed),
            st.dpiPercent.load(std::memory_order_relaxed)));

        // ── Aktiflik ──
        const bool active = st.isActive.load(std::memory_order_relaxed);
        if (card.activeToggle.IsOn() != active)
        {
            // ToggleSwitch::IsOn set etmek Toggled olayini tetikler —
            // ama biz zaten motorun mevcut durumunu yansitiyoruz, geri
            // gondermek zoom'u kapatir. Olayi gecici olarak sokuyoruz.
            //
            // ponytail: suppress bayragi yerine olayi cikar-tak yapmak daha
            // temiz olurdu ama WinRT event token yonetimi burada gurultu
            // yaratiyor; slider'daki ayni deseni kullaniyoruz.
            card.suppressSliderEvent = true;
            card.activeToggle.IsOn(active);
            card.suppressSliderEvent = false;
        }

        // ── Zoom ──
        const float zoom = st.zoomLevel.load(std::memory_order_relaxed);
        card.zoomLabel.Text(std::format(L"{:.2f}×", zoom));

        // Slider'i motorun degerine senkronla, ama kullanici suruklerken
        // ustune yazmayalim: fark esigi 0.005'ten kucukse dokunma.
        const double sliderValue = card.zoomSlider.Value();
        if (std::abs(sliderValue - static_cast<double>(zoom)) > 0.005)
        {
            card.suppressSliderEvent = true;
            card.zoomSlider.Value(static_cast<double>(zoom));
            card.suppressSliderEvent = false;
        }

        // ── FPS ──
        const float fps = st.fps.load(std::memory_order_relaxed);
        card.fpsLabel.Text(fps > 0.0f ? std::format(L"{:.0f} FPS", fps)
                                      : std::wstring(L"— FPS"));

        // ── Capture durumu ──
        const bool captureOk = st.captureOk.load(std::memory_order_relaxed);
        const bool excluded  = st.captureExcluded.load(std::memory_order_relaxed);

        if (!captureOk)
            card.captureLabel.Text(L"Capture: yeniden bağlanıyor");
        else if (!excluded)
            card.captureLabel.Text(L"Capture: feedback riski");
        else
            card.captureLabel.Text(L"Capture: OK");

        // ── Freeze ──
        const bool frozen = st.isFrozen.load(std::memory_order_relaxed);
        if (card.freezeButton.IsChecked() &&
            card.freezeButton.IsChecked().Value() != frozen)
        {
            card.freezeButton.IsChecked(winrt::box_value(frozen));
        }
        else if (!card.freezeButton.IsChecked() && frozen)
        {
            card.freezeButton.IsChecked(winrt::box_value(true));
        }
    }
}
```

`#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>` ve `<cmath>` (pch'de var) gerekli.

- [ ] **Step 6: `kMsgDisplayChange` işleyicisini kartları yenileyecek şekilde bağla**

`ThreadMain()` mesaj loop'undaki `kMsgDisplayChange` dalını değiştir:

```cpp
            if (msg.message == kMsgDisplayChange)
            {
                LOG_DEBUG("Panel: display degisikligi, kartlar yeniden kuruluyor");
                RebuildMonitorCards();
                continue;
            }
```

- [ ] **Step 7: `activeToggle` geri tepmesini düzelt**

Step 5'teki `suppressSliderEvent` bayrağı slider ve toggle için paylaşılıyor ama `Toggled` handler'ı onu KONTROL etmiyor. Düzelt — `activeToggle.Toggled` lambda'sını şuna çevir:

```cpp
        card.activeToggle.Toggled(
            [this, engine, index](winrt::Windows::Foundation::IInspectable const&,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
            {
                if (index >= m_impl->cards.size())
                    return;
                if (m_impl->cards[index].suppressSliderEvent)
                    return;   // Programatik senkronlama, kullanici tiklamasi degil

                PostMessageW(engine, WM_APP_TOGGLE_ZOOM, static_cast<WPARAM>(index), 0);
            });
```

Aynısını `freezeButton.Click` için de yap.

Bayrak adı artık yanıltıcı — `MonitorCard` içinde `suppressSliderEvent`'i `suppressEvents` olarak yeniden adlandır ve tüm kullanım yerlerini güncelle (Step 3, 5, 7).

- [ ] **Step 8: Derle**

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /restore /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Expected: PASS. Olası hatalar:
- `ToggleButton::IsChecked()` `IReference<bool>` döndürür (nullable) — doğrudan `bool` ile karşılaştırma derlenmez; Step 5'teki `.Value()` kullanımını koru
- `DispatcherTimer::Interval` `TimeSpan` bekliyor — `std::chrono::milliseconds` implicit dönüşür (C++/WinRT sağlıyor); olmazsa `winrt::Windows::Foundation::TimeSpan{1000000}` (100 ms = 1.000.000 × 100 ns) kullan
- `std::format` wide string ile → `<format>` pch'de var, `std::format(L"...")` C++20'de destekli

- [ ] **Step 9: Elle test — canlı değerler ve slider**

`.\bin\Debug-x64\BetterMagnifier.exe` çalıştır, tray → Ayarlar.

Doğrula:
1. Üç monitör kartı görünüyor, başlıklarda doğru cihaz adı, çözünürlük, Hz, DPI; primary etiketli
2. Panelden bir monitörün "Aktif" toggle'ını aç → o monitörde overlay görünür, FPS sayacı dönmeye başlar
3. Zoom slider'ını sürükle → `zoomLabel` anında değişir, log'da `Monitor N zoom: X.XX`
4. `Ctrl+Alt+Z` ile hotkey'den zoom aç/kapa → panelin toggle'ı KENDİLİĞİNDEN senkronlanır (motor→GUI yönü çalışıyor)
5. Fare tekerleğiyle zoom değiştir → slider kendiliğinden kayar, geri tepme/titreme YOK
6. Freeze düğmesine bas → `isFrozen` senkronlanır, focal point sabitlenir
7. Paneli kapat, `Ctrl+Alt+Z` ile zoom aç, paneli tekrar aç → değerler doğru (gizliyken güncelleme atlanıyor ama açılışta `UpdateLiveValues` çağrılıyor)

**Geri tepme testi kritik:** slider'ı sürüklerken titreme veya değerin geri zıplaması varsa `suppressEvents` mantığı yanlış — 0.005 eşiğini ve bayrak sırasını kontrol et.

- [ ] **Step 10: Commit**

```bash
git add src/ControlPanel.h src/ControlPanel.cpp src/StatusSnapshot.h src/App.h src/App.cpp
git commit -m @'
feat(gui): add live status tab with per-monitor cards

* Monitor cards show device name, resolution, refresh rate, DPI, primary
* DispatcherTimer polls StatusSnapshot at 10 Hz, no locks on render path
* Zoom slider posts WM_APP_SET_ZOOM; engine state syncs back to the UI
* suppressEvents guard prevents programmatic updates from echoing back
* Capture health surfaces reconnect and feedback-risk states
'@
```

---

### Task 7: Ayarlar sekmesi

Spec bölüm 6 "Ayarlar sekmesi".

**Files:**
- Modify: `src/ControlPanel.h` (`BuildSettingsTab`, `PushSettings` bildirimleri)
- Modify: `src/ControlPanel.cpp`
- Modify: `src/HotkeyManager.h` (`LastFailedMask` zaten Task 4'te eklendi — GUI'ye taşımak için snapshot'a bayrak ekle)
- Modify: `src/StatusSnapshot.h` (`hotkeyFailedMask`)
- Modify: `src/App.cpp` (`ApplySettings` sonrası bayrağı yayınla, `startWithWindows` uygula)

**Interfaces:**
- Consumes: `SettingsStore::MutableGeneral()`, `WM_APP_SETTINGS_CHANGED`, `StatusSnapshot::hotkeyFailedMask`
- Produces: `void ControlPanel::BuildSettingsTab()`, `void ControlPanel::PushSettings()` (private); `bool ApplyStartWithWindows(bool enable)` (`App.cpp` anonim namespace)

- [ ] **Step 1: Hotkey hata bayrağını snapshot'a ekle**

`src/StatusSnapshot.h` `StatusSnapshot` sınıfının `public:` bölümüne, `monitorCount`'un yanına ekle:

```cpp
    // HotkeyManager::Reregister sonucu. bit 0 = toggle basarisiz,
    // bit 1 = freeze basarisiz. Panel bunu kirmizi uyari satirinda gosteriyor.
    std::atomic<unsigned> hotkeyFailedMask{0};
```

`src/App.cpp` `ApplySettings()` içindeki `m_hotkeyManager.Reregister(g);` satırını şuna çevir:

```cpp
    const UINT failedMask = m_hotkeyManager.Reregister(g);
    m_status.hotkeyFailedMask.store(failedMask, std::memory_order_release);
```

`InitializeComponents()` içinde `m_hotkeyManager.Initialize(...)` çağrısından sonra ekle:

```cpp
    m_status.hotkeyFailedMask.store(m_hotkeyManager.LastFailedMask(), std::memory_order_release);
```

- [ ] **Step 2: `startWithWindows`'u uygula**

`src/App.cpp` anonim namespace'e (dosya başında, `namespace BetterMagnifier {` sonrası) ekle:

```cpp
namespace {

// =============================================================================
// ApplyStartWithWindows — Run registry anahtari
// =============================================================================
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run
//
// Neden HKCU, HKLM degil: HKLM admin gerektirir. HKCU kullanici bazinda,
// yetki istemez. Tasinabilirlik: exe yolu degisirse anahtar bayatlar —
// her acilista yeniden yaziyoruz, o yuzden sorun degil.
//
// Python analojisi: winreg.SetValueEx(winreg.HKEY_CURRENT_USER, ...)
// =============================================================================
bool ApplyStartWithWindows(bool enable)
{
    constexpr wchar_t kRunKey[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kValueName[] = L"BetterMagnifier";

    HKEY key = nullptr;
    LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (st != ERROR_SUCCESS)
    {
        LOG_ERROR("Run anahtari acilamadi: {}", st);
        return false;
    }

    bool ok = false;

    if (enable)
    {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        {
            LOG_ERROR("GetModuleFileNameW basarisiz: {}", GetLastError());
            RegCloseKey(key);
            return false;
        }

        // Yol bosluk icerebilir — cift tirnak icine al, yoksa Windows
        // yolu ilk bosluktan keser.
        const std::wstring quoted = L"\"" + std::wstring(exePath) + L"\"";

        st = RegSetValueExW(key, kValueName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(quoted.c_str()),
                static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
        ok = (st == ERROR_SUCCESS);

        if (!ok)
            LOG_ERROR("Run degeri yazilamadi: {}", st);
    }
    else
    {
        st = RegDeleteValueW(key, kValueName);
        // Deger yoksa da basarili sayiyoruz — istenen son durum bu.
        ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);

        if (!ok)
            LOG_ERROR("Run degeri silinemedi: {}", st);
    }

    RegCloseKey(key);
    return ok;
}

} // anonymous namespace
```

`ApplySettings()` sonuna ekle:

```cpp
    // Windows ile baslat
    ApplyStartWithWindows(g.startWithWindows);
```

- [ ] **Step 3: `ControlPanel.h`'ye ayar sekmesi metodlarını ekle**

`private:` metodlara ekle:

```cpp
    // Ayarlar sekmesini kur. SADECE GUI thread'den, bir kez.
    void BuildSettingsTab();

    // GUI kontrollerinden SettingsStore'a yaz, sonra motora haber ver.
    // SIRA ONEMLI: once yaz, SONRA PostMessage — yoksa motor eski degeri okur.
    void PushSettings();
```

`Impl` yapısına ayar kontrollerini ekle (`src/ControlPanel.cpp`):

```cpp
    // ── Ayarlar sekmesi kontrolleri ──
    winrt::Microsoft::UI::Xaml::Controls::TextBox      toggleHotkeyBox{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox      freezeHotkeyBox{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock    hotkeyWarning{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::CheckBox     hijackWinZBox{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::RadioButton  followMouseRadio{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::RadioButton  followFocusRadio{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::NumberBox    minZoomBox{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::NumberBox    maxZoomBox{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::NumberBox    zoomStepBox{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::CheckBox     startWithWindowsBox{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::CheckBox     rememberZoomBox{nullptr};

    bool suppressSettingsEvents = false;
```

- [ ] **Step 4: `BuildSettingsTab`'i implement et**

```cpp
// =============================================================================
// BuildSettingsTab
// =============================================================================
// Hotkey girisi TextBox ile: kullanici "Ctrl+Alt+Z" yaziyor, ParseHotkey
// dogruluyor. Gercek tus yakalama (KeyDown ile) daha hos olurdu ama
// Win32 hotkey semantigi ile XAML KeyRoutedEventArgs arasindaki eslestirme
// (extended key'ler, sag/sol modifier ayrimi) ayri bir is — YAGNI.
// Metin girisi calisiyor ve ParseHotkey zaten test edilmis.
// =============================================================================
void ControlPanel::BuildSettingsTab()
{
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;

    if (!m_impl || !m_impl->settingsPanel || !m_settings)
        return;

    auto& panel = m_impl->settingsPanel;
    panel.Children().Clear();

    const auto& g = m_settings->General();

    auto addHeader = [&panel](std::wstring_view text) {
        TextBlock h{};
        h.Text(text);
        h.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        h.Margin(ThicknessHelper::FromLengths(0, 12, 0, 0));
        panel.Children().Append(h);
    };

    auto addHint = [&panel](std::wstring_view text) {
        TextBlock t{};
        t.Text(text);
        t.FontSize(12.0);
        t.Opacity(0.7);
        t.TextWrapping(TextWrapping::Wrap);
        panel.Children().Append(t);
    };

    // ── Kisayol tuslari ──
    addHeader(L"Kısayol tuşları");

    m_impl->toggleHotkeyBox = TextBox{};
    m_impl->toggleHotkeyBox.Header(winrt::box_value(L"Zoom aç/kapa"));
    m_impl->toggleHotkeyBox.Text(FormatHotkey(g.toggleModifiers, g.toggleVk));
    panel.Children().Append(m_impl->toggleHotkeyBox);

    m_impl->freezeHotkeyBox = TextBox{};
    m_impl->freezeHotkeyBox.Header(winrt::box_value(L"Freeze / sabitle"));
    m_impl->freezeHotkeyBox.Text(FormatHotkey(g.freezeModifiers, g.freezeVk));
    panel.Children().Append(m_impl->freezeHotkeyBox);

    addHint(L"Biçim: Ctrl+Alt+Z · Modifier'lar: Ctrl, Alt, Shift, Win · Tuşlar: A-Z, 0-9, F1-F24");

    // Kirmizi uyari satiri — UpdateLiveValues bunu snapshot'tan dolduruyor
    m_impl->hotkeyWarning = TextBlock{};
    m_impl->hotkeyWarning.FontSize(12.0);
    m_impl->hotkeyWarning.TextWrapping(TextWrapping::Wrap);
    m_impl->hotkeyWarning.Foreground(
        Media::SolidColorBrush{winrt::Windows::UI::ColorHelper::FromArgb(255, 255, 99, 99)});
    m_impl->hotkeyWarning.Visibility(Visibility::Collapsed);
    panel.Children().Append(m_impl->hotkeyWarning);

    m_impl->hijackWinZBox = CheckBox{};
    m_impl->hijackWinZBox.Content(winrt::box_value(L"Win+Z'yi ele geçir"));
    m_impl->hijackWinZBox.IsChecked(winrt::box_value(g.hijackWinZ));
    panel.Children().Append(m_impl->hijackWinZBox);

    addHint(L"Win+Z normalde Windows'un Snap Layouts kısayolu; RegisterHotKey ile alınamıyor. "
            L"Bu seçenek düşük seviye klavye hook'u kurup tuşu yutar — açıkken Snap Layouts çalışmaz.");

    // ── Takip modu ──
    addHeader(L"Takip modu");

    m_impl->followMouseRadio = RadioButton{};
    m_impl->followMouseRadio.Content(winrt::box_value(L"Sadece fare"));
    m_impl->followMouseRadio.GroupName(L"FollowMode");
    m_impl->followMouseRadio.IsChecked(winrt::box_value(g.followMode == FollowMode::Mouse));
    panel.Children().Append(m_impl->followMouseRadio);

    m_impl->followFocusRadio = RadioButton{};
    m_impl->followFocusRadio.Content(winrt::box_value(L"Fare + klavye odağı"));
    m_impl->followFocusRadio.GroupName(L"FollowMode");
    m_impl->followFocusRadio.IsChecked(winrt::box_value(g.followMode == FollowMode::MouseAndFocus));
    panel.Children().Append(m_impl->followFocusRadio);

    addHint(L"Klavye odağı takibi: Tab ile odak değiştirince zoom bölgesi oraya kayar.");

    // ── Zoom sinirlari ──
    // Tum monitorlere uygulanir. Per-monitor farkli sinir istegi cikarsa
    // buraya monitor secici eklenir — su an YAGNI.
    addHeader(L"Zoom sınırları");

    auto makeNumberBox = [](std::wstring_view header, double value,
                            double min, double max, double step) {
        NumberBox nb{};
        nb.Header(winrt::box_value(header));
        nb.Value(value);
        nb.Minimum(min);
        nb.Maximum(max);
        nb.SmallChange(step);
        nb.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
        return nb;
    };

    // Ilk monitorun ayarlarini temsili olarak gosteriyoruz; yazarken
    // TUM monitorlere uygulaniyor (PushSettings'e bak).
    MonitorSettings representative{};
    if (m_status)
    {
        const std::wstring device(m_status->Monitor(0).deviceName);
        if (!device.empty())
            representative = m_settings->Monitor(device);
    }

    m_impl->minZoomBox  = makeNumberBox(L"En az", representative.minZoom,  1.0, 20.0, 0.25);
    m_impl->maxZoomBox  = makeNumberBox(L"En çok", representative.maxZoom,  1.0, 20.0, 0.25);
    m_impl->zoomStepBox = makeNumberBox(L"Adım",  representative.zoomStep, 0.05, 2.0, 0.05);

    panel.Children().Append(m_impl->minZoomBox);
    panel.Children().Append(m_impl->maxZoomBox);
    panel.Children().Append(m_impl->zoomStepBox);

    addHint(L"Tüm monitörlere uygulanır.");

    // ── Diger ──
    addHeader(L"Diğer");

    m_impl->startWithWindowsBox = CheckBox{};
    m_impl->startWithWindowsBox.Content(winrt::box_value(L"Windows ile başlat"));
    m_impl->startWithWindowsBox.IsChecked(winrt::box_value(g.startWithWindows));
    panel.Children().Append(m_impl->startWithWindowsBox);

    m_impl->rememberZoomBox = CheckBox{};
    m_impl->rememberZoomBox.Content(winrt::box_value(L"Zoom kapanınca seviyeyi hatırla"));
    m_impl->rememberZoomBox.IsChecked(winrt::box_value(g.rememberZoomLevel));
    panel.Children().Append(m_impl->rememberZoomBox);

    // ── Olay baglantilari ──
    // Hepsi ayni yere gidiyor: PushSettings.
    auto onChanged = [this](winrt::Windows::Foundation::IInspectable const&,
                            RoutedEventArgs const&)
    {
        if (m_impl && !m_impl->suppressSettingsEvents)
            PushSettings();
    };

    m_impl->hijackWinZBox.Checked(onChanged);
    m_impl->hijackWinZBox.Unchecked(onChanged);
    m_impl->followMouseRadio.Checked(onChanged);
    m_impl->followFocusRadio.Checked(onChanged);
    m_impl->startWithWindowsBox.Checked(onChanged);
    m_impl->startWithWindowsBox.Unchecked(onChanged);
    m_impl->rememberZoomBox.Checked(onChanged);
    m_impl->rememberZoomBox.Unchecked(onChanged);

    // TextBox: LostFocus'ta uygula, her tus vurusunda degil —
    // "Ctrl+Alt+" yariyolda gecersiz, her harfte hata gostermek gurultu.
    auto onLostFocus = [this](winrt::Windows::Foundation::IInspectable const&,
                              RoutedEventArgs const&)
    {
        if (m_impl && !m_impl->suppressSettingsEvents)
            PushSettings();
    };
    m_impl->toggleHotkeyBox.LostFocus(onLostFocus);
    m_impl->freezeHotkeyBox.LostFocus(onLostFocus);

    // NumberBox: ValueChanged farkli imza kullaniyor
    auto onNumberChanged = [this](NumberBox const&, NumberBoxValueChangedEventArgs const&)
    {
        if (m_impl && !m_impl->suppressSettingsEvents)
            PushSettings();
    };
    m_impl->minZoomBox.ValueChanged(onNumberChanged);
    m_impl->maxZoomBox.ValueChanged(onNumberChanged);
    m_impl->zoomStepBox.ValueChanged(onNumberChanged);
}
```

`#include <winrt/Windows.UI.h>` ekle (`ColorHelper` için).

- [ ] **Step 5: `PushSettings`'i implement et**

```cpp
// =============================================================================
// PushSettings — GUI -> SettingsStore -> motor
// =============================================================================
// SIRA KRITIK:
//   1. SettingsStore'a yaz
//   2. Diske kaydet
//   3. SONRA motora PostMessage
//
// Ters sirada yaparsak motor eski degerleri okur. PostMessage asenkron,
// biz once yazmayi bitirdigimiz icin yaris yok.
// =============================================================================
void ControlPanel::PushSettings()
{
    if (!m_impl || !m_settings)
        return;

    auto& g = m_settings->MutableGeneral();

    // ── Hotkey'ler ──
    // ParseHotkey basarisizsa cikti parametrelerine DOKUNMUYOR — eski deger
    // korunuyor. Kullaniciya kirmizi satirla haber veriyoruz.
    std::wstring warning;

    {
        const std::wstring text{m_impl->toggleHotkeyBox.Text()};
        if (!ParseHotkey(text, g.toggleModifiers, g.toggleVk))
            warning += L"Zoom kısayolu anlaşılamadı: \"" + text + L"\". ";
    }
    {
        const std::wstring text{m_impl->freezeHotkeyBox.Text()};
        if (!ParseHotkey(text, g.freezeModifiers, g.freezeVk))
            warning += L"Freeze kısayolu anlaşılamadı: \"" + text + L"\". ";
    }

    if (!warning.empty())
    {
        m_impl->hotkeyWarning.Text(warning);
        m_impl->hotkeyWarning.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
    }

    // ── Bayraklar ──
    auto checkboxValue = [](winrt::Microsoft::UI::Xaml::Controls::CheckBox const& cb) {
        const auto v = cb.IsChecked();
        return v && v.Value();
    };

    g.hijackWinZ       = checkboxValue(m_impl->hijackWinZBox);
    g.startWithWindows = checkboxValue(m_impl->startWithWindowsBox);
    g.rememberZoomLevel = checkboxValue(m_impl->rememberZoomBox);

    // ── Takip modu ──
    {
        const auto v = m_impl->followMouseRadio.IsChecked();
        g.followMode = (v && v.Value()) ? FollowMode::Mouse : FollowMode::MouseAndFocus;
    }

    // ── Zoom sinirlari — TUM monitorlere uygula ──
    {
        double minZ  = m_impl->minZoomBox.Value();
        double maxZ  = m_impl->maxZoomBox.Value();
        double stepZ = m_impl->zoomStepBox.Value();

        // NumberBox bos birakilinca NaN dondurur — varsayilana dus.
        if (std::isnan(minZ))  minZ  = 1.0;
        if (std::isnan(maxZ))  maxZ  = 10.0;
        if (std::isnan(stepZ)) stepZ = 0.25;

        // Mantiksiz araligi duzelt: max her zaman min'den buyuk olmali.
        if (maxZ <= minZ)
            maxZ = minZ + 1.0;

        const size_t count = m_status
            ? m_status->monitorCount.load(std::memory_order_acquire) : 0;

        for (size_t i = 0; i < count; ++i)
        {
            const std::wstring device(m_status->Monitor(i).deviceName);
            if (device.empty())
                continue;

            auto ms = m_settings->Monitor(device);
            ms.minZoom  = static_cast<float>(minZ);
            ms.maxZoom  = static_cast<float>(maxZ);
            ms.zoomStep = static_cast<float>(stepZ);
            ms.lastZoom = std::clamp(ms.lastZoom, ms.minZoom, ms.maxZoom);
            m_settings->SetMonitor(device, ms);
        }
    }

    // ── Kaydet, sonra motora haber ver ──
    m_settings->Save();

    if (m_engineHwnd)
        PostMessageW(m_engineHwnd, WM_APP_SETTINGS_CHANGED, 0, 0);

    // Durum sekmesindeki slider aralikları da degisti — kartlari yenile.
    RebuildMonitorCards();
}
```

- [ ] **Step 6: `settingsPanel`'i kaydet ve `BuildSettingsTab`'i çağır**

`ThreadMain()` içindeki `settingsTab` bloğunu şuna çevir:

```cpp
    TabViewItem settingsTab{};
    settingsTab.Header(winrt::box_value(L"Ayarlar"));
    settingsTab.IsClosable(false);
    {
        ScrollViewer sv{};
        m_impl->settingsPanel = StackPanel{};
        m_impl->settingsPanel.Padding(ThicknessHelper::FromUniformLength(16));
        m_impl->settingsPanel.Spacing(8);
        sv.Content(m_impl->settingsPanel);
        settingsTab.Content(sv);
    }
```

`RebuildMonitorCards(); StartLiveTimer();` satırlarının arasına ekle:

```cpp
    BuildSettingsTab();
```

- [ ] **Step 7: Hotkey uyarısını canlı okumaya bağla**

`UpdateLiveValues()` sonuna ekle:

```cpp
    // ── Hotkey kayit hatalari ──
    // RegisterHotKey basarisiz olduysa kullaniciya soyle. Sadece log'a
    // yazmak yeterli degil — kullanici tusa basip hicbir sey olmadigini
    // gorur ve nedenini bilmez.
    if (m_impl->hotkeyWarning && m_settings)
    {
        const unsigned mask = m_status->hotkeyFailedMask.load(std::memory_order_acquire);

        if (mask != 0)
        {
            const auto& g = m_settings->General();
            std::wstring msg;

            if (mask & 0b01)
                msg += L"\"" + FormatHotkey(g.toggleModifiers, g.toggleVk)
                     + L"\" kaydedilemedi (başka uygulama veya Windows kullanıyor). ";
            if (mask & 0b10)
                msg += L"\"" + FormatHotkey(g.freezeModifiers, g.freezeVk)
                     + L"\" kaydedilemedi. ";

            m_impl->hotkeyWarning.Text(msg);
            m_impl->hotkeyWarning.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
        else if (m_impl->hotkeyWarning.Visibility() == winrt::Microsoft::UI::Xaml::Visibility::Visible)
        {
            // Parse hatasi uyarisini ezmemek icin: sadece bizim yazdigimiz
            // "kaydedilemedi" metnini temizliyoruz.
            const std::wstring current{m_impl->hotkeyWarning.Text()};
            if (current.find(L"kaydedilemedi") != std::wstring::npos)
                m_impl->hotkeyWarning.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
    }
```

- [ ] **Step 8: Derle**

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /restore /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Expected: PASS. Olası hatalar:
- `NumberBox` bulunamıyor → `winrt/Microsoft.UI.Xaml.Controls.h` içinde olmalı; WinUI 3'te `NumberBox` `Microsoft.UI.Xaml.Controls` altında
- `RadioButton::GroupName` `hstring` bekliyor → `L"FollowMode"` implicit dönüşür
- `ColorHelper` bulunamıyor → `winrt/Windows.UI.h` include'ı ekli mi
- `Media::SolidColorBrush` → `winrt::Microsoft::UI::Xaml::Media::SolidColorBrush` tam nitelikli yaz

- [ ] **Step 9: Elle test — ayar kalıcılığı**

Uygulamayı çalıştır, tray → Ayarlar → Ayarlar sekmesi.

Doğrula:
1. Tüm kontroller mevcut değerlerle dolu geliyor
2. Toggle hotkey'i `Ctrl+Shift+M` yap, alana tıklamayı bırak (LostFocus) → log'da `Hotkey: Ctrl+Shift+M = Toggle Zoom`; `Ctrl+Shift+M` gerçekten çalışıyor, eski `Ctrl+Alt+Z` çalışmıyor
3. Geçersiz metin yaz (`Bogus+Q`), odağı kaydır → kırmızı uyarı "Zoom kısayolu anlaşılamadı", eski hotkey ÇALIŞMAYA DEVAM ediyor
4. `Win+Z` yaz → kırmızı uyarı "kaydedilemedi" (Snap Layouts rezerve) — Task 9'un neden gerektiğinin kanıtı
5. Zoom max'ı 4.0 yap → Durum sekmesindeki slider'ların üst sınırı 4.0'a düşüyor; 8x'te açık bir zoom 4.0'a çekiliyor
6. "Windows ile başlat"ı işaretle, doğrula:

```bash
powershell -Command "Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name BetterMagnifier -ErrorAction SilentlyContinue | Select-Object -ExpandProperty BetterMagnifier"
```

Expected: exe'nin tam yolu, çift tırnak içinde. İşareti kaldır → anahtar silinir.

7. Uygulamayı kapat, tekrar aç → tüm ayarlar korunmuş. INI dosyasını gör:

```bash
powershell -Command "Get-Content (Join-Path $env:APPDATA 'BetterMagnifier\settings.ini')"
```

- [ ] **Step 10: Commit**

```bash
git add src/ControlPanel.h src/ControlPanel.cpp src/StatusSnapshot.h src/App.cpp
git commit -m @'
feat(gui): add settings tab

* Hotkey text entry validated through ParseHotkey, invalid input keeps old binding
* Failed RegisterHotKey surfaces as a red warning instead of a log-only line
* Follow mode, zoom limits, start-with-Windows and remember-zoom wired up
* Settings write to disk before notifying the engine, avoiding a stale read
* Start-with-Windows writes a quoted exe path to the HKCU Run key
'@
```

---

### Task 8: Klavye odağı takibi

Spec bölüm 6 "Klavye odağı takibi". `FollowMode::MouseAndFocus`'u gerçek yapar.

**Files:**
- Modify: `src/InputThread.h`, `src/InputThread.cpp` (`SetWinEventHook`)
- Modify: `src/App.h`, `src/App.cpp` (`WM_APP_FOCUS_CHANGED` işleyicisi)

**Interfaces:**
- Consumes: `WM_APP_FOCUS_CHANGED`, `GeneralSettings::followMode`
- Produces: `void InputThread::SetFollowMode(FollowMode)` — thread-safe; `void App::OnFocusChanged(HWND)`

- [ ] **Step 1: `InputThread`'e focus hook ekle**

`src/InputThread.h`:
- `#include "SettingsStore.h"` ekle (`FollowMode` için)
- `public:` bölümüne ekle:

```cpp
    // Takip modunu degistir. Ayarlar degisince cagrilir, thread-safe.
    // Mouse modunda focus hook olaylari yoksayilir (hook kurulu kalir —
    // kur/kaldir yapmaktansa atomic bayrak okumak daha ucuz).
    void SetFollowMode(FollowMode mode);
```

- `private:` bölümüne ekle:

```cpp
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild,
                                      DWORD idEventThread, DWORD dwmsEventTime);

    HWINEVENTHOOK           m_focusHook = nullptr;
    std::atomic<FollowMode> m_followMode{FollowMode::MouseAndFocus};
```

- `Start` imzasını değiştir:

```cpp
    bool Start(HWND targetHwnd, FollowMode initialMode);
```

- [ ] **Step 2: Hook'u kur ve callback'i yaz**

`src/InputThread.cpp` `InstallHooks()` sonuna, `return true;`'dan ÖNCE ekle:

```cpp
    // ── Klavye odagi hook'u ──
    // SetWinEventHook(EVENT_OBJECT_FOCUS): sistemde odak degisince haber verir.
    // WINEVENT_OUTOFCONTEXT: callback BIZIM thread'imizde cagrilir (DLL
    // enjeksiyonu yok). Bu yuzden bu thread'in mesaj loop'u olmak zorunda —
    // zaten var.
    //
    // Python analojisi: pywinauto/UIAutomation'in focus event handler'i.
    m_focusHook = SetWinEventHook(
        EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS,
        nullptr,                  // tum process'ler
        WinEventProc,
        0, 0,                     // tum process/thread
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!m_focusHook)
    {
        // Kritik degil — fare takibi calismaya devam eder.
        LOG_WARN("EVENT_OBJECT_FOCUS hook kurulamadi: {} — klavye odagi takibi devre disi",
            GetLastError());
    }
    else
    {
        LOG_INFO("  Klavye odagi hook'u aktif");
    }
```

`RemoveHooks()`'a ekle:

```cpp
    if (m_focusHook)
    {
        UnhookWinEvent(m_focusHook);
        m_focusHook = nullptr;
        LOG_DEBUG("Klavye odagi hook'u kaldirildi");
    }
```

Callback:

```cpp
// =============================================================================
// WinEventProc — odak degisti
// =============================================================================
// WINEVENT_SKIPOWNPROCESS sayesinde kendi pencerelerimiz (panel, overlay)
// buraya dusmez — panelde gezinirken zoom bolgesinin zıplamasi engellendi.
// =============================================================================
void CALLBACK InputThread::WinEventProc(
    HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG /*idChild*/,
    DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/)
{
    if (event != EVENT_OBJECT_FOCUS || !hwnd)
        return;

    // idObject == OBJID_CLIENT: gercek bir kontrol odaklandi.
    // Menu/scrollbar/caret gibi alt nesneleri yoksayiyoruz — zoom bolgesini
    // her scrollbar tiklamasinda zıplatmak istemiyoruz.
    if (idObject != OBJID_CLIENT)
        return;

    if (!s_instance || !s_instance->m_target)
        return;

    if (s_instance->m_followMode.load(std::memory_order_relaxed) != FollowMode::MouseAndFocus)
        return;

    PostMessageW(s_instance->m_target, WM_APP_FOCUS_CHANGED, 0,
                 reinterpret_cast<LPARAM>(hwnd));
}

void InputThread::SetFollowMode(FollowMode mode)
{
    m_followMode.store(mode, std::memory_order_relaxed);
}
```

`Start()` imzasını ve gövdesini güncelle — `initialMode` parametresini `m_followMode`'a yaz:

```cpp
bool InputThread::Start(HWND targetHwnd, FollowMode initialMode)
{
    if (m_running.load(std::memory_order_acquire))
        return true;
    ...
    m_followMode.store(initialMode, std::memory_order_relaxed);
    m_target   = targetHwnd;
    s_instance = this;
    ...
```

- [ ] **Step 3: `App`'te odak olayını işle**

`src/App.h` `private:` metodlara ekle:

```cpp
    void OnFocusChanged(HWND focused);
```

`src/App.cpp`:

```cpp
// =============================================================================
// OnFocusChanged — klavye odagi degisti, focal point'i oraya kaydir
// =============================================================================
// Odaklanan pencerenin MERKEZINI focal point yapiyoruz. Daha isabetli olan
// caret pozisyonu UI Automation gerektiriyor ve uygulama bazinda tutarsiz
// calisiyor — kapsam disi (spec bolum 1).
//
// Freeze aktifse dokunmuyoruz: kullanici bilincli olarak sabitlemis.
// =============================================================================
void App::OnFocusChanged(HWND focused)
{
    if (!focused)
        return;

    if (m_settings.General().followMode != FollowMode::MouseAndFocus)
        return;

    RECT rc{};
    if (!GetWindowRect(focused, &rc))
        return;

    // Sifir boyutlu veya ekran disi pencereleri yoksay
    if (rc.right <= rc.left || rc.bottom <= rc.top)
        return;

    POINT center{ (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };

    MonitorInfo* target = m_monitorManager.FindByPoint(center);
    if (!target)
        return;

    // Sadece zoom AKTIF ve frozen DEGILSE kaydir
    if (!target->zoom.isActive || target->zoom.isFrozen)
        return;

    target->zoom.focalPoint = center;
}
```

`MessageWndProc`'a ekle:

```cpp
    case WM_APP_FOCUS_CHANGED:
        if (s_instance)
            s_instance->OnFocusChanged(reinterpret_cast<HWND>(lParam));
        return 0;
```

- [ ] **Step 4: `Update()`'in fare takibini odak takibiyle çakışmayacak hale getir**

Şu anda `Update()` her frame fare pozisyonunu focal point'e yazıyor — odak takibi bunu anında eziyor. Fare gerçekten hareket ettiğinde fareyi, etmediğinde odağı takip etmeli.

`src/App.h` `private:` üyelere ekle:

```cpp
    // Fare gercekten hareket etti mi? Odak takibi ile cakismasin diye.
    POINT m_lastCursorPos{ -1, -1 };
```

`src/App.cpp` `Update()` içindeki focal point atamasını şuna çevir:

```cpp
        // ── Freeze aktif degilse focal point'i fareye kilitle ──
        // ONEMLI: sadece fare GERCEKTEN HAREKET ETTIYSE. Yoksa klavye odagi
        // takibinin yazdigi focal point'i her frame eziyoruz ve Tab ile
        // odak degistirmek hicbir sey yapmiyor gibi gorunuyor.
        const bool cursorMoved = (cursor.x != m_lastCursorPos.x)
                              || (cursor.y != m_lastCursorPos.y);

        if (cursorMoved && !mon->zoom.isFrozen && PtInRect(&mon->bounds, cursor))
        {
            mon->zoom.focalPoint = cursor;
        }
```

`Update()` sonuna (döngüden SONRA) ekle:

```cpp
    m_lastCursorPos = cursor;
```

- [ ] **Step 5: `Start` ve `ApplySettings` çağrılarını güncelle**

`src/App.cpp` `InitializeComponents()` içindeki input thread başlatmayı:

```cpp
    if (!m_inputThread.Start(m_messageHwnd, m_settings.General().followMode))
        LOG_WARN("InputThread baslatilamadi — mouse wheel zoom devre disi");
```

`ApplySettings()` içine ekle:

```cpp
    // Takip modu — input thread'in atomic bayragini guncelle
    m_inputThread.SetFollowMode(g.followMode);
```

- [ ] **Step 6: Derle ve elle test et**

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /restore /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Expected log satırı: `  Klavye odagi hook'u aktif`

Elle test:
1. Zoom aç, Ayarlar → "Fare + klavye odağı" seçili olsun
2. Bir uygulama penceresi aç (Notepad), Alt+Tab ile başka pencereye geç → zoom bölgesi yeni pencereye kayar
3. Fareyi hareket ettir → fare takibi devralır
4. Fareyi hareketsiz bırak, Tab ile odak değiştir → zoom bölgesi odağa kayar (fare ezmiyor)
5. Ayarlar → "Sadece fare" seç → Tab ile odak değişimi artık zoom bölgesini oynatmıyor
6. Freeze aktifken odak değiştir → zoom bölgesi SABIT kalıyor
7. Kontrol panelinde gezin → `WINEVENT_SKIPOWNPROCESS` sayesinde zoom bölgesi panele zıplamıyor

- [ ] **Step 7: Commit**

```bash
git add src/InputThread.h src/InputThread.cpp src/App.h src/App.cpp
git commit -m @'
feat(input): follow keyboard focus with EVENT_OBJECT_FOCUS

* SetWinEventHook runs on the input thread, skips our own process
* Only OBJID_CLIENT focus events move the focal point, not menus or scrollbars
* Mouse tracking now requires actual cursor movement so focus is not overwritten
* Follow mode is an atomic flag, switched live from the settings tab
'@
```

---

### Task 9: `Win+Z` ele geçirme (opt-in)

Spec bölüm 5.5. Kullanıcının açıkça istediği özellik; varsayılan kapalı.

**Files:**
- Modify: `src/InputThread.h`, `src/InputThread.cpp` (`WH_KEYBOARD_LL`)
- Modify: `src/App.cpp` (`ApplySettings`'te bayrağı ilet)

**Interfaces:**
- Consumes: `GeneralSettings::hijackWinZ`, `WM_APP_TOGGLE_ZOOM`
- Produces: `void InputThread::SetHijackWinZ(bool)` — thread-safe

- [ ] **Step 1: `InputThread`'e klavye hook'u ekle**

`src/InputThread.h`:
- `public:` bölümüne ekle:

```cpp
    // Win+Z'yi yut ve kendi toggle'imiza cevir. Thread-safe, canli degisir.
    void SetHijackWinZ(bool enable);
```

- `private:` bölümüne ekle:

```cpp
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    HHOOK             m_keyboardHook = nullptr;
    std::atomic<bool> m_hijackWinZ{false};
```

- `Start` imzasına ekle:

```cpp
    bool Start(HWND targetHwnd, FollowMode initialMode, bool hijackWinZ);
```

- [ ] **Step 2: Hook'u kur ve callback'i yaz**

`src/InputThread.cpp` `InstallHooks()` içine, mouse hook'tan SONRA ekle:

```cpp
    // ── Klavye hook'u ──
    // Her zaman kuruyoruz ama sadece hijackWinZ acikken olay yutuyoruz.
    // Neden kur/kaldir yapmiyoruz: atomic bayrak okumak, hook'u canli
    // kurup kaldirmaktan hem ucuz hem yaris kosulsuz.
    //
    // Bu callback sistemdeki HER tus vurusunda cagriliyor. Icinde is yapmak
    // yasak — bayragi oku, gerekirse PostMessage, hemen don.
    m_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        GetModuleHandleW(nullptr),
        0);

    if (!m_keyboardHook)
    {
        LOG_WARN("WH_KEYBOARD_LL kurulamadi: {} — Win+Z ele gecirme devre disi",
            GetLastError());
    }
    else
    {
        LOG_INFO("  Klavye hook'u aktif");
    }
```

`RemoveHooks()`'a ekle:

```cpp
    if (m_keyboardHook)
    {
        UnhookWindowsHookEx(m_keyboardHook);
        m_keyboardHook = nullptr;
        LOG_DEBUG("Klavye hook'u kaldirildi");
    }
```

Callback:

```cpp
// =============================================================================
// LowLevelKeyboardProc — Win+Z'yi yut
// =============================================================================
// return 1 = olayi YUT (chain'e gitmez, Windows gormez).
// Bu, RegisterHotKey'in yapamadigi seyi yapmanin tek yolu: Win+Z Windows 11'de
// Snap Layouts'a rezerve, RegisterHotKey basarisiz doner.
//
// NE YUTUYORUZ, NE YUTMUYORUZ:
//   Sadece Win basiliyken Z'nin KeyDown'unu yutuyoruz. Win tusunun kendisini
//   yutmuyoruz — yutsak Start menusu, Win+D, Win+E hepsi bozulur.
//
// KAYBEDILEN: Win+Z acikken Windows 11 Snap Layouts calismaz. Varsayilan
// KAPALI olmasinin sebebi bu.
//
// YUTAMADIKLARIMIZ (kernel/Winlogon korumali, kod ile engellenemez):
//   Ctrl+Alt+Del, Win+L. Bunlar mimari olarak bizim erisimimizin disinda.
//
// ADMIN NOTU: Yuksek integrity'li pencere odaktayken (Task Manager, UAC)
// hook devreye girmez. DXGI Desktop Duplication da secure desktop'ta
// calismadigi icin sinir zaten orada — ek kisit getirmiyor.
// =============================================================================
LRESULT CALLBACK InputThread::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance &&
        s_instance->m_hijackWinZ.load(std::memory_order_relaxed))
    {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (kb && kb->vkCode == 'Z' &&
            (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
        {
            // Win tusu basili mi? GetAsyncKeyState en anlik durum kaynagi.
            // 0x8000 biti = su an basili.
            const bool winDown =
                (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

            if (winDown && s_instance->m_target)
            {
                PostMessageW(s_instance->m_target, WM_APP_TOGGLE_ZOOM, kFocusedMonitor, 0);

                // YUT — Snap Layouts bu tusu gormesin.
                return 1;
            }
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void InputThread::SetHijackWinZ(bool enable)
{
    const bool prev = m_hijackWinZ.exchange(enable, std::memory_order_relaxed);

    if (prev != enable)
    {
        LOG_INFO("Win+Z ele gecirme {}{}",
            enable ? "ACIK" : "KAPALI",
            enable ? " — Windows Snap Layouts devre disi" : "");
    }
}
```

`kFocusedMonitor` için `AppMessages.h` zaten include edilmiş.

- [ ] **Step 3: `Start` ve `ApplySettings`'i güncelle**

`src/InputThread.cpp` `Start()` gövdesine, `m_followMode.store(...)` yanına ekle:

```cpp
    m_hijackWinZ.store(hijackWinZ, std::memory_order_relaxed);
```

`src/App.cpp` `InitializeComponents()`:

```cpp
    if (!m_inputThread.Start(m_messageHwnd,
                             m_settings.General().followMode,
                             m_settings.General().hijackWinZ))
    {
        LOG_WARN("InputThread baslatilamadi — mouse wheel zoom devre disi");
    }
```

`ApplySettings()`'e ekle:

```cpp
    m_inputThread.SetHijackWinZ(g.hijackWinZ);
```

- [ ] **Step 4: Derle**

```bash
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" BetterMagnifier.sln /restore /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
```

Expected: PASS, log'da `  Klavye hook'u aktif`.

- [ ] **Step 5: Elle test — yutma çalışıyor ve yan etki yok**

1. Uygulamayı çalıştır, Ayarlar → "Win+Z'yi ele geçir" KAPALI
2. `Win+Z`'ye bas → Windows Snap Layouts açılır (normal davranış), zoom açılmaz
3. Checkbox'ı AÇ → log: `Win+Z ele gecirme ACIK — Windows Snap Layouts devre disi`
4. `Win+Z`'ye bas → **zoom açılır, Snap Layouts AÇILMAZ**
5. `Win+D` (masaüstünü göster), `Win+E` (dosya gezgini), Start menüsü → **hepsi normal çalışıyor** (Win tuşunun kendisini yutmuyoruz)
6. Z tuşuna Win olmadan bas (bir metin alanında) → normal `z` harfi yazılıyor
7. Checkbox'ı tekrar KAPAT → `Win+Z` Snap Layouts'a döner
8. Tray → Exit, log'da `Klavye hook'u kaldirildi`

**Regresyon kontrolü — girdi gecikmesi:** hijack açıkken ve zoom aktifken bir metin editöründe hızlı yazı yaz. Takılma OLMAMALI. Takılma varsa hook yanlış thread'de — `InputThread baslatildi (thread id: N)` satırındaki id'nin log'daki `[T:...]` değerinden farklı olduğunu doğrula.

- [ ] **Step 6: Commit**

```bash
git add src/InputThread.h src/InputThread.cpp src/App.cpp
git commit -m @'
feat(input): add opt-in Win+Z hijacking

* WH_KEYBOARD_LL swallows Win+Z only while the setting is enabled
* Win key itself is never swallowed, so Start menu and Win+D keep working
* Hook lives on the input thread, callback only posts and returns
* Default off because hijacking disables Windows 11 Snap Layouts
'@
```

---

## Plan Self-Review

**1. Spec kapsamı**

| Spec bölümü | Karşılayan task |
|---|---|
| 2 — MTA/STA çakışması | Task 1 (spike), Task 5 (üretim) |
| 3 — Üç thread mimarisi | Task 4 (input), Task 5 (GUI) |
| 3.2 — Veri akışı kontratı | Task 2 (snapshot), Task 4 (mesajlar), Task 6 (10 Hz okuma) |
| 3.3 — GUI thread ömrü | Task 5 Step 9 |
| 4 — Bileşenler | Task 2, 3, 4, 5 |
| 4 — INI ayar deposu | Task 3 |
| 5.2 — İmperatif C++/WinRT | Task 5, 6, 7 |
| 5.3 — SDK minimum 1.5 | Task 1 Step 1, Task 5 Step 1 |
| 5.4 — Runtime'sız çekirdek | Task 5 Step 4, Step 8 |
| 5.5 — `Win+Z` opt-in | Task 9 |
| 6 — Durum sekmesi | Task 6 |
| 6 — Ayarlar sekmesi | Task 7 |
| 6 — Klavye odağı takibi | Task 8 |
| 7 — Hata yönetimi | Hotkey: Task 7 Step 7 · Capture: Task 6 Step 5 · Bootstrapper: Task 5 Step 4 · GUI çökmesi: Task 5 Step 4 (try/catch) · DisplayAffinity: Task 2 Step 8 + Task 6 Step 5 |
| 8 — Test | Task 3 (self-check), diğerlerinde derleme + log + elle doğrulama |

Boşluk yok.

**2. Placeholder taraması**

`PINNED_SDK_VERSION` bilinçli bir yer tutucu — Task 1 Step 1 onu doldurmayı açık bir adım olarak tanımlıyor, sonra Task 5 Step 1 kullanıyor. Bu bir plan hatası değil, sıralı bağımlılık.

Belirsiz "hata yönetimi ekle" tipi ifade yok; her hata yolunun somut davranışı yazılı.

**3. Tip tutarlılığı**

Düzeltilen çelişkiler:
- `MonitorCard::suppressSliderEvent` → `suppressEvents` (Task 6 Step 7'de yeniden adlandırma açıkça belirtildi, çünkü hem slider hem toggle kullanıyor)
- `InputThread::Start` imzası üç task boyunca büyüyor: Task 4 `(HWND)`, Task 8 `(HWND, FollowMode)`, Task 9 `(HWND, FollowMode, bool)`. Her task çağrı yerini güncelliyor — Task 8 Step 5 ve Task 9 Step 3.
- `HotkeyManager::Initialize` imzası Task 4'te `(HWND, const GeneralSettings&)` oluyor; `LastFailedMask()` Task 4 Step 3'te ekleniyor, Task 7 Step 1'de kullanılıyor.
- `StatusSnapshot` üç kez büyüyor: Task 2 (canlı alanlar), Task 6 Step 1 (statik monitör bilgisi), Task 7 Step 1 (`hotkeyFailedMask`). Her ekleme kendi task'ında yazılı.

## Bilinen sınırlar

- Zoom sınırları per-monitor saklanıyor ama GUI'de tüm monitörlere birlikte uygulanıyor (Task 7 Step 4/5). Monitör başına farklı sınır istenirse ayar sekmesine monitör seçici gerekir — şimdilik YAGNI.
- Hotkey girişi metin tabanlı, gerçek tuş yakalama değil (Task 7 Step 4'te gerekçesi yazılı).
- `MonitorStatus::deviceName` atomic değil; `WM_DISPLAYCHANGE` anında teorik yarış var, sonucu bozuk bir etiket (Task 6 Step 1'de belgelendi).
- Render kalitesi ayarları kapsam dışı — shader pipeline (ana yol haritasının Adım 4'ü) gelmeden anlamsız.
