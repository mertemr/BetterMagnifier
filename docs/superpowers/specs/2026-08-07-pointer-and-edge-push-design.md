# Monitör Bazlı Tam Ekran Büyütme, Kendi İmlecimiz ve Edge-Push Pan — Tasarım

**Tarih:** 2026-08-07
**Durum:** Onaylandı, uygulama planı bekliyor
**Önceki:** [`2026-08-04-control-panel-gui-design.md`](2026-08-04-control-panel-gui-design.md)
**Bağlam:** [`../../STATUS.md`](../../STATUS.md) — açık kararlar 1, 2, 3, 4, 5 bu spec ile kapanıyor

## 1. Amaç

Üç şeyi birlikte çözmek:

1. **Popup'lar doğru büyütülsün.** Menüler, dropdown'lar ve bağlam menüleri şu anda ya canlı ama büyütülmemiş (ve çift görünüyor) ya da büyütülmüş ama donuk. İkisi de kabul edilemez.
2. **Kenar-itmeli pan.** Görünüm imleç içeride gezerken sabit dursun, yalnızca imleç kenar bandına girince kaysın — native Magnifier'ın davranışı, ama **monitör bazlı** ve doğru tıklama hizasıyla.
3. **Kontrol paneli varsayılan açık** ve yeni ayarları sunuyor.

Değişmeyen kısıt: **monitör başına bağımsız zoom.** Bu projenin var oluş sebebi ve her kararın üstünde.

### Kapsam dışı (bilinçli)

- **UAC secure desktop (mavi onay ekranı).** Hiçbir üçüncü parti uygulama oraya çizemez, hiçbir ayrıcalıkla. Windows Magnifier bile "uzanmıyor" — winlogon o masaüstünde ayrı bir instance başlatıyor ve bu liste sabit kodlu. Kalıcı olarak kapsam dışı.
- **Kernel driver / TrustedInstaller.** Yanlış katman: kompozisyon DWM'de olur ve kernel'den ona per-monitör transform sokulamaz. `TrustedInstaller`/SYSTEM yalnızca dosya, registry ve servis hakkı verir; kompozisyon ACL'e bağlı değil. Tek meşru kernel yolu Indirect Display Driver'dır, o da *yeni sanal* monitör yaratır — var olan fiziksel monitörü dönüştüremez. İşe yarayan ayrıcalık `UIAccess`'tir ve admin ile ilgisi yoktur (bkz. §6).
- **Animasyonlu imleçlerin (`.ani`) kare yürüyüşü.** İlk kare önbelleğe alınır, bekleme çarkı donuk görünür. Nadiren bakılan bir durum.
- **Monitör başına zoom limitleri.** Seçici gerektirir, mevcut global limitler yeterli.
- **Metin imleci (caret) takibi.** Önceki spec'te de kapsam dışıydı, öyle kalıyor.

## 2. Karar: `magnification.dll` tamamen dışlanmıyor

Dışlanan tek şey **`MagSetFullscreenTransform`** — o gerçekten desktop-wide ve per-monitör zoom'u öldürüyor. Ama aynı DLL'deki iki şey kullanılabilir kalıyor:

- **`MagShowSystemCursor`** — gerçek imleci gizlemenin belgelenmiş yolu. §5.3'ün temeli.
- **`WC_MAGNIFIER` kontrolü** — pencere kapsamlı, `UIAccess` istemez, monitör başına bir tane konabilir. Faz 0'ın sonucuna göre değerlendirilecek yedek mimari.

Gerekçe: dışlama gerekçesi ("desktop-wide") yalnızca fullscreen transform'a ait; aynı isim yüzünden ikisini birlikte elemek gerekçeyi taşımadığı yere taşımak olur. Dahası, `magnification.dll` sıfır olursa imleç gizlemenin tek yolu global `SetSystemCursor` kalır — süreç çökerse kullanıcı logoff'a kadar imleçsiz. Bir erişilebilirlik aracında bu takas yanlış tarafa düşüyor.

