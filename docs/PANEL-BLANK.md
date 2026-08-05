# Kontrol paneli bos beyaz aciliyor — teshis notlari

**Tarih:** 2026-08-05
**Branch:** `wip/panel-blank-island`
**Belirti:** Panel penceresi aciliyor, baslik cubugu koyu ve dogru boyutta, ama
istemci alani **tamamen beyaz** — hicbir XAML icerigi cizilmiyor. Debug ve
Release ayni.

Kod tarafi calisiyor: `Control panel opened` log'a dusuyor, o satir `BuildUi`'nin
en sonunda. Yani butun XAML cagrilari istisna atmadan gecti.

## Olculenler

Hepsi olculdu, hicbiri tahmin degil.

| Ne | Sonuc |
|---|---|
| Host pencere | Var. `700x850` (125% DPI'da 560x680'in olceklenmisi), koyu baslik cubugu calisiyor |
| Ekran goruntusu | Istemci alani duz beyaz, hicbir kontrol yok |
| `EnumChildWindows(host)` | **Sifir cocuk pencere.** Island'in bridge penceresi hic olusmamis |
| `root.Loaded` | **Hic tetiklenmiyor** — agac canli visual tree'ye hic girmiyor |
| `root.SizeChanged` | Hic tetiklenmiyor — layout slotu hic almiyor |
| `Application::UnhandledException` | Hic tetiklenmiyor — yutulan bir XAML istisnasi YOK |
| `DispatcherTimer` (10 Hz) tick | **Hic tetiklenmiyor** (`Panel dispatcher is ticking` log'a hic dusmuyor) |
| `Application::Start` donusu | Donmuyor — `Control panel closed` log'a hic dusmuyor |
| `BM_PANEL_PROBE=1` (spike'in birebir icerigi: StackPanel + TextBlock + Button, Light tema) | Ayni sekilde bos. **Yani sorun TabView'e ozel degil** |
| Kurulu runtime | `Microsoft.WindowsAppRuntime.1.8` `8000.921.1539.0` + `MicrosoftCorporationII.WinAppRuntime.Main.1.8` — FINDINGS.md'nin calistigini soyledigi surumun aynisi |
| Cikti klasoru | Sadece `BetterMagnifier.pri`, `resources.pri` yok. **Spike de oyleydi**, muhtemelen sebep degil |

## En daraltilmis teshis

`Application::Start` **donmuyor** (yani loop'unda), ama ayni thread'deki
`DispatcherTimer` hic tick atmiyor ve XAML hic layout yapmiyor.

Yani: mesaj loop'u calisiyor gorunuyor ama **o loop XAML dispatcher'ini
isletmiyor**. Island bir compositor'a hic baglanmiyor, o yuzden ne bridge
penceresi olusuyor ne `Loaded` tetikleniyor.

Bu, "hangi kontrol cizilmiyor" sorunu degil; **island hic canlanmiyor**.

## Evde denenecekler — sirayla

### 1. PMv2 DPI farki (en guclu somut ipucu)

Spike ile uygulama arasindaki **olculebilir tek ortam farki**: `src/main.cpp`
process'i per-monitor DPI aware v2 yapiyor, spike hicbir DPI farkindaligi
ayarlamiyor.

Test: `main.cpp`'deki `SetProcessDpiAwarenessContext` cagrisini gecici olarak
kapat, paneli ac. Cizilirse sebep bu ve cozum island'i DPI degisimine karsi elle
yonetmek olur.

### 2. `Application::Start` yerine belgelenmis Islands yolu

Suphe: `Application::Start` bu senaryoda XAML dispatcher'ini bizim thread'e
baglamiyor. Belgelenmis Islands deseni farkli:

```cpp
auto controller = DispatcherQueueController::CreateOnCurrentThread();
auto manager    = WindowsXamlManager::InitializeForCurrentThread();
DesktopWindowXamlSource source{};
source.Initialize(...);
// kendi GetMessage loop'u
```

`WindowsXamlManager::InitializeForCurrentThread()` hic denenmedi ve tam olarak
"XAML runtime'i BU thread'de ayaga kaldir" isini yapiyor. Eksik parca bu olabilir.

Bu yola gecilirse: mesaj loop'u bize ait olacagi icin `DispatcherQueue.TryEnqueue`
yerine tekrar `PostThreadMessage` kullanilabilir, ve `Application::Start`'in
"process basina bir kez" kisiti kalkar.

Dikkat: spike bunu denemis ve `DispatcherQueueController::CreateOnCurrentThread` +
dogrudan `Application app{}` insaasi `RPC_E_WRONG_THREAD` vermis. Ama o denemede
`WindowsXamlManager` YOKTU ve `Application` elle kuruluyordu — bu farkli bir sey.

### 3. `init_apartment` cakismasi

`ThreadMain` once `winrt::init_apartment(single_threaded)` cagiriyor, sonra
`Application::Start`. Start apartment'i kendi kurmak isteyebilir. Spike de ayni
sirayi kullaniyordu, ama denemesi bir satir.

### 4. Spike'i kullanicinin kendi shell'inden tekrar calistir

Bugun ajanin shell'inden calistirildiginda spike **`[2/5] Application::Start
cagriliyor...` satirinda takiliyor** ve callback'e hic girmiyor — yani spike da
su an bozuk gorunuyor, ama uygulamadan **daha erken** bir yerde. Uygulamada
callback giriliyor.

Bu, ajanin shell'inden yapilan spike/uygulama karsilastirmasini guvenilmez
kiliyor. Spike'in hala calistigini kendi shell'inden dogrula: calisiyorsa fark
kodda, calismiyorsa fark ortamda.

## Ortam tuzaklari (bunlar cozuldu, tekrar kesfedilmesin)

**Yukseltilmis process ortam degiskenini miras ALMIYOR.** UAC ile yukseltilen
process, baslatanin bellekteki environment'ini degil, kullanici registry'sinden
kurulan temiz bir tanesini aliyor. Yani `$env:BM_X='1'; Start-Process ...` hicbir
sey yapmiyor. Butun `BM_*` switch'leri artik kalici yazilmali:

```powershell
[Environment]::SetEnvironmentVariable('BM_OPEN_PANEL', '1', 'User')
```

**Uygulamanin penceresine script'ten ulasilamiyor.** `PostMessage` UIPI'ye
takiliyor; `FindWindow` bu makinede yukseltilmemis derlemede bile uygulamanin
penceresini gormedi, `EnumWindows` gordu. Bu yuzden `BM_OPEN_PANEL=1` var:
uygulama paneli kendisi aciyor ve kendi log'undan rapor veriyor.

**Ekran yakalayan script DPI aware olmali.** Olmazsa `GetWindowRect`
sanallastirilmis koordinat donuyor, `CopyFromScreen` fiziksel pikselle
calisiyor, yakalama kayiyor:

```powershell
[void][W]::SetProcessDpiAwarenessContext([IntPtr](-4))  # PER_MONITOR_AWARE_V2
```

## Bu branch'te duran teshis araclari

Hepsi kalsin, olcum yapan seyler bunlar:

| Ne | Nerede |
|---|---|
| `BM_OPEN_PANEL=1` | `App.cpp`, `OpenPanelOnStartup()` — panel acilista acilir |
| `BM_PANEL_PROBE=1` | `ControlPanel.cpp`, `BuildUi()` — spike'in birebir icerigini koyar |
| `Panel root Loaded` / `SizeChanged` log'lari | `ControlPanel.cpp`, `BuildUi()` |
| `Panel dispatcher is ticking` log'u | `ControlPanel.cpp`, `StartLiveTimer()` |
| `XAML unhandled exception` log'u | `ControlPanel.cpp`, `Application::Start` callback'i |

## Panelin geri kalani

Kod tam: iki sekme, monitor kartlari, canli okuma, butun ayarlar, INI yolu ve
diskten yeniden yukleme. Derleme temiz (Debug + Release, warnings-as-error acik).
Eksik olan tek sey island'in cizmesi. Ayrintili tasarim ve bilincli sapmalar:
[`STATUS.md`](STATUS.md) "Control panel" bolumu.
