# BetterMagnifier Kontrol Paneli — Tasarım

**Tarih:** 2026-08-04
**Durum:** Onaylandı, uygulaması kısmen yapıldı

> **Uygulama durumu (2026-08-08):** Panelin motor tarafı hazır — mesaj kontratı
> (`AppMessages.h`), lock-free durum aktarımı (`StatusSnapshot.h`), INI ayar deposu
> (`SettingsStore`) ve ayrı input thread'i (bölüm 3.1'deki latent bug) uygulandı.
> GUI'nin kendisi (XAML Islands) hâlâ bölüm 9'daki spike'ı bekliyor. Ayrıntı ve
> sapmalar: `docs/superpowers/plans/2026-08-04-control-panel-gui.md` → "Uygulama Durumu".

## 1. Amaç

BetterMagnifier'a küçük, modern bir kontrol paneli eklemek. İki işi var:

1. **Durum sekmesi** — her monitörün zoom seviyesi, aktiflik, freeze, capture sağlığı ve FPS'i canlı gösterir; slider ile zoom anında değiştirilir.
2. **Ayarlar sekmesi** — hotkey atama, takip modu, zoom sınırları, başlangıç davranışı.

Panel opsiyonel bir yüzeydir. Magnifier çekirdeği panel olmadan da, hatta Windows App Runtime kurulu olmadan da tam çalışır.

### Kapsam dışı (bilinçli)

- **Render kalitesi ayarları** (ölçekleme filtresi, v-sync, smooth zoom hızı) — shader pipeline (Adım 4) gelmeden anlamsız. O iş bitince ayrı bir tur.
- **Metin imleci (caret) takibi** — UI Automation `TextPattern` gerektiriyor, uygulama bazında tutarsız çalışıyor, Windows Magnifier'da da en sık bozulan özellik.
- **Birim test altyapısı** — `SettingsStore` dışında test edilebilir saf mantık yok; framework kurmak kapsamı ikiye katlar.

## 2. Kısıt: MTA / STA çakışması

`src/main.cpp` ana thread'i `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` ile MTA yapıyor. XAML ise UI thread'inde STA istiyor (`winrt::init_apartment(winrt::apartment_type::single_threaded)`).

Ana thread'i STA'ya çevirmek seçenek değil: render loop'u `Present(vSync=true)` ile blokluyor, XAML dispatcher'ı aç kalır ve panel her frame donar.

Sonuç: GUI kendi STA thread'inde yaşar.

Ayrıca standalone bir WinUI 3 `Window` de seçenek değil — WinUI 3'te `Application::Start()` çağıran thread'in mesaj loop'una sahip olur, bizim thread'lerimizin kendi loop'ları var. Mevcut Win32 uygulamasına XAML gömmenin yolu **XAML Islands** (`DesktopWindowXamlSource`, `Microsoft.UI.Xaml.Hosting`).

## 3. Mimari — üç thread

```
┌─ RENDER THREAD (main, MTA) ────────────────────────────────┐
│  App::Run()  D3D11 device · swap chains · DXGICapture      │
│  overlay pencereleri · mesaj penceresi · RegisterHotKey     │
└────────────────────────────────────────────────────────────┘
      ▲ PostMessage                    │ atomic write
      │ (WM_APP_*)                     ▼
┌─────┴───────────────┐    ┌───────────────────────────────┐
│ INPUT THREAD        │    │  StatusSnapshot               │
│ WH_KEYBOARD_LL      │    │  (lock-free atomic'ler)       │
│ WH_MOUSE_LL         │    └───────────────────────────────┘
│ kendi GetMessage    │                    │ 10 Hz poll
└─────────────────────┘                    ▼
                          ┌─────────────────────────────────┐
      PostMessage         │ GUI THREAD (STA)                │
      ◄─────────────────  │ DispatcherQueueController       │
                          │ host HWND + DesktopWindowXaml-  │
                          │ Source · kendi GetMessage       │
                          └─────────────────────────────────┘
```

### 3.1 Input thread neden ayrı