## 3. Faz 0 — Ölçüm ve mimari karar

STATUS.md kendi tarihini yazmış: son iki mimari karar varsayımla verildi ve ikisi de yanlış çıktı. Bu yüzden render yolu **ölçümle** seçiliyor.

### Ö1 — Popup yakalamada canlı mı? (neredeyse bedava)

Overlay zaten `WDA_EXCLUDEFROMCAPTURE`. Yani yakalama bizi hiç görmüyor; popup altımızda kalsa bile DWM onu kompozisyona canlı katıyor **olmalı**. Eğer öyleyse mevcut "donuyor" teşhisi bir mimari duvar değil, bir hata.

Yöntem: `BM_DUMP_FRAME` + `BM_DUMP_AFTER` mevcut. Overlay en üstteyken bir bağlam menüsü açılır, fare maddeler arasında gezdirilirken ardışık kareler dökülür, **vurgunun karede fareyi takip edip etmediğine** bakılır.

Takip ediyorsa mevcut boru hattında popup sorunu yok demektir; görünen arıza z-order kavgasının kendisidir (`SetWindowPos` menü takibini iptal ediyor olabilir).

### Ö2 — `WC_MAGNIFIER` spike'ı (yalnızca Ö1 başarısızsa)

~200 satır, dört soru:

1. Per-monitör host pencerede canlı, büyütülmüş, **fare takibi çalışan** bir bağlam menüsü çıkıyor mu?
2. `MagSetWindowFilterList(MW_FILTERMODE_EXCLUDE)` Win11'de kendi host'umuzu gerçekten dışlıyor mu, yoksa özyineleme mi alıyoruz?
3. `WS_EX_LAYERED | WS_EX_TRANSPARENT` pencere içinde render ediyor mu? (click-through kaybedilemez)
4. Kare hızı timer'a bağlı; 4K/yüksek tazeleme oranında kabul edilebilir mi?

### Karar kuralı

| Ö1 | Ö2 | Sonuç |
|---|---|---|
| Canlı | çalıştırılmaz | **Yaklaşım 1** — mevcut DXGI/D3D boru hattı korunur |
| Donuk | dördü de temiz | **Yaklaşım 2** — `WC_MAGNIFIER` host + ayrı imleç katmanı |
| Donuk | herhangi biri başarısız | **Yaklaşım 1**, popup modu kullanıcı ayarı olarak kalır |

Sonuç `docs/STATUS.md`'ye yazılır.

`MagShowSystemCursor` testi **Ö2'ye bağlanmaz** — her iki mimaride de gerekli, Faz 1'in ilk işidir.

### Fazlardan bağımsızlık

İmleç boru hattı, girdi ölçekleme, edge-push matematiği ve GUI **mimariden bağımsız**. İkisinde de aynı tasarım, farklı çizim yüzeyi:

