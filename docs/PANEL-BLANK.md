# Kontrol paneli — teshis notlari

**Son guncelleme:** 2026-08-07
**Durum:** Tam agac (monitor kartlari + ayarlar sekmesinin tamami) artik
cokmeden aciliyor ve dogrulandi — ekran goruntusuyle. Panel yine de
**varsayilan olarak KAPALI**, `BM_PANEL=1` ile aciliyor: gercek etkilesim
(slider/checkbox degisikliklerinin ayarlara yazilip yazilmadigi, restart
sonrasi kalicilik) henuz elle test edilmedi. Cekirdek magnifier bundan
etkilenmiyor; tepsi menusunde de "Settings..." maddesi ancak switch aciksa
gorunuyor.

Iki ayri hata vardi, ikisi de ayni kok sebepten: cozuldu.

---

## 1. COZULDU — island hic canlanmiyordu (bos beyaz pencere)

**Belirti:** Pencere aciliyordu, koyu baslik cubugu dogruydu, istemci alani
tamamen beyazdi. Butun XAML cagrilari istisna atmadan geciyordu.

**Sebep:** `WindowsXamlManager::InitializeForCurrentThread()` cagrilmiyordu.

`Application::Start` bir mesaj loop'u isletiyor ama bu thread'e XAML runtime'i
kurmuyor. Sonuc: `Content()` kabul ediliyor, `XamlRoot` bile olusuyor, ama hicbir
layout ya da render pass'i calismiyor.

**Cozum** — `Application::Start` tamamen birakildi, belgelenmis Islands deseni:

```cpp
winrt::init_apartment(single_threaded);
auto controller   = DispatcherQueueController::CreateOnCurrentThread();
auto xamlManager  = WindowsXamlManager::InitializeForCurrentThread();   // EKSIK OLAN
BuildUi();
// kendi GetMessage loop'umuz
```

Yan faydalar: mesaj loop'u bize ait oldugu icin `PostThreadMessage` tekrar
kullanilabilir, ve `Application::Start`'in "process basina bir kez" kisiti kalkti.

**Elenen hipotezler** (bunlari tekrar deneme):

| Hipotez | Sonuc |
|---|---|
| PMv2 DPI farkindaligi | **Sebep degil.** `main.cpp`'deki cagriyi atlamak DPI'i degistirmiyor — WinAppSDK auto-initializer zaten kuruyor, spike'ta da oyleydi |
| Eksik `resources.pri` | Cikti klasorunde sadece `BetterMagnifier.pri` var, **spike'ta da oyleydi** |
| Yanlis runtime surumu | `Microsoft.WindowsAppRuntime.1.8` `8000.921.1539.0` kurulu, FINDINGS.md'nin calistigini soyledigi surumun aynisi |
| `XamlControlsResources` | Hala elle ATANMAMALI, o kisim dogruydu |
| Sert oldurmenin (Stop-Process) sonraki acilisi bozmasi | Temiz tek acilista da ayni sonuc |

**Teshiste ise yarayan olcum:** `root.XamlRoot()`. Null ise agac hicbir island'a
bagli degil; doluysa bagli ve sorun baska yerde. Yaninda `root.Loaded`,
`root.SizeChanged` ve 10 Hz `DispatcherTimer` tick log'lari — ucu birden sessizse
XAML o thread'de hic calismiyor demektir.

---

## 2. COZULDU — metin-girisli kontroller butun process'i olduruyor

**Belirti:** Panel aciliyor, ilk layout pass'inde process
`0xC000027B` (`STATUS_STOWED_EXCEPTION`) ile oluyor. Log'da hicbir sey yok.

**Bisect sonucu (kesin, iki ayri tur icin):**

| Icerik | Sonuc |
|---|---|
| Sadece monitor kartlari (Border, Grid, TextBlock, ToggleSwitch, Slider, ToggleButton) | **Ayakta**, Loaded + SizeChanged + dispatcher tick calisiyor |
| Ayarlar bolumu, baslik + **2 adet `TextBox`** | **Cokuyor** |
| Ayarlar bolumu, `TextBox` cikarilmis, **CheckBox + RadioButton** kadar | **Ayakta** |
| Yukaridakine + 3 adet `NumberBox` (zoom limitleri) | **Cokuyor** |
| Sadece **1 adet `NumberBox`**, baska hicbir sey yok | **Cokuyor** |