Low-level hook'lar, onları kuran thread'in mesaj kuyruğunda çağrılır. Mevcut kod `WH_MOUSE_LL`'i ana thread'e kuruyor (`HotkeyManager::StartMouseHook`, `App::InitializeComponents` üzerinden). Ana thread render loop'unu çalıştırıp `Present(vSync)` ile blokladığı için **sistemdeki her fare olayı bizim frame'imizin arkasında kuyruğa giriyor**. Windows'un `LowLevelHooksTimeout`'u (varsayılan 300 ms) aşılırsa hook sessizce devre dışı bırakılıyor.

Bu, panel işinden bağımsız, hâlihazırda var olan bir kusur. Klavye hook'u eklemek (`Win+Z`'yi ele geçirmek için) bunu ikiye katlayacağı için input thread'i bu iş kapsamında ayırıyoruz.

### 3.2 Veri akışı kontratı

Render hot path'inde kilit yok. Üç yön:

| Yön | Mekanizma | Frekans |
|---|---|---|
| GUI → motor, ayar değişimi | `SettingsStore` yaz, sonra `WM_APP_SETTINGS_CHANGED` postala | nadir |
| GUI → motor, interaktif zoom | `WM_APP_SET_ZOOM`, `wParam`=monitör indeksi, `lParam`=zoom×1000 | slider sürüklerken |
| Motor → GUI, canlı durum | `StatusSnapshot` atomic yazma / GUI `DispatcherTimer` ile okuma | yazma her frame, okuma 10 Hz |

Tüm GUI→motor iletişimi mevcut mesaj penceresine `PostMessage`. Tüm motor→GUI iletişimi lock-free atomic snapshot. `MonitorManager` tek sahipli (render thread) kalır.

10 Hz okuma bilinçli: ayar panelinde 60 Hz gerekmez, çekişme sıfıra iner.

### 3.3 GUI thread yaşam döngüsü

Panel ilk açılışta tembel oluşturulur. Kapatılınca **thread yaşamaya devam eder** — Windows App SDK 1.5'ten beri Islands (`DesktopWindowXamlSource`) için varsayılan davranış bu: son XAML penceresi kapanınca thread'in event loop'u otomatik çıkmıyor (`Application.DispatcherShutdownMode`). Panel aç-kapa maliyeti sıfır.

## 4. Bileşenler

### Yeni dosyalar

| Dosya | Sorumluluk | Bağımlılık |
|---|---|---|
| `src/SettingsStore.h/.cpp` | INI oku/yaz, tipli erişimciler, varsayılanlar. Saf mantık, UI yok | Win32 profile API |
| `src/StatusSnapshot.h` | Motor→GUI atomic snapshot. Header-only, POD | `<atomic>` |
| `src/InputThread.h/.cpp` | İki hook + kendi mesaj loop'u; olayları mesaj penceresine postalar | — |
| `src/ControlPanel.h/.cpp` | GUI thread: STA init, bootstrapper, host HWND, XAML island, loop, yaşam döngüsü | Windows App SDK, C++/WinRT |

### Mevcut dosyalarda değişiklik

- `HotkeyManager` — hook kodu `InputThread`'e taşınır. `RegisterHotKey` kalır (pencereye bağlı, render thread'de olmalı).
- `App` — `SettingsStore`, `InputThread`, `ControlPanel` üyeleri; `MessageWndProc`'a `WM_APP_*` işleyicileri; `Update()` her frame `StatusSnapshot` yazar.
- `TrayIcon` — menüye "Ayarlar" maddesi.
- `main.cpp` — değişmez, MTA kalır.

### Ayar deposu

`%APPDATA%\BetterMagnifier\settings.ini`, Win32 `WritePrivateProfileStringW` / `GetPrivateProfileIntW` ile. Sıfır bağımlılık, parser yazmak yok, elle düzenlenebilir. Per-monitor ayarlar doğal olarak section olur:

```ini
[General]
ToggleHotkey=Ctrl+Alt+Z
FreezeHotkey=Ctrl+Alt+X
HijackWinZ=0
FollowMode=MouseAndFocus   ; izinli degerler: Mouse | MouseAndFocus
StartWithWindows=0
RememberZoomLevel=1

[\\.\DISPLAY1]
MinZoom=1.0
MaxZoom=10.0
ZoomStep=0.25
LastZoom=2.5
```