- Yaklaşım 1: imleç sprite'ı içerik shader pass'inin ardından aynı swap chain'e çizilir.
- Yaklaşım 2: monitör başına ikinci, ince bir layered pencere yalnızca imleci çizer (`WDA_EXCLUDEFROMCAPTURE`, click-through, mag host'unun üstünde).

## 4. Mimari

Mevcut üç-thread yapısı korunuyor. Dört küçük birim ekleniyor.

```
RENDER THREAD (main, MTA)
  D3D11, per-monitor swap chains, DXGICapture, overlay, mesaj penceresi
  + CursorRenderer  ── sekil onbellegi, sprite cizimi
        ▲ PostMessage                    │ atomic snapshot okuma
        │                                ▼
  INPUT THREAD                     ViewportSnapshot (lock-free)
  WH_MOUSE_LL, WH_KEYBOARD_LL             ▲
  + PointerInput      ── yut, olcekle, SetCursorPos
  + ViewportController ── srcOrigin, edge-push, kirpma
        │
  GUI THREAD (STA)  — degismiyor
```

| Birim | Sahibi | Sorumluluk | Bağımlılık |
|---|---|---|---|
| `ViewportController` | input thread | Monitör başına `srcOrigin` + `zoom`, edge-push matematiği, sınır kırpma | **Yok — saf matematik, Windows'suz** |
| `PointerInput` | input thread | LL hook tarafı: yut, `1/zoom` ile ölçekle, `SetCursorPos` | `ViewportController`, Win32 |
| `CursorRenderer` | render thread | `HCURSOR` → doku önbelleği, hotspot hizası, sprite çizimi | D3D11, Win32 |
| `SystemCursor` | main | Gerçek imleci gizle/geri getir + geri getirme garantileri | `magnification.dll` (yedek: `user32`) |

### Pan neden input thread'de

"İtme kadarı" modeli her fare olayına oransal olmalı. `srcOrigin` render thread'de güncellenirse pan hızı kare hızına bağlanır — 60 Hz'de bir tempo, 144 Hz'de başka. Bu yüzden `V_f` ve `srcOrigin` olayla birlikte input thread'de ilerler; render thread yalnızca atomik snapshot okur. Projenin `StatusSnapshot` deseninin aynısı.

Zoom değişiklikleri (hotkey ve panel) controller'a mesajla iletilir.

### Anchor değişmezi emekli oluyor

`srcOrigin = anchor·(1 − 1/zoom)` özdeşliği tıklama hizasını korumak için vardı ve görünümü imlece bağlamak zorunda kalıyordu. Artık hizayı **gerçek imlecin `round(V_f)`'te durması** sağlıyor. Sonuçlar:

- Görünüm imleçten bağımsız hareket edebilir → edge-push mümkün.
- Hiza her takip modunda korunur → `FollowMode::MouseAndFocus`'un "tıklamalar hizalanmıyor" bedeli ortadan kalkar.

## 5. Faz 1 — İmleç boru hattı

### 5.1 Sanal imleç ve girdi ölçekleme (`PointerInput`)

Durum: `V_f` (float, sanal masaüstü koordinatı — **otorite budur**) ve `lastSetReal` (en son `SetCursorPos`'a verilen nokta).

`LowLevelMouseProc`, yalnızca `WM_MOUSEMOVE` için:

1. Olay injected ise: konum `lastSetReal` ise **kendi yankımız**, sessizce düşür. Değilse yabancı bir `SetCursorPos` (oyun, RDP, `Ctrl+Alt+Del` dönüşü) → **resync**: `V_f = P`, geçir.

   > Bu, mevcut davranışı değiştiriyor. Şu anda `LLMHF_INJECTED` olayları toptan yok sayılıyor (`BM_ALLOW_INJECTED=1` ile açılıyor), çünkü otomasyonun uygulamayı sürüklemesi istenmiyordu. Artık **kendi** `SetCursorPos`'umuz da injected geliyor, dolayısıyla toptan yok sayma mümkün değil. Yeni kural: injected hareket olayları yukarıdaki eşleşmeye göre ayrıştırılır; injected **tuş ve düğme** olayları eskisi gibi yok sayılmaya devam eder. `BM_ALLOW_INJECTED` anlamını korur.
2. `round(V_f)`'i içeren monitörde `zoom == 1` → dokunma, `V_f = P`, `return 0`. **Büyütme yokken sıfır maliyet.**
3. `delta = P − lastSetReal` → `V_f += delta · speed / zoom`. Float birikim; alt-piksel el hareketi kaybolmaz.
4. `ViewportController::OnPointerMoved(m, V_f)` → gerekiyorsa `srcOrigin` itilir, `V_f` geri verilir.
5. `SetCursorPos(round(V_f))`, `lastSetReal` güncellenir, snapshot yayınlanır, **`return 1`** (orijinali yut).

**Tuşlar ve tekerlek yutulmaz** — yalnızca hareket. Tıklama `round(V_f)`'e gider ve bu, kullanıcının sprite'ın altında gördüğü kaynak pikselin ta kendisidir. Hiza garantisi inşadan gelir, düzeltmeden değil.

`speed` çarpanı varsayılan `1.0`; el hareketi ekranda 1:1 hissedilir, zoom ne olursa olsun aynı hız.

**Riskler:**

- **`LowLevelHooksTimeout` (300 ms).** Adım 3–5 aritmetik + tek `SetCursorPos` syscall'ı. Debug'da p99 loglanır, eşik ~1 ms. Aşılırsa yedek: `SetCursorPos`'u input thread'in mesaj döngüsüne al — bir kuyruk sıçraması gecikme, kare bağımsızlığı korunur.
- **Sistem genelinde her hareketi yutmak** bu uygulamanın yapacağı en ağır iş. Adım 2'deki erken çıkış bunu "yalnızca büyütürken"e sınırlar.
- **Ham girdi tüketicileri** (oyunlar, 3B görünümler) OS'un verdiği delta'yı okur, biz onu bastırdık. Bilinen sınırlama; o monitörde zoom kapatmak anında normale döndürür, ayrıca `pointerScaling` ayarı native davranışa düşürür.

### 5.2 İmleç şekli ve çizimi (`CursorRenderer`)

`GetCursorInfo` her karede okunur (ucuz). Önbellek: `HCURSOR → (doku, hotspot, boyut)`.

- **Monokrom imleçler ele alınmak zorunda.** `hbmColor == NULL`, `hbmMask` iki kat yükseklikte: üst yarı AND, alt yarı XOR. I-beam monokromdur ve metin içinde en çok görülen imleçtir; atlanırsa özellik yarım kalır.
- Renkli imleçlerde `hbmColor` 32-bit DIB; gerçek alfa kanalı varsa o kullanılır, yoksa `hbmMask` AND maskesi olarak.
- Çizim konumu: `(V_f − srcOrigin)·zoom − hotspot·cursorScale`, boyut `iconSize · cursorScale`.
- `cursorScale` varsayılanı `zoom`; ek çarpanla büyütülebilir — az gören kullanıcılar imleci içerikten *daha* büyük ister.
- İçerik bilinear, **imleç point sampling**. Büyütülmüş bir okun keskin kenarlı olması bulanık olmasından iyi okunur; ayrı sampler.
- `ci.flags == 0` → imleç zaten gizli (metin alanına yazarken, oyunlarda). Hiçbir şey çizme.

### 5.3 Gerçek imlecin gizlenmesi (`SystemCursor`)

Birincil: `MagInitialize()` + `MagShowSystemCursor(FALSE)`.
Yedek: `SetSystemCursor` ile boş imleç, her standart `OCR_*` kimliği için.

Geri getirme garantileri — bir erişilebilirlik aracında **kalıcı görünmez imleç** mümkün olan en kötü arıza:

1. Gizleme **yalnızca en az bir monitörde `zoom > 1` iken**. Zoom kapanınca anında geri gelir; maruziyet penceresi tam olarak "aktif büyütme".
2. `SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0)` tek güvenilir geri alma. Şuralardan çağrılır: normal çıkış, `WM_QUERYENDSESSION`, `WM_ENDSESSION`, `Ctrl+Alt+Shift+Q` panik çıkışı, `SetConsoleCtrlHandler`, `std::set_terminate`.
3. `WTS_SESSION_LOCK` → geri getir. Mevcut `WTSRegisterSessionNotification` altyapısı hazır.

**Kapı kuralı.** Süreç Task Manager'dan öldürülürse temizlik çalışmaz. `MagShowSystemCursor` durumu süreçle birlikte öldüğü için o yolda sorun yok; `SetSystemCursor` yedeğinde imleç asılı kalır. Bu yüzden:

> `MagShowSystemCursor` çalışmıyorsa **imleç kompozisyonu varsayılan olarak açılmaz.** Ayar arkasında, uyarı metniyle kalır; edge-push native imleç davranışına (zoom katı hız, yutma yok) düşer.

Bu ölçüm Faz 1'in ilk işidir.

## 6. Faz 1b — UIAccess

Üç şart: manifestte `uiAccess="true"`, makinenin `LocalMachine\Root` deposundaki bir köke zincirlenen Authenticode imzası, `%ProgramFiles%` altında kurulum.

`tools/install-uiaccess.ps1` (+ `uninstall-uiaccess.ps1`): sertifika yoksa üretir, `LocalMachine\Root` ve `TrustedPublisher`'a ekler, `Set-AuthenticodeSignature` ile imzalar, `C:\Program Files\BetterMagnifier\` altına kopyalar. Yalnızca PowerShell — signtool/SDK bağımlılığı yok. Bir kez, admin ile.

**Çalışma anında tespit**, derleme yapılandırmasıyla değil: `GetTokenInformation(TokenUIAccess)`. Aynı ikili hem `bin/`'den (dev, UIAccess yok) hem `Program Files`'tan (kurulu) çalışacak.

- UIAccess **var**: tek `SetWindowPos(HWND_TOPMOST)`, z-order kavgası yok. Popup'lar normal topmost bandında kalır, biz üstteki UIAccess bandındayız, yakalama bizi görmez → canlı ve doğru büyütülmüş popup.
- UIAccess **yok**: mevcut mekanizma, Faz 0'ın seçtiği yol.

**Admin gerekliliği düşüyor.** Yükseltilmiş uygulamalara erişim UIAccess bayrağından gelir, admin'den değil. `RequireAdministrator` kaldırılabilir:

- Her açılışta UAC istemi biter.
- STATUS.md'nin not ettiği "Windows yükseltme isteyen `Run` girdisini logonda başlatmayı reddedebilir" sorunu kendiliğinden çözülür; Windows ile başlatma HKCU `Run` ile gerçekten çalışır.
- Ortam değişkenlerinin (`BM_*`) kalıcı yazılması zorunluluğu da kalkar.

Belgelenecek uyarı: UIAccess sürecine debugger bağlamak için Visual Studio'nun yükseltilmiş çalışması gerekir.

## 7. Faz 2 — Edge-push pan (`ViewportController`)

Model: **itme kadarı.** Görünüm yalnızca farenin bandın ötesine gitmeye çalıştığı miktar kadar kayar; fare durunca görünüm durur. Kazara sürüklenme yok.

Monitör başına, eksen başına bağımsız. Monitör `M`, zoom `z`, bant `b`:

```
vw     = M.w / z                            // kaynak goruntu alani genisligi
srcMax = M.w - vw                           // srcOrigin araligi: [0, srcMax]
S      = (V.x - M.x - srcOrigin.x) * z      // imlecin ekrandaki yeri, 0..M.w
```

Sağ kenarda itme:

```
if S > M.w - b:
    over    = S - (M.w - b)                 // bandin ic kenarini asan ekran pikseli
    want    = over / z                      // kaynak pikseline cevir
    applied = min(want, srcMax - srcOrigin.x)
    srcOrigin.x += applied
    // artan (want - applied) V'de kalir: imlec fiziksel kenara dogru ilerler
```

Sol kenar simetrik (`S < b`, `applied = min(want, srcOrigin.x)`). Y ekseni aynı.

**İstenen davranış buradan çıkıyor:** `srcOrigin` tavana dayanana kadar imleç bantta kalır ve görünüm kayar — yani monitörün sağ kenarına *fiziksel olarak gitmeden* içeriğin sağ kenarına ulaşılır. Kaynak bitince itilecek bir şey kalmaz, imleç fiziksel kenara yürür ve yan monitöre doğal olarak geçer. **`ClipCursor` veya imleç hapsetme gerekmiyor** — geçiş matematikten düşer.

Bant genişliği varsayılanı eksen uzunluğunun %12'si, `[80, 300]` ekran pikseline kırpılır, ayarlanabilir.

İki ince nokta:

- **Zoom değişimi:** `V` sabit tutulur, `srcOrigin_new = V − M − S/z₂` seçilip kırpılır → imleç hem aynı içeriğin hem de aynı ekran noktasının üstünde kalır.
- **Monitör geçişi:** hedef monitörün `srcOrigin`'i **korunur**, `V` ona göre yeniden konumlandırılır. Giriş kenarı, geçilen fiziksel kenardır: soldan girişte `S = 0`, yani `V = M.x + srcOrigin.x`; sağdan girişte `S = M.w`. Görünen sprite ekran kenarından girer, görünüm ise kullanıcının bıraktığı yerdedir. Gerçek imleci ışınlıyoruz ama zaten görünmüyor — kendi imlecimizin olması bunu bedavaya veriyor.

  Hedef monitörde `zoom == 1` ise ölçekleme kapalıdır ve `V` doğrudan fiziksel konumu izler; geçiş sıradan bir monitör geçişine indirgenir.

## 8. Faz 3 — GUI

Panel zaten kurulu (`src/ControlPanel.cpp`). Üç kalem:

1. **Elle doğrulama** — STATUS.md'nin beklediği beş dakikalık kontrol: slider `PushSettings()`'e ulaşıyor mu, değişiklik restart'tan sağ çıkıyor mu, panel açıkken çıkış temiz mi. Geçince `BM_PANEL` kapısı kaldırılır, tepsi girdisi kalıcı olur.
2. **Yeni ayarlar:**
   - Takip modu: `Edge push` (**yeni varsayılan**) / `Centered on pointer` / `Edge push + keyboard focus`
   - Kenar bant genişliği (slider, eksen yüzdesi)
   - İmleç ölçekleme aç/kapa + hız çarpanı
   - İmleç boyut çarpanı
   - Monitör kartlarına "imleç bu monitörde" göstergesi — hangi kartın slider'ının etki edeceğini söylediği için gerekli
3. **UIAccess durum satırı:** kurulu mu, değilse tek cümle açıklama ve kurulum betiğine yönlendirme. **Uygulama içinden kurulum yok** — `Program Files`'a ve makine kök deposuna yazmak bilinçli ve görünür bir betik olarak kalmalı.

Popup modu ayarı **yalnızca** Faz 0 seçimi hâlâ gerekli kılıyorsa eklenir. UIAccess veya Ö1 sorunu kapatıyorsa ayar hiç doğmaz — kalıcı bir düğmeye dönüşmez.

STATUS.md'nin sıralama kuralı korunuyor: `SettingsStore` yaz → kaydet → *sonra* `WM_APP_SETTINGS_CHANGED` gönder.

## 9. Hata yönetimi

Kural: **başarısız olma, bozularak çalışmaya devam et.**

| Durum | Davranış |
|---|---|
| `MagShowSystemCursor` çalışmıyor | İmleç kompozisyonu kapanır, edge-push native imleç davranışına düşer, panel sebebi gösterir |
| **Hook watchdog** | Input thread'de 1 Hz: `GetCursorPos` değişiyor ama olay gelmiyorsa hook ölmüştür → yeniden kur. Kurulamıyorsa **imleci derhal geri getir** |
| `WM_DISPLAYCHANGE` | Tüm `srcOrigin` yeniden kırpılır; `V_f` hiçbir monitörde değilse `GetCursorPos()`'a resync |
| Capture kaybı | Mevcut kısıtlanmış kurtarma, değişmiyor |
| Panik çıkışı | Önce imleci geri getir, sonra kapan |

Hook watchdog **yeni ve artık kritik**: eskiden ölü hook'un bedeli "kısayollar çalışmıyor"du; artık imleç gizliyken ölü hook **görünmez ve ölçeklenmemiş imleç** demek.

## 10. Test

1. **`ViewportController` assert paketi** — mevcut Debug self-check'e girer. Saf matematik olduğu için Windows'suz çalışır.

   Değişmezler: `srcOrigin` her zaman `[0, srcMax]`; kırpılmadığı sürece itme birebir oransal; zoom değişimi `S`'yi korur; monitör geçişi `srcOrigin`'i korur.

   Vakalar: iki yönde monitör geçişi, kenara dayanmışken zoom değişimi, bant görüntü alanından genişse (dejenere), `zoom == 1` geçirgenliği, **negatif koordinatlı monitörler** (birincilin solunda/üstünde — klasik hata kaynağı).

2. **İmleç çözümleme harness'ı** — `IDC_ARROW`, `IDC_IBEAM` (monokrom), `IDC_WAIT` ve 32-bit renkli bir imleç BGRA'ya çevrilip diske dökülür, gözle kontrol. `BM_DUMP_FRAME` hazır emsal.

3. **Hook gecikmesi** — Debug'da p99 ölçümü loglanır, eşik ~1 ms.

4. **Elle kontrol listesi.** Görsel/etkileşimli davranış burada otomatize edilemiyor; UIAccess ile normal bir kabuk pencerelere hiç ulaşamayacak. Liste hatırlanan değil tekrarlanabilir olsun diye burada:

   - [ ] 2× / 4× / 8×'te tıklama hizası
   - [ ] Bağlam menüsü canlı ve büyütülmüş, vurgu fareyi takip ediyor
   - [ ] İki yönde monitör geçişi, her iki monitörde farklı zoom ile
   - [ ] Görünüm hedef monitörde bırakıldığı yerde
   - [ ] Task Manager'dan öldürüldükten sonra imleç geri geliyor
   - [ ] Kilit → açma sonrası imleç ve hook'lar sağlam
   - [ ] Panel değişikliği restart'tan sağ çıkıyor
   - [ ] Metin alanında I-beam doğru çiziliyor (monokrom yol)

## 11. Silinenler

Gelenler kadar gidenler de:

- Z-order kavgası mekanizması — `AppMessages.h:49`, `InputThread.cpp:475` ve çevresi (UIAccess veya Ö1 sorunu kapatırsa)
- Anchor özdeşliği `srcOrigin = anchor·(1 − 1/zoom)` — `ViewportController` yerine geçer
- `BM_NO_TOPMOST_FIGHT` ortam anahtarı
- Faz 0 Yaklaşım 2'ye gönderirse: tüm DXGI/D3D boru hattı (`D3DRenderer.cpp`, `DXGICapture.cpp`)

Dokunulan bölgelerdeki Türkçe yorumlar iş sırasında halledilir, ayrı tarama yok. `InputThread.cpp` ve `D3DRenderer.cpp` zaten büyük ölçüde yeniden yazılacak — bu, STATUS.md açık karar #3'ün büyük kısmını kendiliğinden kapatır.

## 12. STATUS.md açık kararlarının karşılığı

| # | Karar | Bu spec'te |
|---|---|---|
| 0 | Panel varsayılan açık mı | Faz 3.1 — elle doğrulama sonrası açılır |
| 1 | Hangi popup modu varsayılan | Faz 0 + UIAccess çözerse **soru ortadan kalkar** |
| 2 | `Win+Plus` elevation altında çift tetikliyor mu | UIAccess ile hook artık Magnifier'ın kısayolunu yutabilir |
| 3 | Yorumları çevir mi sil mi | Dokunulan yerde hallet, ayrı tarama yok |
| 4 | Edge-push: hizayı feda et mi, imleç kompozisyonu mu | **İmleç kompozisyonu.** §5, kapı kuralıyla |
| 5 | `WC_MAGNIFIER` spike edilsin mi | Faz 0 Ö2 — ama yalnızca Ö1 başarısızsa |
