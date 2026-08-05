# Spike Sonucu: WinUI 3 XAML Islands — BAŞARILI

**Tarih:** 2026-08-05 (ilk deneme 2026-08-04 başarısızdı, aşağıda)
**Soru:** Ana thread MTA kalırken, ayrı bir STA thread'de `DesktopWindowXamlSource` ayağa kalkar mı?
**Cevap:** **EVET.** Island ikincil STA thread'de çalışıyor, ana thread MTA kalıyor, tema stilleri uygulanıyor, kontroller girdi alıyor.

Kontrol paneli tasarımının açık riski kapandı. Spec bölüm 9'daki geri dönüş yollarına (ayrı process + IPC, Dear ImGui) gerek yok.

## Doğrulanan konfigürasyon

| Ne | Değer |
|---|---|
| Windows App SDK | `1.8.250916003` (foundation `1.8.250906002`, WinUI `1.8.250906003`) |
| Makinedeki runtime | `Microsoft.WindowsAppRuntime.1.8` `8000.921.1539.0` |
| Bootstrapper | `WindowsPackageType=None` auto-initializer (elle `MddBootstrapInitialize` YOK) |
| Ana thread | MTA — `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` |
| GUI thread | STA — `winrt::init_apartment(apartment_type::single_threaded)` |
| Toolset | `$(DefaultPlatformToolset)` (VS 18.8'de v145 ile derlendi) |

Çıktı:

```
=== Varyant: mta ===
Ana thread: MTA (COINIT_MULTITHREADED)
GUI thread baslatiliyor (STA)...
  [1/5] Bootstrapper auto-init (WindowsPackageType=None)
  [2/5] Application::Start cagriliyor...
  [2.5/5] Start callback'ine GIRILDI
  [2.6/5] Application ornegi olusturuldu
  [3/5] XAML runtime OK (tema sozlugu varsayilan)
  [4/5] Host HWND OK
  [5/5] DesktopWindowXamlSource OK

SPIKE BASARILI: island ikincil STA thread'de ayakta, ana thread MTA
```

Pencerede yuvarlak köşeli, düzgün stillenmiş bir WinUI butonu ve doğru renkte metin
görünüyor — hiçbir renk elle verilmedi, hepsi tema sözlüğünden geldi.

## İlk denemeyi batıran iki hata

Önceki tur `0xC0000005` (access violation) alıyordu ve bu, sorunun MTA/STA
olmadığı sonucuna götürmüştü. İki ayrı hata vardı, ikisi de bizim kodumuzdaydı:

### 1. `Application::Start` callback'i bir `Application` örneği OLUŞTURMALI

`Application::Start(cb)` kendisi bir `Application` yaratmaz — bunu callback'ten
bekler. Önceki kod callback içinde doğrudan `Application::Current()` çağırıyordu;
örnek olmadığı için `null` dönüyordu ve `null` üzerinden `.DispatcherShutdownMode()`
çağırmak = access violation.

C++/WinRT'de null bir projeksiyon nesnesinde metot çağırmak sessizce patlar,
istisna fırlatmaz. Hata mesajı olmadığı için "callback'e hiç girilmiyor" sanılmıştı;
oysa giriliyordu, ilk üç satırda düşüyordu.

Çözüm — projeksiyonda hazır olan `ApplicationT<D>` şablonuyla markup'sız bir App:

```cpp
struct SpikeApp : winrt::Microsoft::UI::Xaml::ApplicationT<SpikeApp> {};

Application::Start([](auto&&) {
    auto app = winrt::make<SpikeApp>();   // BU SATIR ŞART
    auto current = Application::Current(); // artık dolu
    ...
});
```

### 2. `XamlControlsResources` elle ATANMAMALI

Access violation düzeldikten sonra temiz bir `E_FAIL` çıktı:
`Cannot find a resource with the given key: AcrylicBackgroundFillColorDefaultBrush`.

Bu bir eksik dosya sorunu **değil**. WinUI 2 / UWP'de tema sözlüğünü elle merge
etmek gerekiyordu; WinUI 3'te gerekmiyor, varsayılan sözlük zaten yüklü.
`current.Resources(XamlControlsResources{})` mevcut sözlüğü komple değiştiriyor ve
`XamlControlsResources`'ın kendi iç referansları (tam da o Acrylic fırçası) o anda
ortadan kalkmış oluyor. Yani o satır çözüm değil, sorunun kaynağıydı.

**Eleme kaydı** — PRI dosyası şüphesi tek tek test edildi ve elendi:

| Deneme | Sonuç |
|---|---|
| Sadece `spike.pri` | E_FAIL |
| `resources.pri` eklendi (uygulamanın kendi PRI'si) | E_FAIL |
| Framework'ün `Microsoft.UI.Xaml.Controls.pri`'si exe yanına | E_FAIL |
| `resources.pri` = framework Controls.pri | E_FAIL |
| **`XamlControlsResources` satırı kaldırıldı** | **ÇALIŞTI** |

`makepri merge` da denendi — Windows SDK 10.0.26100'de `merge` komutu yok, zaten
gereksizdi.

## Teşhis aracı: vectored exception handler

`0xC0000005` tek başına hiçbir şey anlatmıyor. `spike.cpp`'deki `CrashProbe`,
SEH'ten önce çalışıp hata adresini, modülü ve erişilmeye çalışılan adresi yazdırıyor.
Erişilen adres `0x0` ise suçlu bellidir: null pointer üzerinden sanal metot çağrısı.

Bu fonksiyon spike'a özgü değil — ana projede de benzer bir çökme olursa aynı desen
işe yarar.

## Build tarafında çözülen sorunlar

Bunlar ana projeye WinUI eklenirken de gerekecek:

1. **`ResolveNuGetPackageAssets` patlaması** — C++ vcxproj'da `PackageReference`
   kullanınca `Microsoft.NuGet.targets` içindeki eski task "Sıra hiçbir öğe
   içermiyor" ile düşüyor. Çözüm: `<ResolveNuGetPackages>false</ResolveNuGetPackages>`.

2. **`Microsoft.WindowsAppRuntime.Bootstrap.lib` bulunamıyor** — SDK alt paketlere
   bölünmüş. Lib `microsoft.windowsappsdk.foundation/<sürüm>/lib/native/x64/` altında;
   paketin `.props`'u include yolunu ekliyor ama **lib yolunu eklemiyor**. Elle
   `AdditionalLibraryDirectories` gerekiyor.

   Dikkat: foundation'ın sürümü ana paketten **farklı** ve transitive seçiliyor
   (`1.8.250916003` → `1.8.250906002`). `spike.vcxproj`'da `WasdkFoundationVersion`
   property'si olarak duruyor; ana paket sürümü değişirse bu da güncellenmeli.
   (Glob ile otomatikleştirmek denendi: `ItemDefinitionGroup` içinde item listesi
   kullanmak yasak — MSB4164.)

3. **`UseWinUI=true`** gerekiyor; WinUI hedeflerini ve `.pri` üretimini devreye sokuyor.

4. **C3779 — eksik include'lar.** C++/WinRT'de "auto döndüren işlev tanımlanmadan
   kullanılamaz" hatası her zaman eksik bir header demek:
   - `IVector<T>::Append` → `winrt/Windows.Foundation.Collections.h`
   - `Button::Click` → `winrt/Microsoft.UI.Xaml.Controls.Primitives.h`

5. **`C4002` `GetCurrentTime`** — `windows.h` bunu makro yapıyor,
   `Microsoft.UI.Xaml.Media.Animation`'da aynı isimde metot var. Ana projede
   `TreatWarningAsError` açık olduğu için **hata** olur. XAML header'larından önce
   `#undef GetCurrentTime`.

6. **`MSB8027` / `LNK4042`** — `WindowsAppRuntimeAutoInitializer.cpp` iki ayrı
   paketten geldiği için iki kez derleniyor. Sadece uyarı, çıktı doğru. Ana projeye
   taşınırken `TreatWarningAsError` yüzünden bastırılması gerekebilir.

## Tasarıma etkisi

Spec'teki üç thread'li mimari **aynen geçerli**:

- Render thread (main, MTA) — D3D11/DXGI/overlay
- Input thread — low-level hook'lar
- GUI thread (STA) — `DesktopWindowXamlSource` ile XAML island

Bir kısıt netleşti: `Application::Start` mesaj loop'unu kendisi işletiyor ve
uygulama kapanana kadar dönmüyor. Yani GUI thread'e iş geçirmek için
`PostThreadMessage` yerine `DispatcherQueue.TryEnqueue` kullanmak gerekiyor.
GUI→motor yönü etkilenmiyor (mesaj penceresine `PostMessage`, spec'te olduğu gibi).

## Sonraki adım

Plan Task 5'ten devam edilebilir. Task 2/3/4 zaten tamamdı ve bu bulgudan
etkilenmiyor.