## 5. Kararlar ve gerekçeleri

### 5.1 GUI teknolojisi: WinUI 3 / XAML Islands

Değerlendirilen alternatifler:

- **Dear ImGui + mevcut D3D11** — mevcut render pipeline'a oturuyordu, canlı durum sekmesi bedava gelirdi, çalışma zamanı bağımlılığı yoktu. Reddedildi: yerel Windows görünümü değil.
- **Saf Win32 controls** — sıfır bağımlılık, en küçük diff. Reddedildi: comctl32 kontrolleri dark mode desteklemiyor; modern görünmesi için owner-draw yazmak ImGui vendor'lamaktan fazla iş.
- **WinUI 3 / XAML Islands** — seçildi. Gerçek Fluent görünüm. Bedeli deployment (bkz. 5.4).

### 5.2 UI markup değil, imperatif C++/WinRT kurulum

Derlenmiş XAML (`.xaml` dosyaları) vcxproj'a WinUI XAML compiler target'ları eklemeyi gerektiriyor; Islands senaryosunda düz bir Win32 vcxproj'da sancılı yol. Panelde ~15 kontrol var, kodla kurmak yönetilebilir.

Fluent görünümü kaybedilmiyor: island'ın kaynaklarına `XamlControlsResources` yüklenince varsayılan WinUI stilleri devreye giriyor.

### 5.3 Windows App SDK minimum 1.5