Suclular: `TextBox` ve `NumberBox`. Ikisi de metin girisi barindiran kontroller
— `NumberBox` iceride bir `TextBox` gomuyor, cokme sebebi ayni. Ayni agactaki
`CheckBox`, `RadioButton`, `Slider`, `Button`, `ToggleSwitch`, `ToggleButton`
sorunsuz. Sebep: paketlenmemis + yukseltilmis bir process'te, ikincil STA
thread'deki island icin metin girisi servisleri (TSF/IME) ayaga kalkmiyor.

**Yapilan duzeltme:**
- Hotkey'ler panelde salt okunur gosteriliyor; duzenleme `settings.ini`
  uzerinden, panelin "Reload from disk" butonuyla.
- Zoom limitleri (Minimum/Maximum/Step) artik `NumberBox` degil `Slider` +
  yaninda deger gosteren bir `TextBlock` — `RebuildMonitorCards()`'taki
  per-monitor zoom slider'iyla ayni, zaten dogrulanmis desen.

**Dogrulama:** Tam agac (monitor kartlari + ayarlar sekmesinin tum
bolumleri: Hotkeys, Follow mode, Zoom limits, Other, Settings file) acildi,
`Panel root Loaded` / `SizeChanged` / `Panel dispatcher is ticking` uclu
log'u geldi, process ayakta kaldi, ve ekran goruntusuyle gorsel olarak
dogru render edildigi teyit edildi.

**Hala acik:** Etkilesimli dogrulama yapilmadi — slider suruklemek/checkbox
tiklamak gercekten `PushSettings()`'i tetikliyor mu, deger `settings.ini`'ye
yaziliyor mu, restart sonrasi kaliyor mu. Otomasyondan erisilemiyor (UIPI),
elle bakilmasi gerekiyor.

**Ileride, hotkey girisi gerekirse:** `TextBox` yerine, zaten sahip oldugumuz
`WH_KEYBOARD_LL` hook'uyla "tusa bas" yakalama. Daha iyi UX, yeni kontrol yok.

---

## Tasarim degisikligi: TabView gitti

Panel artik tek kaydirilabilir sayfa. Iki sekme orijinal plandi ama `TabView`
agir sablonlu bir kontrol ve bu boyuttaki bir pencerede ayni bilgiyi tek sayfa
tasiyor. (Not: TabView'in kendisi cokme sebebi degildi, o `TextBox`'ti.)

---

## Ortam tuzaklari (cozuldu, tekrar kesfedilmesin)

**Yukseltilmis process ortam degiskenini miras ALMIYOR.** UAC ile yukseltilen
process, baslatanin bellekteki environment'ini degil, kullanici registry'sinden
kurulan temiz bir tanesini aliyor. `$env:BM_X='1'; Start-Process ...` hicbir sey
yapmiyor. Butun `BM_*` switch'leri kalici yazilmali:

```powershell
[Environment]::SetEnvironmentVariable('BM_PANEL', '1', 'User')
```

**Uygulamanin penceresine script'ten ulasilamiyor.** `PostMessage` UIPI'ye
takiliyor. `FindWindow` bu makinede uygulamanin penceresini hic bulamadi;
`EnumWindows` buldu. `BM_PANEL=1` paneli acilista actigi icin test bunu kullaniyor.

**Ekran yakalayan script DPI aware olmali**, yoksa `GetWindowRect`
sanallastirilmis koordinat donuyor ve `CopyFromScreen` kayiyor:

```powershell
[void][W]::SetProcessDpiAwarenessContext([IntPtr](-4))  # PER_MONITOR_AWARE_V2
```

Tek pencere yerine **tum sanal ekrani** yakalamak daha guvenilir: koordinat
matematigi yok, panel neredeyse orada gorunur.

---

## Panelde duran teshis araclari

| Ne | Nerede |
|---|---|
| `BM_PANEL=1` | `App.cpp`, `PanelEnabled()` — paneli acar, tepsi maddesini gosterir |
| `Panel XamlRoot present/NULL` log'u | `ControlPanel.cpp`, `BuildUi()` |
| `Panel root Loaded` / `SizeChanged` | `ControlPanel.cpp`, `BuildUi()` |
| `Panel dispatcher is ticking` | `ControlPanel.cpp`, `StartLiveTimer()` |
