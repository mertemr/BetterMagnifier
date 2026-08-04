# Spike Sonucu: WinUI 3 XAML Islands — BAŞARISIZ

**Tarih:** 2026-08-04
**Soru:** Ana thread MTA kalırken, ayrı bir STA thread'de `DesktopWindowXamlSource` ayağa kalkar mı?
**Cevap:** Soru cevaplanamadı — daha temel bir şey bozuk. `Application::Start` bu ortamda hiçbir konfigürasyonda çalışmıyor.

## Ne denendi

| Varyant | Konfigürasyon | Sonuç |
|---|---|---|
| `mta` | Ana thread MTA + ayrı STA thread | `0xC0000005` |
| `none` | Ana thread apartment init yok + ayrı STA thread | `0xC0000005` |
| `mainsta` | `Application::Start` **ana thread'de**, ikincil thread yok | `0xC0000005` |

Üçüncüsü kontrol deneyi: threading tamamen devre dışı, en basit WinUI 3 kullanımı. O da çöküyor.

**Sonuç: çökme threading kaynaklı değil.** Tasarımın MTA/STA varsayımı test edilemedi.

## Denenen iki başlatma yolu

1. **Elle bootstrapper** — `MddBootstrapInitialize(0x00020003, ...)` → `S_OK`, ardından `DispatcherQueueController::CreateOnCurrentThread()` → OK, ardından `Application app{}` → `0x8001010E` (`RPC_E_WRONG_THREAD`).

   WinUI 3'te `Application` doğrudan aktive edilemiyor; `Application::Start()` üzerinden kurulmalı.

2. **`Application::Start(callback)`** — hem elle bootstrapper ile hem `<WindowsPackageType>None</WindowsPackageType>` auto-initializer ile denendi. İkisinde de callback'e **hiç girmeden** `0xC0000005`.

## Ortam durumu — runtime eksik değil

```
Microsoft.WindowsAppRuntime.2   2.3.1.0   (kurulu)
Microsoft.WindowsAppRuntime.2   2.3.0.0
Microsoft.WindowsAppRuntime.2   2.2.0.0
+ 1.4 / 1.5 / 1.6 / 1.7 / 1.8 hatları
```

Windows App SDK 2.3.1 runtime kurulu. Bootstrapper framework paketini çözüyor (`S_OK`). Yani "redist yükle" çözümü geçerli değil.

## Build tarafında çözülen üç ayrı sorun

Bunlar spike'a özgü değil, ana projeye de gerekecek:

1. **`ResolveNuGetPackageAssets` patlaması** — C++ vcxproj'da `PackageReference` kullanınca `Microsoft.NuGet.targets` içindeki eski task "Sıra hiçbir öğe içermiyor" ile düşüyor (native projede managed `TargetFramework` arıyor). Çözüm: `<ResolveNuGetPackages>false</ResolveNuGetPackages>`.

2. **`Microsoft.WindowsAppRuntime.Bootstrap.lib` bulunamıyor** — Windows App SDK 2.x alt paketlere bölünmüş. Lib `microsoft.windowsappsdk.foundation/<sürüm>/lib/native/x64/` altında ve paketin `.props`'u include yolunu ekliyor ama **lib yolunu eklemiyor**. Elle `AdditionalLibraryDirectories` gerekiyor. `windowsappsdk 2.3.1` → `foundation 2.3.5` (transitive).

3. **`C3779` `IVector<UIElement>::Append`** — `winrt/Windows.Foundation.Collections.h` include etmek şart, `Windows.Foundation.h` yetmiyor.

4. **`C4002` `GetCurrentTime`** — `windows.h` bunu makro yapıyor, `Microsoft.UI.Xaml.Media.Animation`'da aynı isimde metot var. Üretilmiş header'da uyarı çıkıyor. Ana projede `TreatWarningAsError` açık olduğu için **hata** olur. Çözüm: XAML header'larından önce `#undef GetCurrentTime`.

## Sıradaki adım — karar kullanıcıya ait

Spec bölüm 9'daki geri dönüş yolları artık gündemde:

**A. Access violation'ı debugger ile kovala.** WinDbg/VS ile crash stack'ine bak. Muhtemel sebepler: 2.x'te değişmiş bir init sırası, eksik bir `.winmd` kaydı, ya da bu makinedeki runtime kurulumunun bozuk olması. Belirsiz süre.

**B. Windows App SDK 1.8 hattına düş.** 1.8 (`8000.921.1539.0`) kurulu ve Islands o hatta uzun süre olgunlaştı. 2.x'in yeni `XamlIsland`/`DesktopAttachedSiteBridge` API'lerinden vazgeçilir. En düşük riskli WinUI 3 yolu.

**C. Paneli ayrı process'e taşı.** Kontrol paneli kendi exe'si olur, ana uygulama ile `WM_COPYDATA` veya named pipe üzerinden konuşur. WinUI 3 kendi process'inde kendi ana thread'inde yaşar — MTA/STA sorunu tamamen ortadan kalkar. Bedeli: IPC katmanı + iki binary.

**D. GUI teknolojisini yeniden değerlendir.** Brainstorming'de reddedilen Dear ImGui seçeneği bu bulgu ışığında yeniden bakılabilir: mevcut D3D11 device'a oturuyor, çalışma zamanı bağımlılığı sıfır, bu sınıf hatalara açık değil. Bedeli: yerel Windows görünümü değil.

Öneri: **B önce denenmeli** (tek satırlık sürüm değişikliği, spike'ı tekrar çalıştır). Başarısızsa **C**, çünkü tasarımın geri kalanı (SettingsStore, StatusSnapshot, InputThread) IPC ile aynen çalışır.

## Etkilenmeyen işler

Planın Task 2, 3, 4'ü GUI teknolojisinden bağımsız ve bu bulgudan etkilenmiyor:

- **Task 2** `AppMessages.h` + `StatusSnapshot` — thread'ler arası kontrat
- **Task 3** `SettingsStore` — INI kalıcılık + hotkey ayrıştırma
- **Task 4** `InputThread` — hook'ları render thread'den çıkarır (mevcut latent bug)

Bunlar hangi GUI seçilirse seçilsin gerekli.