Islands 1.4'te deneysellikten çıktı, ama 3.3'teki davranışa (thread loop'unun otomatik çıkmaması) bel bağlıyoruz — o 1.5'te geldi. Kesin sürüm planın ilk adımında NuGet restore ile pinlenir; hedef, uygulama anındaki en güncel sürüm.

### 5.4 Çekirdek, runtime'a bağımlı olmaz

Paketlenmemiş uygulama + Windows App SDK, kullanıcı makinesinde `Microsoft.WindowsAppRuntime.Redist` kurulu olmasını şart koşuyor; herhangi bir WinRT/XAML tipine dokunmadan önce bootstrapper çağrılmalı.

Sert kural: **magnifier çekirdeği runtime yokken de tam çalışır.** Bootstrapper init, panel ilk açıldığında tembel çalışır. Başarısız olursa log + MessageBox ("ayar paneli için Windows App Runtime gerekli"); tray toggle ve hotkey'ler çalışmaya devam eder.

Yan etki: projeye NuGet giriyor, `msbuild` artık `/restore` ile çağrılmalı. Build talimatı değişiyor.

### 5.5 `Win+Z` ele geçirme opt-in

`Win+Z` kernel korumalı değil (`Ctrl+Alt+Del` gibi) ve Winlogon'a bağlı değil (`Win+L` gibi), yani `WH_KEYBOARD_LL` içinden `return 1` ile yutulabilir. Ama yutmak, uygulama çalışırken Snap Layouts'u öldürür.

Varsayılan kapalı. Ayarda checkbox, yanında ne kaybedildiğini söyleyen açıklama. `Ctrl+Alt+Z` varsayılan kalır.

Admin olmadan yüksek integrity'li pencere odaktayken (Task Manager, UAC) hook devreye girmez. DXGI Desktop Duplication da secure desktop'ta çalışmadığı için sınır zaten orada — tutarlı, ek kısıt getirmiyor.

## 6. Panel içeriği

Host pencere: `WS_OVERLAPPEDWINDOW`, ~520×640, koyu başlık çubuğu (`DWMWA_USE_IMMERSIVE_DARK_MODE`), XAML island client alanını kaplar. Resize var, maximize yok.

### Durum sekmesi

```
┌─ BetterMagnifier ────────────────────── ─ □ ✕ ┐
│  [ Durum ]  Ayarlar                            │
├────────────────────────────────────────────────┤
│  DISPLAY1  2560×1440 · 75Hz · %125   PRIMARY   │
│  ● Aktif    ▬▬▬▬▬●▬▬▬▬  2.50×    72 FPS        │
│  Capture: OK          [Freeze]                 │
│                                                │
│  DISPLAY2  1920×1200 · 144Hz · %125            │
│  ○ Pasif    ▬▬●▬▬▬▬▬▬▬  1.00×      — FPS       │
│  Capture: OK          [Freeze]                 │
│                                                │
│  DISPLAY3  1920×1080 · 75Hz · %100             │
│  ○ Pasif    ▬▬●▬▬▬▬▬▬▬  1.00×      — FPS       │
│  Capture: OK          [Freeze]                 │
└────────────────────────────────────────────────┘
```

Monitör başına bir kart: cihaz adı, çözünürlük, refresh rate, DPI ölçeği, primary etiketi; aktiflik toggle'ı, zoom slider'ı, sayısal zoom, FPS, capture durumu, freeze düğmesi.

Monitör listesi `WM_DISPLAYCHANGE` ile yeniden kurulur. `App::OnDisplayChange` GUI thread'e de haber verir.

### Ayarlar sekmesi

- Hotkey atama: toggle, freeze. Çakışma varsa (`RegisterHotKey` başarısız) kırmızı uyarı satırı.
- "Win+Z'yi ele geçir" checkbox'ı — varsayılan kapalı, Snap Layouts uyarısıyla.
- Takip modu radio: fare / fare + klavye odağı.
- Zoom min, max, adım.
- "Windows ile başlat".
- "Zoom kapanınca seviyeyi hatırla".

### Klavye odağı takibi

`SetWinEventHook(EVENT_OBJECT_FOCUS)` input thread'e oturur. Odak değişince focal point o pencereye kayar. Caret takibi kapsam dışı (bkz. 1).

## 7. Hata yönetimi

| Durum | Davranış |
|---|---|
| Hotkey kaydı başarısız | GUI'de kırmızı satır + log (şu an sadece log) |
| Capture `ACCESS_LOST` | Snapshot `captureOk=false` → panelde "Capture: yeniden bağlanıyor" |
| Bootstrapper başarısız | Log + MessageBox; çekirdek çalışmaya devam eder |
| GUI thread çöker | Render thread etkilenmez — ayrı thread, paylaşılan mutable state yok |
| `SetWindowDisplayAffinity` başarısız | Log uyarısı (Windows 10 2004 öncesi) + durum sekmesinde monitör kartında "Capture: feedback riski" satırı |

## 8. Test

- `SettingsStore` — `assert` tabanlı `demo()` self-check: INI yaz→oku→değer eşleşiyor; eksik dosya varsayılana düşüyor; bozuk değer varsayılana düşüyor.
- Threading ve XAML — manuel doğrulama + log. Birim test altyapısı yok, kurmak kapsamı ikiye katlar.

## 9. Açık risk — planın ilk adımı bunu kapatır

`DesktopWindowXamlSource`'un **ikincil bir STA thread'de** çalıştığı dokümanda dolaylı olarak doğrulandı (per-thread XAML runtime, `DispatcherShutdownMode`'un thread bazlı olması) ama birebir "secondary thread" ifadesi bulunamadı.

Planın ilk işi bir spike olmalı: boş bir XAML island'ı ayrı STA thread'de ayağa kaldır, ana thread MTA kalsın, ikisi birlikte çalışsın. Çalışıyorsa tasarım geçerli. Çalışmıyorsa geri dönüş yolu: paneli ayrı bir process'e taşımak (named pipe veya `WM_COPYDATA` ile IPC) ya da GUI teknolojisini yeniden değerlendirmek.

## 10. Uygulama sırası (özet)

1. **Spike:** ikincil STA thread'de XAML island (bölüm 9)
2. `SettingsStore` + self-check
3. `InputThread` — mevcut hook'ları taşı, latent bug'ı kapat
4. `StatusSnapshot` + `App::Update()` yazma tarafı
5. `ControlPanel` iskelet: host pencere, bootstrapper, island, boş iki sekme
6. Durum sekmesi — monitör kartları, canlı okuma, zoom slider
7. Ayarlar sekmesi — hotkey atama, takip modu, diğerleri
8. `EVENT_OBJECT_FOCUS` klavye odağı takibi
9. `Win+Z` ele geçirme (`WH_KEYBOARD_LL`), opt-in

Detaylı plan ayrı dosyada.
