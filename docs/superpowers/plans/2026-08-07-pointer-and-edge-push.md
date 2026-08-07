# Kendi İmlecimiz ve Edge-Push Pan — Uygulama Planı

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Monitör bazlı tam ekran büyütmeyi popup'lar doğru görünecek şekilde çalıştırmak, gerçek imleci gizleyip kendi büyütülmüş imlecimizi çizmek, ve kenar-itmeli pan'i tıklama hizasını bozmadan eklemek.

**Architecture:** `srcOrigin` artık imlece çapalı değil; input thread'de yaşayan saf-matematik bir `ViewportController` tarafından yürütülüyor. Gerçek imleç `round(V_f)` konumunda tutuluyor (tıklama hizası buradan geliyor), sistem imleci gizleniyor, ve büyütülmüş sprite `(V_f − srcOrigin)·zoom` konumunda çiziliyor. Fare girdisi `1/zoom` ile ölçekleniyor, böylece el hareketi ekranda 1:1 hissediliyor.

**Tech Stack:** C++20, Win32, D3D11, DXGI Desktop Duplication, `magnification.dll` (yalnızca `MagShowSystemCursor`), WinUI 3 XAML Islands (mevcut panel), MSBuild + `bm.ps1`.

**Spec:** [`../specs/2026-08-07-pointer-and-edge-push-design.md`](../specs/2026-08-07-pointer-and-edge-push-design.md)

## Global Constraints

- **Monitör başına bağımsız zoom hiçbir koşulda feda edilmez.** Projenin var oluş sebebi.
- `TreatWarningAsError` açık. Debug x64 ve Release x64 **ikisi de** temiz derlenmeli.
- Yeni `.cpp`/`.h` dosyaları `BetterMagnifier.vcxproj`'a **ve** `BetterMagnifier.vcxproj.filters`'a eklenmeli.
- Tüm yeni kod ve yorumlar **İngilizce**. Dokunulan mevcut satırlardaki Türkçe yorumlar İngilizce'ye çevrilir veya silinir; ayrı çeviri turu yok.
- Namespace: `BetterMagnifier`. Header guard deseni: `BETTER_MAGNIFIER_<NAME>_H` artı `#pragma once`.
- Low-level hook callback'i **hiçbir zaman** bloklamaz. `LowLevelHooksTimeout` 300 ms; hedef p99 < 1 ms.
- Test framework yok. Testler `SettingsStoreSelfCheck()` desenindeki assert tabanlı self-check'lerdir, `#ifdef _DEBUG` altında.
- Build: `.\bm.ps1` (Debug), `.\bm.ps1 release`, `.\bm.ps1 errors` (son logdaki WARN/ERROR).
- `std::atomic<double>` x64'te lock-free; snapshot'lar bunu kullanabilir.
- Commit'ler sık ve küçük. Her task en az bir commit ile biter.

## Faz 0 → mimari dallanma

Bu plan **Yaklaşım 1**'e (mevcut DXGI/D3D boru hattı korunur) göre yazılmıştır; spec'in karar tablosundaki üç sonuçtan ikisi buraya çıkıyor.

Task 1 `WC_MAGNIFIER`'a gönderirse (Ö1 donuk **ve** Ö2'nin dördü de temiz) **yalnızca Task 8 ve içerik render'ı** değişir: sprite ayrı bir layered pencereye çizilir. Task 2, 3, 4, 5, 6, 7, 9, 10, 11, 12 aynen geçerli kalır. O durumda Task 8 yeniden yazılır, plan yeniden yazılmaz.

---

### Task 1: Faz 0 — popup yakalamada canlı mı? (Ö1)

Kod değişikliği neredeyse yok; bu bir **ölçüm** task'ı. Sonucu tüm mimariyi belirliyor.

**Files:**
- Modify: `src/App.cpp` (kare dökümü tetikleyicisi, geçici)
- Modify: `docs/STATUS.md` (sonucu kaydet)

**Interfaces:**
- Consumes: mevcut `BM_DUMP_FRAME` / `BM_DUMP_AFTER` ortam anahtarları
- Produces: `docs/STATUS.md`'de yazılı mimari karar. Sonraki task'lar Yaklaşım 1 varsayar.

- [ ] **Step 1: Mevcut döküm yolunu çok kareli hale getir**

`BM_DUMP_AFTER` tek kare döküyor. Menü vurgusunun *takip edip etmediğini* görmek için ardışık kareler gerekiyor. `src/App.cpp`'de döküm tetiğini bulup (`BM_DUMP_FRAME` okunan yer) sayaç mantığını şu hale getir:

```cpp
// Debug aid for the popup-liveness measurement (docs/superpowers/specs/
// 2026-08-07-pointer-and-edge-push-design.md, phase 0). BM_DUMP_COUNT frames
// are dumped starting at BM_DUMP_AFTER, one every BM_DUMP_EVERY frames.
// Names are <path>.NNN.bmp so they sort in capture order.
static int  s_dumpCount = 0;
static int  s_dumpEvery = 0;
static int  s_dumpsDone = 0;
```

`BM_DUMP_COUNT` (varsayılan 1) ve `BM_DUMP_EVERY` (varsayılan 30) ortam değişkenlerini `BM_DUMP_AFTER` ile aynı yerde oku. Döküm dosya adına `.{:03d}` sırası ekle.

- [ ] **Step 2: Derle**

```bash
.\bm.ps1
```

Beklenen: temiz Debug build.

- [ ] **Step 3: Ölçümü çalıştır**

```powershell
[Environment]::SetEnvironmentVariable('BM_DUMP_FRAME', "$env:TEMP\bm-popup", 'User')
[Environment]::SetEnvironmentVariable('BM_DUMP_AFTER', '120', 'User')
[Environment]::SetEnvironmentVariable('BM_DUMP_COUNT', '12', 'User')
[Environment]::SetEnvironmentVariable('BM_DUMP_EVERY', '20', 'User')
```

Uygulamayı başlat, bir monitörde zoom'u aç, masaüstünde sağ tık ile bağlam menüsü aç ve **dökümler bitene kadar fareyi menü maddeleri arasında yavaşça gezdir**. Sonra:

```powershell
'BM_DUMP_FRAME','BM_DUMP_AFTER','BM_DUMP_COUNT','BM_DUMP_EVERY' |
  ForEach-Object { [Environment]::SetEnvironmentVariable($_, $null, 'User') }
```

- [ ] **Step 4: Kareleri değerlendir ve kararı yaz**

`$env:TEMP\bm-popup.000.bmp` … `.011.bmp` dosyalarını sırayla aç.

| Gözlem | Sonuç |
|---|---|
| Menü karelerde var **ve** vurgulu madde kareden kareye değişiyor | **CANLI** → Yaklaşım 1. Ö2 çalıştırılmaz. |
| Menü var ama vurgu hep aynı maddede | **DONUK** → Ö2 gerekli (aşağıdaki not) |
| Menü karelerde hiç yok | **DONUK** → Ö2 gerekli |

Bulguyu `docs/STATUS.md`'deki "Known broken or unresolved" başlığı altına, kaç kare bakıldığı ve hangi Windows sürümünde ölçüldüğü ile birlikte yaz. Açık karar #1 ve #5'i güncelle.

**DONUK çıkarsa:** burada dur ve spec §3'teki Ö2 spike'ını `spike/wc-magnifier/` altında ayrı bir çalışma olarak yürüt, sonra bu planın Task 8'ini o bulgulara göre yeniden yaz. Task 2–7 ve 9–12 etkilenmez, sırayla devam edilebilir.

- [ ] **Step 5: Commit**

```bash
git add src/App.cpp docs/STATUS.md
git commit -m "measure: settle whether an occluded popup stays live in the capture"
```

---

### Task 2: `SystemCursor` — gizleme, geri getirme ve kapı kuralı

**Files:**
- Create: `src/SystemCursor.h`, `src/SystemCursor.cpp`
- Modify: `src/main.cpp` (guard kurulumu + probe logu)
- Modify: `BetterMagnifier.vcxproj`, `BetterMagnifier.vcxproj.filters`

**Interfaces:**
- Consumes: yok
- Produces: `SystemCursor::Probe() -> bool`, `MagPathAvailable() -> bool`, `Hide()`, `Restore()`, `IsHidden() -> bool`, `InstallGuards()`

- [ ] **Step 1: Header'ı yaz**

`src/SystemCursor.h`:

```cpp
#pragma once

// Hides and restores the system mouse pointer.
//
// The primary path is magnification.dll's MagShowSystemCursor. It is preferred
// for one reason that matters more than convenience: its effect dies with the
// process. SetSystemCursor does not — if BetterMagnifier is killed from Task
// Manager while the pointer is hidden, the user is left without a pointer
// until they log off. On an accessibility tool that is the worst possible
// failure, so the SetSystemCursor fallback is gated (see MagPathAvailable).
//
// All members are static: there is exactly one system pointer.

#ifndef BETTER_MAGNIFIER_SYSTEM_CURSOR_H
#define BETTER_MAGNIFIER_SYSTEM_CURSOR_H

#include <windows.h>

namespace BetterMagnifier {

class SystemCursor
{
public:
    // Loads magnification.dll and calls MagInitialize. Call once at startup.
    // Returns true when MagShowSystemCursor is usable.
    static bool Probe();

    // Probe()'s result. Callers gate pointer compositing on this: when it is
    // false the feature stays off unless the user explicitly opts in.
    static bool MagPathAvailable();

    // Idempotent. Only call while at least one monitor is magnified.
    static void Hide();

    // Idempotent and safe from any exit path, including a terminate handler.
    static void Restore();

    static bool IsHidden();

    // Registers the console-control and terminate handlers that call Restore.
    // Call once, after Probe.
    static void InstallGuards();
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_SYSTEM_CURSOR_H
```

- [ ] **Step 2: Implementasyonu yaz**

`src/SystemCursor.cpp`:

```cpp
#include "pch.h"
#include "SystemCursor.h"
#include "Logger.h"

#include <atomic>
#include <exception>

namespace BetterMagnifier {
namespace {

using MagInitializeFn        = BOOL (WINAPI*)();
using MagUninitializeFn      = BOOL (WINAPI*)();
using MagShowSystemCursorFn  = BOOL (WINAPI*)(BOOL);

HMODULE                 g_magDll        = nullptr;
MagShowSystemCursorFn   g_magShowCursor = nullptr;
MagUninitializeFn       g_magUninit     = nullptr;

std::atomic<bool> g_magAvailable{false};
std::atomic<bool> g_hidden{false};

std::terminate_handler g_previousTerminate = nullptr;

// Every standard cursor SetSystemCursor can replace. OCR_ICON and OCR_SIZE are
// deliberately absent: they are obsolete and SetSystemCursor rejects them.
constexpr WORD kCursorIds[] = {
    OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP,
    OCR_SIZENWSE, OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL,
    OCR_NO, OCR_HAND, OCR_APPSTARTING,
};

// A 1x1 fully transparent cursor. Recreated per call because SetSystemCursor
// takes ownership of the handle it is given.
HCURSOR MakeBlankCursor()
{
    BYTE andMask[] = { 0xFF };   // 1 = keep screen
    BYTE xorMask[] = { 0x00 };   // 0 = do not invert  => fully transparent
    return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 1, 1, andMask, xorMask);
}

void RestoreFromRegistry()
{
    // The single reliable undo for SetSystemCursor: reload every system cursor
    // from the user's registry settings.
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
}

BOOL WINAPI ConsoleCtrlHandler(DWORD)
{
    SystemCursor::Restore();
    return FALSE;   // let the default handler continue
}

void TerminateHandler()
{
    SystemCursor::Restore();
    if (g_previousTerminate)
        g_previousTerminate();
    std::abort();
}

} // namespace

bool SystemCursor::Probe()
{
    if (g_magDll)
        return g_magAvailable.load();

    g_magDll = LoadLibraryW(L"magnification.dll");
    if (!g_magDll)
    {
        LOG_WARN("magnification.dll not present — pointer compositing will be gated");
        return false;
    }

    auto init      = reinterpret_cast<MagInitializeFn>(
                        GetProcAddress(g_magDll, "MagInitialize"));
    g_magUninit    = reinterpret_cast<MagUninitializeFn>(
                        GetProcAddress(g_magDll, "MagUninitialize"));
    g_magShowCursor = reinterpret_cast<MagShowSystemCursorFn>(
                        GetProcAddress(g_magDll, "MagShowSystemCursor"));

    if (!init || !g_magShowCursor || !init())
    {
        LOG_WARN("MagInitialize/MagShowSystemCursor unavailable — pointer "
                 "compositing will be gated");
        return false;
    }

    // Prove the call actually works rather than trusting that it exists: hide
    // and immediately show. A round trip that returns FALSE means this build of
    // Windows refuses it for us, and we must not rely on it later.
    const BOOL hid = g_magShowCursor(FALSE);
    g_magShowCursor(TRUE);

    g_magAvailable.store(hid != FALSE);
    LOG_INFO("MagShowSystemCursor probe: {}", hid ? "available" : "refused");
    return g_magAvailable.load();
}

bool SystemCursor::MagPathAvailable() { return g_magAvailable.load(); }
bool SystemCursor::IsHidden()         { return g_hidden.load(); }

void SystemCursor::Hide()
{
    if (g_hidden.exchange(true))
        return;

    if (g_magAvailable.load())
    {
        g_magShowCursor(FALSE);
        return;
    }

    for (WORD id : kCursorIds)
    {
        HCURSOR blank = MakeBlankCursor();
        if (blank)
            SetSystemCursor(blank, id);   // takes ownership
    }
}

void SystemCursor::Restore()
{
    if (!g_hidden.exchange(false))
        return;

    if (g_magAvailable.load())
    {
        g_magShowCursor(TRUE);
        return;
    }

    RestoreFromRegistry();
}

void SystemCursor::InstallGuards()
{
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    g_previousTerminate = std::set_terminate(TerminateHandler);
}

} // namespace BetterMagnifier
```

- [ ] **Step 3: Projeye ekle ve main'den çağır**

`BetterMagnifier.vcxproj`'a `<ClCompile Include="src\SystemCursor.cpp" />` ve `<ClInclude Include="src\SystemCursor.h" />`, `.filters`'a karşılıkları. Linker'a `Magnification.lib` **eklenmiyor** — `LoadLibraryW` ile geç bağlanıyoruz, böylece DLL yoksa uygulama yine açılıyor.

`src/main.cpp`'de self-check bloğundan hemen sonra:

```cpp
    BetterMagnifier::SystemCursor::Probe();
    BetterMagnifier::SystemCursor::InstallGuards();
```

`src/App.cpp`'deki mesaj penceresi `WndProc`'unda **üç** yerden `SystemCursor::Restore()` çağır:

```cpp
    case WM_QUERYENDSESSION:
    case WM_ENDSESSION:
        SystemCursor::Restore();
        break;

    case WM_WTSSESSION_CHANGE:
        // The secure desktop has its own pointer, but our hidden state must not
        // survive across the switch: hooks are detached there and the restore
        // path would have no way to run if anything went wrong while locked.
        if (wParam == WTS_SESSION_LOCK)
            SystemCursor::Restore();
        break;
```

`WTS_SESSION_UNLOCK` zaten hook'ları yeniden kuruyor; oraya bir şey eklemeye gerek yok, çünkü zoom hâlâ açıksa bir sonraki kare `Hide()`'ı tekrar çağırıyor. Panik çıkışı handler'ına da `Restore()` ekle.

- [ ] **Step 4: Derle ve probe sonucunu oku**

```bash
.\bm.ps1 run
```

Sonra:

```bash
.\bm.ps1 log
```

Beklenen: logda `MagShowSystemCursor probe: available` **veya** `: refused`. **Bu satır Task 6 ve 8'in varsayılan davranışını belirliyor** — sonucu `docs/STATUS.md`'ye yaz.

- [ ] **Step 5: Commit**

```bash
git add src/SystemCursor.h src/SystemCursor.cpp src/main.cpp src/App.cpp BetterMagnifier.vcxproj BetterMagnifier.vcxproj.filters docs/STATUS.md
git commit -m "feat(cursor): hide and restore the system pointer, with exit guards"
```

---

### Task 3: `ViewportController` — çekirdek dönüşüm, kırpma ve `--self-check`

Bu task testleri koşulabilir hale getiriyor. Sonraki her task ona dayanıyor.

**Files:**
- Create: `src/ViewportController.h`, `src/ViewportController.cpp`
- Modify: `src/main.cpp` (`--self-check` anahtarı)
- Modify: `BetterMagnifier.vcxproj`, `BetterMagnifier.vcxproj.filters`

**Interfaces:**
- Consumes: yok — saf matematik, Windows bağımlılığı yok
- Produces: aşağıdaki tam sınıf arayüzü; `ViewportControllerSelfCheck()`

- [ ] **Step 1: Header'ı yaz**

`src/ViewportController.h`:

```cpp
#pragma once

// Per-monitor source-rectangle state and the edge-push pan model.
//
// Pure math: no Windows calls, no logging, no allocation. That is deliberate —
// it is the only part of the magnification transform that can be asserted from
// a script, and every geometry bug this project has had lived here.
//
// Coordinates: pointer positions are virtual-desktop pixels (they can be
// negative when a monitor sits left of or above the primary). srcOrigin is
// monitor-local. Both are double so that sub-pixel hand motion survives
// scaling by 1/zoom.
//
// Thread ownership: the input thread. It advances on mouse events rather than
// on frames, because "push by as much as the mouse pushed" has to be
// proportional to mouse motion, not to frame rate.

#ifndef BETTER_MAGNIFIER_VIEWPORT_CONTROLLER_H
#define BETTER_MAGNIFIER_VIEWPORT_CONTROLLER_H

#include <array>
#include <cstddef>

namespace BetterMagnifier {

enum class Edge { Left, Right, Top, Bottom };

struct EdgePushConfig
{
    bool  enabled      = true;
    float bandFraction = 0.12f;    // of the axis length
    float bandMinPx    = 80.0f;
    float bandMaxPx    = 300.0f;
};

struct MonitorViewport
{
    long   originX = 0;      // monitor rect in virtual-desktop coordinates
    long   originY = 0;
    long   width   = 0;
    long   height  = 0;

    double srcOriginX = 0.0; // monitor-local, always within [0, MaxSrcOrigin]
    double srcOriginY = 0.0;
    double zoom       = 1.0;
};

class ViewportController
{
public:
    static constexpr size_t kMaxMonitors = 8;

    void   SetMonitorCount(size_t count);
    size_t MonitorCount() const { return m_count; }

    void SetMonitorRect(size_t index, long originX, long originY,
                        long width, long height);

    void SetConfig(const EdgePushConfig& cfg) { m_cfg = cfg; }
    const EdgePushConfig& Config() const { return m_cfg; }

    // Keeps both the content under the pointer and the pointer's screen
    // position fixed across the change.
    void   SetZoom(size_t index, double zoom, double pointerX, double pointerY);
    double Zoom(size_t index) const;

    // The pointer has already been advanced by the caller. Pushes srcOrigin
    // when the pointer's screen position falls inside an edge band. Does not
    // modify the pointer: when the source runs out, the leftover motion is
    // exactly what carries the pointer to the physical edge and onto the next
    // monitor.
    void OnPointerMoved(size_t index, double pointerX, double pointerY);

    // Positions the pointer for a monitor it just entered, preserving that
    // monitor's srcOrigin so the view stays where the user left it.
    void PlaceOnEntry(size_t index, Edge entry, double& pointerX, double& pointerY);

    // Pulls a pointer that fell outside every monitor back into the nearest
    // one, so it cannot drift into empty space at the desktop's outer edge.
    void ClampPointerToDesktop(double& x, double& y) const;

    // -1 when no monitor contains the point.
    int MonitorIndexAt(double x, double y) const;

    // Call after a resolution or topology change.
    void ReclampAll();

    const MonitorViewport& Viewport(size_t index) const;

    double MaxSrcOriginX(size_t index) const;
    double MaxSrcOriginY(size_t index) const;

    // Band width in screen pixels for an axis of the given length.
    double BandPx(long axisLength) const;

    // Pointer's screen position on its monitor, 0..width / 0..height.
    double ScreenX(size_t index, double pointerX) const;
    double ScreenY(size_t index, double pointerY) const;

private:
    MonitorViewport&       At(size_t index);
    const MonitorViewport& At(size_t index) const;

    std::array<MonitorViewport, kMaxMonitors> m_v{};
    size_t         m_count = 0;
    EdgePushConfig m_cfg{};
};

#ifdef _DEBUG
// Assert-based self-check, run from main. Mirrors SettingsStoreSelfCheck.
void ViewportControllerSelfCheck();
#endif

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_VIEWPORT_CONTROLLER_H
```

- [ ] **Step 2: Self-check'i yaz (henüz implementasyon yok — testler önce)**

`src/ViewportController.cpp`'nin sonuna. Bu turda **yalnızca çekirdek dönüşüm** doğrulanıyor; edge-push assert'leri Task 4'te ekleniyor.

```cpp
#ifdef _DEBUG
void ViewportControllerSelfCheck()
{
    // 1x zoom: the source covers the whole monitor and cannot move.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 1.0, 960.0, 540.0);
        assert(vc.MaxSrcOriginX(0) == 0.0);
        assert(vc.Viewport(0).srcOriginX == 0.0);
        assert(vc.ScreenX(0, 960.0) == 960.0);
    }

    // 2x zoom: the source is half the monitor, so srcOrigin can travel half.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 960.0, 540.0);
        assert(std::abs(vc.MaxSrcOriginX(0) - 960.0) < 1e-9);
        assert(std::abs(vc.MaxSrcOriginY(0) - 540.0) < 1e-9);
    }

    // Zoom change keeps the pointer over the same content AND at the same
    // screen position. This is the invariant that replaces the old anchor
    // identity, so it gets an explicit case.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 960.0, 540.0);
        const double screenBefore = vc.ScreenX(0, 960.0);
        vc.SetZoom(0, 4.0, 960.0, 540.0);
        const double screenAfter = vc.ScreenX(0, 960.0);
        assert(std::abs(screenBefore - screenAfter) < 1e-6);
    }

    // srcOrigin never leaves [0, max], even when the pointer sits outside.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 4.0, -5000.0, -5000.0);
        assert(vc.Viewport(0).srcOriginX >= 0.0);
        assert(vc.Viewport(0).srcOriginX <= vc.MaxSrcOriginX(0));
        vc.SetZoom(0, 4.0, 99999.0, 99999.0);
        assert(vc.Viewport(0).srcOriginX <= vc.MaxSrcOriginX(0));
    }

    // Monitors left of and above the primary have negative origins. This has
    // been a bug source in every codebase that assumes (0,0) is the corner.
    {
        ViewportController vc;
        vc.SetMonitorCount(2);
        vc.SetMonitorRect(0, -1920, -200, 1920, 1080);
        vc.SetMonitorRect(1, 0, 0, 2560, 1440);
        assert(vc.MonitorIndexAt(-1000.0, 100.0) == 0);
        assert(vc.MonitorIndexAt(1000.0, 100.0) == 1);
        assert(vc.MonitorIndexAt(9999.0, 9999.0) == -1);

        vc.SetZoom(0, 2.0, -960.0, 340.0);
        assert(vc.Viewport(0).srcOriginX >= 0.0);
        assert(vc.ScreenX(0, -960.0) >= 0.0);
        assert(vc.ScreenX(0, -960.0) <= 1920.0);
    }

    // A pointer outside every monitor is pulled back into the nearest one.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        double x = 5000.0, y = -300.0;
        vc.ClampPointerToDesktop(x, y);
        assert(x >= 0.0 && x <= 1920.0);
        assert(y >= 0.0 && y <= 1080.0);
    }

    // Band width is clamped at both ends.
    {
        ViewportController vc;
        EdgePushConfig cfg;
        cfg.bandFraction = 0.12f;
        cfg.bandMinPx    = 80.0f;
        cfg.bandMaxPx    = 300.0f;
        vc.SetConfig(cfg);
        assert(vc.BandPx(400)  == 80.0);    // 48 -> floored to the minimum
        assert(vc.BandPx(1920) == 230.4);   // inside the range
        assert(vc.BandPx(7680) == 300.0);   // 921 -> capped at the maximum
    }

    LOG_INFO("ViewportControllerSelfCheck passed");
}
#endif
```

- [ ] **Step 3: `--self-check` anahtarını ekle**

Bir self-check'i script'ten koşabilmek şart; aksi halde "testin başarısız olduğunu doğrula" adımı tiyatro olur. `src/main.cpp`'de, mevcut Debug self-check bloğunu şununla değiştir:

```cpp
#ifdef _DEBUG
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

    BetterMagnifier::SettingsStoreSelfCheck();
    BetterMagnifier::ViewportControllerSelfCheck();

    // --self-check runs the pure-logic assertions and exits, so the suite is
    // scriptable. Without it the process would go on to open windows and never
    // return, and there would be no way to gate a commit on the asserts.
    if (std::wcsstr(GetCommandLineW(), L"--self-check") != nullptr)
    {
        LOG_INFO("Self-check complete, exiting (--self-check)");
        return 0;
    }
#endif
```

`<cwchar>` include'unu ekle.

- [ ] **Step 4: Testin başarısız olduğunu doğrula**

Önce `ViewportController.cpp`'ye yalnızca **kasten yanlış** iskelet koy, böylece assert'lerin gerçekten koştuğu görülür:

```cpp
double ViewportController::MaxSrcOriginX(size_t) const { return 0.0; }   // WRONG on purpose
```

diğer üyeler derlenecek kadar minimal (`{}` gövdeler, sabit dönüşler).

```bash
.\bm.ps1
.\bin\Debug-x64\BetterMagnifier.exe --self-check
```

Beklenen: **sıfırdan farklı çıkış kodu**, stderr'de assert satırı (`MaxSrcOriginX(0) - 960.0`). Çıkış kodu 0 gelirse assert'ler koşmuyor demektir — devam etme, sebebini bul.

- [ ] **Step 5: Gerçek implementasyonu yaz**

```cpp
#include "pch.h"
#include "ViewportController.h"
#include "Logger.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace BetterMagnifier {

namespace {
// Below this the source equals the monitor and nothing can pan.
constexpr double kNoZoom = 1.0;
} // namespace

MonitorViewport& ViewportController::At(size_t index)
{
    return m_v[index < kMaxMonitors ? index : kMaxMonitors - 1];
}

const MonitorViewport& ViewportController::At(size_t index) const
{
    return m_v[index < kMaxMonitors ? index : kMaxMonitors - 1];
}

const MonitorViewport& ViewportController::Viewport(size_t index) const
{
    return At(index);
}

void ViewportController::SetMonitorCount(size_t count)
{
    m_count = (std::min)(count, kMaxMonitors);
}

void ViewportController::SetMonitorRect(size_t index, long originX, long originY,
                                        long width, long height)
{
    MonitorViewport& v = At(index);
    v.originX = originX;
    v.originY = originY;
    v.width   = (std::max)(1L, width);
    v.height  = (std::max)(1L, height);
    v.srcOriginX = std::clamp(v.srcOriginX, 0.0, MaxSrcOriginX(index));
    v.srcOriginY = std::clamp(v.srcOriginY, 0.0, MaxSrcOriginY(index));
}

double ViewportController::Zoom(size_t index) const { return At(index).zoom; }

double ViewportController::MaxSrcOriginX(size_t index) const
{
    const MonitorViewport& v = At(index);
    if (v.zoom <= kNoZoom) return 0.0;
    return static_cast<double>(v.width) - static_cast<double>(v.width) / v.zoom;
}

double ViewportController::MaxSrcOriginY(size_t index) const
{
    const MonitorViewport& v = At(index);
    if (v.zoom <= kNoZoom) return 0.0;
    return static_cast<double>(v.height) - static_cast<double>(v.height) / v.zoom;
}

double ViewportController::BandPx(long axisLength) const
{
    const double raw = static_cast<double>(axisLength) * m_cfg.bandFraction;
    return std::clamp(raw, static_cast<double>(m_cfg.bandMinPx),
                           static_cast<double>(m_cfg.bandMaxPx));
}

double ViewportController::ScreenX(size_t index, double pointerX) const
{
    const MonitorViewport& v = At(index);
    return (pointerX - static_cast<double>(v.originX) - v.srcOriginX) * v.zoom;
}

double ViewportController::ScreenY(size_t index, double pointerY) const
{
    const MonitorViewport& v = At(index);
    return (pointerY - static_cast<double>(v.originY) - v.srcOriginY) * v.zoom;
}

void ViewportController::SetZoom(size_t index, double zoom,
                                 double pointerX, double pointerY)
{
    MonitorViewport& v = At(index);

    // Screen position of the pointer BEFORE the change; we keep it.
    const double screenX = ScreenX(index, pointerX);
    const double screenY = ScreenY(index, pointerY);

    v.zoom = (std::max)(kNoZoom, zoom);

    if (v.zoom <= kNoZoom)
    {
        v.srcOriginX = 0.0;
        v.srcOriginY = 0.0;
        return;
    }

    // srcOrigin such that the same content stays under the same screen point.
    v.srcOriginX = pointerX - static_cast<double>(v.originX) - screenX / v.zoom;
    v.srcOriginY = pointerY - static_cast<double>(v.originY) - screenY / v.zoom;

    v.srcOriginX = std::clamp(v.srcOriginX, 0.0, MaxSrcOriginX(index));
    v.srcOriginY = std::clamp(v.srcOriginY, 0.0, MaxSrcOriginY(index));
}

int ViewportController::MonitorIndexAt(double x, double y) const
{
    for (size_t i = 0; i < m_count; ++i)
    {
        const MonitorViewport& v = m_v[i];
        if (x >= v.originX && x < v.originX + v.width &&
            y >= v.originY && y < v.originY + v.height)
            return static_cast<int>(i);
    }
    return -1;
}

void ViewportController::ClampPointerToDesktop(double& x, double& y) const
{
    if (m_count == 0 || MonitorIndexAt(x, y) >= 0)
        return;

    size_t best = 0;
    double bestDist = -1.0;
    for (size_t i = 0; i < m_count; ++i)
    {
        const MonitorViewport& v = m_v[i];
        const double cx = std::clamp(x, static_cast<double>(v.originX),
                                        static_cast<double>(v.originX + v.width)  - 1.0);
        const double cy = std::clamp(y, static_cast<double>(v.originY),
                                        static_cast<double>(v.originY + v.height) - 1.0);
        const double d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        if (bestDist < 0.0 || d < bestDist) { bestDist = d; best = i; }
    }

    const MonitorViewport& v = m_v[best];
    x = std::clamp(x, static_cast<double>(v.originX),
                      static_cast<double>(v.originX + v.width)  - 1.0);
    y = std::clamp(y, static_cast<double>(v.originY),
                      static_cast<double>(v.originY + v.height) - 1.0);
}

void ViewportController::ReclampAll()
{
    for (size_t i = 0; i < m_count; ++i)
    {
        m_v[i].srcOriginX = std::clamp(m_v[i].srcOriginX, 0.0, MaxSrcOriginX(i));
        m_v[i].srcOriginY = std::clamp(m_v[i].srcOriginY, 0.0, MaxSrcOriginY(i));
    }
}

// OnPointerMoved and PlaceOnEntry land in Task 4.
void ViewportController::OnPointerMoved(size_t, double, double) {}
void ViewportController::PlaceOnEntry(size_t, Edge, double&, double&) {}

} // namespace BetterMagnifier
```

- [ ] **Step 6: Testin geçtiğini doğrula**

```bash
.\bm.ps1
.\bin\Debug-x64\BetterMagnifier.exe --self-check
```

Beklenen: çıkış kodu **0**, logda `ViewportControllerSelfCheck passed`. Ayrıca Release'in de temiz derlendiğini doğrula:

```bash
.\bm.ps1 release
```

- [ ] **Step 7: Commit**

```bash
git add src/ViewportController.h src/ViewportController.cpp src/main.cpp BetterMagnifier.vcxproj BetterMagnifier.vcxproj.filters
git commit -m "feat(viewport): add the pure-math viewport controller and a scriptable self-check"
```

---

### Task 4: Edge-push matematiği

**Files:**
- Modify: `src/ViewportController.cpp` (`OnPointerMoved`, `PlaceOnEntry` ve self-check)

**Interfaces:**
- Consumes: Task 3'ün tüm arayüzü
- Produces: çalışan `OnPointerMoved(size_t, double, double)` ve `PlaceOnEntry(size_t, Edge, double&, double&)`

- [ ] **Step 1: Başarısız assert'leri yaz**

`ViewportControllerSelfCheck()`'in sonuna, `LOG_INFO` satırından önce:

```cpp
    // Inside the band the view does not move at all.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 960.0, 540.0);
        const double before = vc.Viewport(0).srcOriginX;
        vc.OnPointerMoved(0, 960.0, 540.0);          // dead centre
        assert(vc.Viewport(0).srcOriginX == before);
    }

    // Past the band the view moves by exactly the overshoot, in source pixels.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 480.0, 270.0);            // srcOrigin 0, screen 960
        assert(vc.Viewport(0).srcOriginX == 0.0);

        const double band = vc.BandPx(1920);         // 230.4
        // Put the pointer 100 screen px past the band's inner edge.
        // screen = (p - srcOrigin) * zoom  =>  p = screen / zoom
        const double targetScreen = 1920.0 - band + 100.0;
        const double p = targetScreen / 2.0;
        vc.OnPointerMoved(0, p, 270.0);
        assert(std::abs(vc.Viewport(0).srcOriginX - 50.0) < 1e-6);   // 100 / zoom

        // And the pointer now sits exactly at the band's inner edge.
        assert(std::abs(vc.ScreenX(0, p) - (1920.0 - band)) < 1e-6);
    }

    // The push saturates at the source edge and never overruns it.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 960.0, 540.0);
        for (int i = 0; i < 200; ++i)
            vc.OnPointerMoved(0, 1900.0, 540.0);     // keep shoving right
        assert(std::abs(vc.Viewport(0).srcOriginX - vc.MaxSrcOriginX(0)) < 1e-6);

        // Saturated: the pointer is now free to run past the band toward the
        // physical edge, which is what lets it cross to the next monitor.
        assert(vc.ScreenX(0, 1900.0) > 1920.0 - vc.BandPx(1920));
    }

    // The left edge is the mirror image.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 1440.0, 540.0);           // srcOrigin at max
        const double atMax = vc.Viewport(0).srcOriginX;
        assert(atMax > 0.0);
        for (int i = 0; i < 200; ++i)
            vc.OnPointerMoved(0, 970.0, 540.0);
        assert(vc.Viewport(0).srcOriginX < atMax);
        assert(vc.Viewport(0).srcOriginX >= 0.0);
    }

    // Y pushes independently of X.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 480.0, 270.0);
        vc.OnPointerMoved(0, 480.0, 1000.0);
        assert(vc.Viewport(0).srcOriginX == 0.0);
        assert(vc.Viewport(0).srcOriginY > 0.0);
    }

    // Disabled config means the view never moves.
    {
        ViewportController vc;
        EdgePushConfig cfg; cfg.enabled = false;
        vc.SetConfig(cfg);
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 480.0, 270.0);
        vc.OnPointerMoved(0, 950.0, 270.0);
        assert(vc.Viewport(0).srcOriginX == 0.0);
    }

    // Degenerate: a band wider than half the viewport must not fight itself
    // (both edges pushing at once would oscillate).
    {
        ViewportController vc;
        EdgePushConfig cfg; cfg.bandFraction = 0.9f; cfg.bandMinPx = 1.0f;
        cfg.bandMaxPx = 100000.0f;
        vc.SetConfig(cfg);
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 2.0, 480.0, 270.0);
        const double a = vc.Viewport(0).srcOriginX;
        vc.OnPointerMoved(0, 480.0, 270.0);
        const double b = vc.Viewport(0).srcOriginX;
        vc.OnPointerMoved(0, 480.0, 270.0);
        assert(vc.Viewport(0).srcOriginX == b);   // settles, does not oscillate
        (void)a;
    }

    // zoom == 1 is fully transparent: no push, no state.
    {
        ViewportController vc;
        vc.SetMonitorCount(1);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetZoom(0, 1.0, 960.0, 540.0);
        vc.OnPointerMoved(0, 1919.0, 1079.0);
        assert(vc.Viewport(0).srcOriginX == 0.0);
        assert(vc.Viewport(0).srcOriginY == 0.0);
    }

    // Entering a monitor preserves its srcOrigin and puts the pointer on the
    // crossed edge. The view stays where the user left it.
    {
        ViewportController vc;
        vc.SetMonitorCount(2);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetMonitorRect(1, 1920, 0, 1920, 1080);
        vc.SetZoom(1, 2.0, 1920.0 + 1440.0, 540.0);
        const double kept = vc.Viewport(1).srcOriginX;
        assert(kept > 0.0);

        double px = 1921.0, py = 540.0;
        vc.PlaceOnEntry(1, Edge::Left, px, py);
        assert(vc.Viewport(1).srcOriginX == kept);            // view preserved
        assert(std::abs(vc.ScreenX(1, px) - 0.0) < 1e-6);     // enters at x=0
    }

    // Entry from the right lands on the far edge.
    {
        ViewportController vc;
        vc.SetMonitorCount(2);
        vc.SetMonitorRect(0, 0, 0, 1920, 1080);
        vc.SetMonitorRect(1, 1920, 0, 1920, 1080);
        vc.SetZoom(0, 4.0, 960.0, 540.0);
        double px = 1919.0, py = 540.0;
        vc.PlaceOnEntry(0, Edge::Right, px, py);
        assert(std::abs(vc.ScreenX(0, px) - 1920.0) < 1e-6);
    }
```

- [ ] **Step 2: Başarısızlığı doğrula**

```bash
.\bm.ps1
.\bin\Debug-x64\BetterMagnifier.exe --self-check
```

Beklenen: sıfırdan farklı çıkış kodu; ilk düşen assert `srcOriginX - 50.0` olmalı (boş `OnPointerMoved` hiçbir şey itmiyor).

- [ ] **Step 3: `OnPointerMoved` ve `PlaceOnEntry`'yi yaz**

`ViewportController.cpp`'deki boş gövdeleri şununla değiştir:

```cpp
namespace {

// Shared by both axes. Returns the amount srcOrigin should move.
//
// screen  : pointer position on the axis, 0..length
// length  : monitor extent on the axis, in screen pixels
// band    : band width, in screen pixels
// zoom    : current magnification
// room-   : how far srcOrigin can still travel toward 0
// room+   : how far it can still travel toward the maximum
//
// Only one side can fire per call: when the band is wide enough to overlap
// itself the near edge wins, which keeps a degenerate config from oscillating.
double PushAmount(double screen, double length, double band, double zoom,
                  double roomNegative, double roomPositive)
{
    if (zoom <= 1.0 || band <= 0.0)
        return 0.0;

    if (screen < band)
    {
        const double want = (band - screen) / zoom;
        return -(std::min)(want, roomNegative);
    }

    if (screen > length - band)
    {
        const double want = (screen - (length - band)) / zoom;
        return (std::min)(want, roomPositive);
    }

    return 0.0;
}

} // namespace

void ViewportController::OnPointerMoved(size_t index, double pointerX, double pointerY)
{
    if (!m_cfg.enabled)
        return;

    MonitorViewport& v = At(index);
    if (v.zoom <= kNoZoom)
        return;

    const double bandX = BandPx(v.width);
    const double bandY = BandPx(v.height);

    const double dx = PushAmount(ScreenX(index, pointerX),
                                 static_cast<double>(v.width), bandX, v.zoom,
                                 v.srcOriginX,
                                 MaxSrcOriginX(index) - v.srcOriginX);

    const double dy = PushAmount(ScreenY(index, pointerY),
                                 static_cast<double>(v.height), bandY, v.zoom,
                                 v.srcOriginY,
                                 MaxSrcOriginY(index) - v.srcOriginY);

    v.srcOriginX = std::clamp(v.srcOriginX + dx, 0.0, MaxSrcOriginX(index));
    v.srcOriginY = std::clamp(v.srcOriginY + dy, 0.0, MaxSrcOriginY(index));
}

void ViewportController::PlaceOnEntry(size_t index, Edge entry,
                                      double& pointerX, double& pointerY)
{
    const MonitorViewport& v = At(index);

    // srcOrigin is deliberately left alone: the view stays where the user left
    // it, and the pointer is moved to match. The real pointer teleports, but it
    // is hidden, so what the user sees is a sprite entering from the edge.
    auto sourceFor = [&](double screen, double origin, double srcOrigin) {
        return origin + srcOrigin + screen / v.zoom;
    };

    switch (entry)
    {
    case Edge::Left:
        pointerX = sourceFor(0.0, v.originX, v.srcOriginX);
        break;
    case Edge::Right:
        pointerX = sourceFor(static_cast<double>(v.width), v.originX, v.srcOriginX);
        break;
    case Edge::Top:
        pointerY = sourceFor(0.0, v.originY, v.srcOriginY);
        break;
    case Edge::Bottom:
        pointerY = sourceFor(static_cast<double>(v.height), v.originY, v.srcOriginY);
        break;
    }
}
```

- [ ] **Step 4: Testlerin geçtiğini doğrula**

```bash
.\bm.ps1
.\bin\Debug-x64\BetterMagnifier.exe --self-check
```

Beklenen: çıkış kodu 0. Sonra `.\bm.ps1 release` temiz.

- [ ] **Step 5: Commit**

```bash
git add src/ViewportController.cpp
git commit -m "feat(viewport): push the source rect when the pointer enters an edge band"
```

---

### Task 5: `ViewportSnapshot` ve render thread'in yeni dönüşüme geçmesi

Bu task **eski anchor özdeşliğini siliyor**. Sonrasında büyütme hâlâ çalışıyor olmalı (imleç ölçekleme henüz yok — bu turda `V_f` ham imleç konumu).

**Files:**
- Create: `src/ViewportSnapshot.h`
- Modify: `src/App.h`, `src/App.cpp` (satır ~592-640 dönüşüm bloğu), `src/InputThread.h`, `src/InputThread.cpp`
- Modify: `BetterMagnifier.vcxproj`, `BetterMagnifier.vcxproj.filters`

**Interfaces:**
- Consumes: `ViewportController` (Task 3, 4)
- Produces: `ViewportSnapshot` — `Monitor(size_t) -> MonitorViewportAtomic&`, `pointerX`, `pointerY`, `pointerScaled`, `generation`

- [ ] **Step 1: Snapshot header'ını yaz**

`src/ViewportSnapshot.h`:

```cpp
#pragma once

// Lock-free publication of the viewport state from the input thread to the
// render thread. Same pattern and same reasoning as StatusSnapshot: a lock on
// the render thread's hot path makes frame time unpredictable.
//
// Fields are individually atomic, the struct is not. A reader can see a fresh
// srcOriginX beside a one-event-old zoom; the visible cost is one frame of a
// slightly stale view, which is below the noise floor at 60 Hz.
//
// std::atomic<double> is lock-free on x64.

#ifndef BETTER_MAGNIFIER_VIEWPORT_SNAPSHOT_H
#define BETTER_MAGNIFIER_VIEWPORT_SNAPSHOT_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace BetterMagnifier {

struct MonitorViewportAtomic
{
    std::atomic<double> srcOriginX{0.0};
    std::atomic<double> srcOriginY{0.0};
    std::atomic<double> zoom{1.0};
};

class ViewportSnapshot
{
public:
    static constexpr size_t kMaxMonitors = 8;

    ViewportSnapshot() = default;
    ViewportSnapshot(const ViewportSnapshot&) = delete;
    ViewportSnapshot& operator=(const ViewportSnapshot&) = delete;

    MonitorViewportAtomic& Monitor(size_t i)
    {
        return m_monitors[i < kMaxMonitors ? i : kMaxMonitors - 1];
    }

    const MonitorViewportAtomic& Monitor(size_t i) const
    {
        return m_monitors[i < kMaxMonitors ? i : kMaxMonitors - 1];
    }

    // Virtual-desktop coordinates. Authoritative pointer position: the real OS
    // cursor is kept at round(pointerX, pointerY).
    std::atomic<double> pointerX{0.0};
    std::atomic<double> pointerY{0.0};

    // True while PointerInput is driving the cursor. When false the renderer
    // must not draw a sprite: the real pointer is visible and doing the job.
    std::atomic<bool> pointerScaled{false};

    // Bumped on every publish; lets the renderer skip a redundant frame.
    std::atomic<uint64_t> generation{0};

private:
    std::array<MonitorViewportAtomic, kMaxMonitors> m_monitors{};
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_VIEWPORT_SNAPSHOT_H
```

- [ ] **Step 2: `InputThread`'e controller ve snapshot'ı bağla**

`src/InputThread.h`'ye ekle:

```cpp
#include "ViewportController.h"
#include "ViewportSnapshot.h"
```

ve public'e:

```cpp
    // Both are owned by App and outlive this thread.
    void Attach(ViewportController* controller, ViewportSnapshot* snapshot);

    // Recompute srcOrigin for a monitor whose zoom changed. Posted from the
    // render thread, applied here, because the controller lives on this thread.
    void ApplyZoom(size_t monitorIndex, double zoom);

    // Re-read monitor rects after WM_DISPLAYCHANGE.
    void ApplyMonitorLayout(const ViewportController& layout);
```

private'a:

```cpp
    ViewportController* m_viewport = nullptr;
    ViewportSnapshot*   m_snapshot = nullptr;

    // Publishes m_viewport into m_snapshot. Called after every mutation.
    void PublishViewport();
```

`InputThread.cpp`'de `PublishViewport`:

```cpp
void InputThread::PublishViewport()
{
    if (!m_viewport || !m_snapshot)
        return;

    for (size_t i = 0; i < m_viewport->MonitorCount(); ++i)
    {
        const MonitorViewport& v = m_viewport->Viewport(i);
        MonitorViewportAtomic& a = m_snapshot->Monitor(i);
        a.srcOriginX.store(v.srcOriginX, std::memory_order_relaxed);
        a.srcOriginY.store(v.srcOriginY, std::memory_order_relaxed);
        a.zoom.store(v.zoom, std::memory_order_relaxed);
    }
    m_snapshot->generation.fetch_add(1, std::memory_order_release);
}
```

Bu turda `V_f` hâlâ ham imleç: `LowLevelMouseProc`'ta `WM_MOUSEMOVE` için (hijack bayrağından **bağımsız**, o bayrak yalnızca kısayolları yönetiyor):

```cpp
    if (wParam == WM_MOUSEMOVE && data && s_instance->m_viewport)
    {
        const double px = static_cast<double>(data->pt.x);
        const double py = static_cast<double>(data->pt.y);
        const int    mi = s_instance->m_viewport->MonitorIndexAt(px, py);
        if (mi >= 0)
        {
            s_instance->m_viewport->OnPointerMoved(static_cast<size_t>(mi), px, py);
            s_instance->m_snapshot->pointerX.store(px, std::memory_order_relaxed);
            s_instance->m_snapshot->pointerY.store(py, std::memory_order_relaxed);
            s_instance->PublishViewport();
        }
    }
```

Olay **yutulmuyor** — Task 6'ya kadar davranış aynı kalıyor.

- [ ] **Step 3: Render thread'i snapshot'tan okut**

`src/App.cpp`, satır ~592-640'taki `srcRect` bloğunu değiştir. `focalPoint` tabanlı hesap ve onu açıklayan uzun Türkçe yorum bloğu **tamamen siliniyor**; yerine:

```cpp
        // The source rectangle comes from ViewportController, which lives on
        // the input thread and advances per mouse event rather than per frame.
        // The old anchored identity (srcOrigin = focal * (1 - 1/zoom)) is gone:
        // it tied the view to the cursor, which is exactly what edge-push needs
        // to break. Click alignment now comes from the real cursor sitting at
        // round(pointer) instead.
        const MonitorViewportAtomic& vp = m_viewportSnapshot.Monitor(monitorIndex);

        RECT srcRect{};
        srcRect.left = static_cast<long>(vp.srcOriginX.load(std::memory_order_relaxed));
        srcRect.top  = static_cast<long>(vp.srcOriginY.load(std::memory_order_relaxed));

        // Belt and braces: the snapshot can be a tick behind a resolution
        // change, and a source rect outside the texture is a device removal.
        srcRect.left = std::clamp(srcRect.left, 0L, (std::max)(0L, monW - srcW));
        srcRect.top  = std::clamp(srcRect.top,  0L, (std::max)(0L, monH - srcH));

        srcRect.right  = srcRect.left + srcW;
        srcRect.bottom = srcRect.top  + srcH;
```

`App.h`'ye `ViewportController m_viewport;` ve `ViewportSnapshot m_viewportSnapshot;` üyeleri ekle; `App` başlatmada `m_inputThread.Attach(&m_viewport, &m_viewportSnapshot)` çağır ve monitör listesini `SetMonitorCount` / `SetMonitorRect` ile doldur.

`MonitorManager::SetZoom` / `AdjustZoom` / `ToggleZoom` çağrılan her yerde `m_inputThread.ApplyZoom(index, newZoom)` da çağrılmalı — aksi halde controller zoom'u bilmez. `WM_DISPLAYCHANGE` handler'ında `ApplyMonitorLayout` çağır.

- [ ] **Step 4: Elle doğrula**

```bash
.\bm.ps1 run
```

Kontrol listesi:
- [ ] Zoom açılıyor, görüntü büyüyor
- [ ] `Ctrl+Alt+tekerlek` zoom adımı çalışıyor
- [ ] İki monitörde farklı zoom seviyeleri bağımsız
- [ ] Fare kenara yaklaşınca görünüm kayıyor (edge-push artık canlı)
- [ ] `.\bm.ps1 errors` temiz

Tıklama hizası **bu turda bozuk** — beklenen. Task 6 ve 8 onu düzeltiyor.

- [ ] **Step 5: Commit**

```bash
git add src/ViewportSnapshot.h src/App.h src/App.cpp src/InputThread.h src/InputThread.cpp BetterMagnifier.vcxproj BetterMagnifier.vcxproj.filters
git commit -m "refactor(render): drive the source rect from ViewportController, drop the anchor identity"
```

---

### Task 6: `PointerInput` — ölçekli imleç

**Files:**
- Create: `src/PointerInput.h`, `src/PointerInput.cpp`
- Modify: `src/InputThread.cpp` (`LowLevelMouseProc`), `src/InputThread.h`
- Modify: `BetterMagnifier.vcxproj`, `BetterMagnifier.vcxproj.filters`

**Interfaces:**
- Consumes: `ViewportController`, `ViewportSnapshot`
- Produces: `PointerInput::Attach`, `SetEnabled(bool)`, `SetSpeed(float)`, `OnMouseMove(const MSLLHOOKSTRUCT&) -> bool`, `Resync()`, `GetPointer(double&, double&)`

- [ ] **Step 1: Header'ı yaz**

`src/PointerInput.h`:

```cpp
#pragma once

// Scales mouse motion by 1/zoom so hand movement maps 1:1 to on-screen
// movement, and keeps the real OS cursor at round(V).
//
// Why the real cursor still moves: clicks, hover, drag and every app's own
// hit-testing use the OS cursor position. Keeping it at round(V) is what makes
// a click land on the thing the magnified sprite is pointing at, with no
// coordinate remapping anywhere.
//
// Only WM_MOUSEMOVE is consumed. Buttons and the wheel pass through untouched.
//
// Thread ownership: the input thread, called from inside the low-level hook.
// Everything here is arithmetic plus one SetCursorPos.

#ifndef BETTER_MAGNIFIER_POINTER_INPUT_H
#define BETTER_MAGNIFIER_POINTER_INPUT_H

#include "ViewportController.h"
#include "ViewportSnapshot.h"

#include <windows.h>
#include <atomic>

namespace BetterMagnifier {

class PointerInput
{
public:
    void Attach(ViewportController* controller, ViewportSnapshot* snapshot);

    // Off means native behaviour: no swallowing, no SetCursorPos, and the
    // renderer draws no sprite. Gated on SystemCursor::MagPathAvailable by the
    // caller, because hiding the pointer without a process-scoped undo is not
    // an acceptable default.
    void SetEnabled(bool enabled);
    bool Enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    void SetSpeed(float speed);

    // Returns true when the event was consumed: the caller must return 1 from
    // the hook and must NOT chain.
    bool OnMouseMove(const MSLLHOOKSTRUCT& data);

    // Snap V to the OS cursor. Call on zoom-on, WM_DISPLAYCHANGE and unlock.
    void Resync();

    void GetPointer(double& x, double& y) const;

private:
    ViewportController* m_viewport = nullptr;
    ViewportSnapshot*   m_snapshot = nullptr;

    std::atomic<bool>  m_enabled{false};
    std::atomic<float> m_speed{1.0f};

    // Not atomic: touched only from the hook, which runs on one thread.
    double m_x = 0.0;
    double m_y = 0.0;
    POINT  m_lastSet{0, 0};
    int    m_lastMonitor = -1;
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_POINTER_INPUT_H
```

- [ ] **Step 2: Implementasyonu yaz**

`src/PointerInput.cpp`:

```cpp
#include "pch.h"
#include "PointerInput.h"
#include "Logger.h"

#include <cmath>

namespace BetterMagnifier {

void PointerInput::Attach(ViewportController* controller, ViewportSnapshot* snapshot)
{
    m_viewport = controller;
    m_snapshot = snapshot;
    Resync();
}

void PointerInput::SetSpeed(float speed)
{
    m_speed.store(speed > 0.01f ? speed : 1.0f, std::memory_order_relaxed);
}

void PointerInput::SetEnabled(bool enabled)
{
    if (m_enabled.exchange(enabled) == enabled)
        return;

    Resync();
    if (m_snapshot)
        m_snapshot->pointerScaled.store(enabled, std::memory_order_relaxed);
}

void PointerInput::Resync()
{
    POINT p{};
    if (!GetCursorPos(&p))
        return;

    m_x = static_cast<double>(p.x);
    m_y = static_cast<double>(p.y);
    m_lastSet = p;
    m_lastMonitor = m_viewport ? m_viewport->MonitorIndexAt(m_x, m_y) : -1;

    if (m_snapshot)
    {
        m_snapshot->pointerX.store(m_x, std::memory_order_relaxed);
        m_snapshot->pointerY.store(m_y, std::memory_order_relaxed);
    }
}

void PointerInput::GetPointer(double& x, double& y) const { x = m_x; y = m_y; }

bool PointerInput::OnMouseMove(const MSLLHOOKSTRUCT& data)
{
    if (!m_viewport || !m_snapshot)
        return false;

    const POINT p = data.pt;

    // Our own SetCursorPos comes back through the hook as an injected event at
    // exactly the position we set. Anything else injected is a foreign
    // SetCursorPos (a game, RDP, returning from Ctrl+Alt+Del) and means our V
    // is stale, so we resync instead of treating it as motion.
    if (data.flags & LLMHF_INJECTED)
    {
        if (p.x == m_lastSet.x && p.y == m_lastSet.y)
            return true;           // our echo: consume, do nothing

        Resync();
        return false;
    }

    if (!m_enabled.load(std::memory_order_relaxed))
    {
        m_x = static_cast<double>(p.x);
        m_y = static_cast<double>(p.y);
        m_lastSet = p;
        m_snapshot->pointerX.store(m_x, std::memory_order_relaxed);
        m_snapshot->pointerY.store(m_y, std::memory_order_relaxed);
        return false;
    }

    int monitor = m_viewport->MonitorIndexAt(m_x, m_y);
    if (monitor < 0)
    {
        Resync();
        return false;
    }

    const double zoom = m_viewport->Zoom(static_cast<size_t>(monitor));

    // Unmagnified monitor: leave the pointer entirely alone. This early-out is
    // what keeps the hook free during normal use.
    if (zoom <= 1.0)
    {
        m_x = static_cast<double>(p.x);
        m_y = static_cast<double>(p.y);
        m_lastSet = p;
        m_lastMonitor = monitor;
        m_snapshot->pointerX.store(m_x, std::memory_order_relaxed);
        m_snapshot->pointerY.store(m_y, std::memory_order_relaxed);
        return false;
    }

    const double scale = static_cast<double>(m_speed.load(std::memory_order_relaxed)) / zoom;
    m_x += (static_cast<double>(p.x) - static_cast<double>(m_lastSet.x)) * scale;
    m_y += (static_cast<double>(p.y) - static_cast<double>(m_lastSet.y)) * scale;

    m_viewport->ClampPointerToDesktop(m_x, m_y);

    const int nowOn = m_viewport->MonitorIndexAt(m_x, m_y);
    if (nowOn >= 0 && nowOn != monitor)
    {
        // Crossed onto another monitor. Preserve that monitor's view and slide
        // the pointer onto the edge we came through.
        const MonitorViewport& from = m_viewport->Viewport(static_cast<size_t>(monitor));
        const Edge entry = (m_x >= from.originX + from.width) ? Edge::Left
                         : (m_x <  from.originX)              ? Edge::Right
                         : (m_y >= from.originY + from.height) ? Edge::Top
                                                               : Edge::Bottom;
        m_viewport->PlaceOnEntry(static_cast<size_t>(nowOn), entry, m_x, m_y);
        monitor = nowOn;
    }

    if (monitor >= 0)
        m_viewport->OnPointerMoved(static_cast<size_t>(monitor), m_x, m_y);

    const POINT target{ static_cast<LONG>(std::lround(m_x)),
                        static_cast<LONG>(std::lround(m_y)) };
    SetCursorPos(target.x, target.y);
    m_lastSet = target;
    m_lastMonitor = monitor;

    m_snapshot->pointerX.store(m_x, std::memory_order_relaxed);
    m_snapshot->pointerY.store(m_y, std::memory_order_relaxed);

    return true;   // swallow: the OS must not move the cursor itself
}

} // namespace BetterMagnifier
```

- [ ] **Step 3: Hook'a bağla ve gecikmeyi ölç**

`InputThread.h`'ye `PointerInput m_pointer;` ve `PointerInput& Pointer() { return m_pointer; }` ekle. `Attach` içinde `m_pointer.Attach(controller, snapshot)` çağır.

`LowLevelMouseProc`'ta Task 5'te eklenen `WM_MOUSEMOVE` bloğunu şununla değiştir. **Bu blok `m_hijackMagnifierKeys` kontrolünün DIŞINDA olmalı** — o bayrak yalnızca kısayolları yönetiyor, imleci değil:

```cpp
    if (nCode == HC_ACTION && s_instance && wParam == WM_MOUSEMOVE && lParam)
    {
#ifdef _DEBUG
        const LARGE_INTEGER t0 = QpcNow();
#endif
        const bool consumed = s_instance->m_pointer.OnMouseMove(
            *reinterpret_cast<MSLLHOOKSTRUCT*>(lParam));

        if (s_instance->m_snapshot)
            s_instance->PublishViewport();

#ifdef _DEBUG
        // Exceeding LowLevelHooksTimeout (300 ms) makes Windows uninstall the
        // hook silently, so the cost of this callback is measured rather than
        // assumed. Logged once a second at most.
        LogHookLatency(QpcNow().QuadPart - t0.QuadPart);
#endif
        if (consumed)
            return 1;
    }
```

`QpcNow()` ve `LogHookLatency()`'yi `InputThread.cpp`'de anonim namespace'e koy: `LogHookLatency` çalışan bir maksimum tutup saniyede bir `LOG_DEBUG("mouse hook max {:.3f} ms over last 1s", ms)` yazsın.

- [ ] **Step 4: Elle doğrula**

Ölçekleme henüz `SetEnabled` ile açılmıyor (Task 8'de açılacak), yani bu turda davranış değişmemeli:

```bash
.\bm.ps1 run
```

- [ ] Fare normal hızda hareket ediyor (ölçekleme kapalı)
- [ ] Zoom, edge-push, kısayollar Task 5'teki gibi
- [ ] `.\bm.ps1 log` içinde `mouse hook max ... ms` satırları var ve değer **< 1 ms**

Değer 1 ms'yi aşıyorsa `SetCursorPos`'u hook'tan çıkarıp input thread'in mesaj döngüsüne taşı (spec §5.1'deki yedek) ve tekrar ölç.

- [ ] **Step 5: Commit**

```bash
git add src/PointerInput.h src/PointerInput.cpp src/InputThread.h src/InputThread.cpp BetterMagnifier.vcxproj BetterMagnifier.vcxproj.filters
git commit -m "feat(input): scale mouse motion by 1/zoom and drive the real cursor"
```

---

### Task 7: İmleç şeklini BGRA'ya çözme

**Files:**
- Create: `src/CursorRenderer.h`, `src/CursorRenderer.cpp`
- Modify: `src/main.cpp` (`--dump-cursors` harness'ı)
- Modify: `BetterMagnifier.vcxproj`, `BetterMagnifier.vcxproj.filters`

**Interfaces:**
- Consumes: yok
- Produces: `struct CursorBitmap { std::vector<uint32_t> pixels; int width, height, hotspotX, hotspotY; }`, `bool DecodeCursor(HCURSOR, CursorBitmap&)`

- [ ] **Step 1: Header'ı yaz**

`src/CursorRenderer.h` (bu turda yalnızca çözme kısmı; çizim Task 8'de eklenir):

```cpp
#pragma once

// Decodes a system cursor into straight BGRA pixels so it can be uploaded as a
// texture and drawn magnified.
//
// The monochrome path is not optional. Cursors with hbmColor == NULL store a
// double-height mask: the top half is AND, the bottom half is XOR. I-beam is
// monochrome and is the cursor a magnifier user spends the most time looking
// at, so skipping this path would leave the feature visibly half-built.

#ifndef BETTER_MAGNIFIER_CURSOR_RENDERER_H
#define BETTER_MAGNIFIER_CURSOR_RENDERER_H

#include <windows.h>
#include <cstdint>
#include <vector>

namespace BetterMagnifier {

struct CursorBitmap
{
    std::vector<uint32_t> pixels;   // BGRA, premultiplied, row-major, top-down
    int width    = 0;
    int height   = 0;
    int hotspotX = 0;
    int hotspotY = 0;
};

// False when the handle is null or GDI refuses it. Never throws.
bool DecodeCursor(HCURSOR cursor, CursorBitmap& out);

// Writes a 32-bit BMP. Debug harness only (--dump-cursors).
bool WriteBitmapFile(const CursorBitmap& bmp, const wchar_t* path);

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_CURSOR_RENDERER_H
```

- [ ] **Step 2: Çözücüyü yaz**

`src/CursorRenderer.cpp`:

```cpp
#include "pch.h"
#include "CursorRenderer.h"
#include "Logger.h"

#include <cstdio>

namespace BetterMagnifier {
namespace {

// Reads a bitmap into a top-down BGRA buffer via GetDIBits.
bool ReadBitmapBits(HBITMAP bmp, int width, int height, std::vector<uint32_t>& out)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -height;    // negative = top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    out.assign(static_cast<size_t>(width) * height, 0u);

    HDC screen = GetDC(nullptr);
    const int copied = GetDIBits(screen, bmp, 0, static_cast<UINT>(height),
                                 out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    return copied == height;
}

bool HasRealAlpha(const std::vector<uint32_t>& px)
{
    for (uint32_t p : px)
        if ((p & 0xFF000000u) != 0u)
            return true;
    return false;
}

} // namespace

bool DecodeCursor(HCURSOR cursor, CursorBitmap& out)
{
    if (!cursor)
        return false;

    ICONINFO info{};
    if (!GetIconInfo(cursor, &info))
        return false;

    struct Cleanup {
        HBITMAP color, mask;
        ~Cleanup() { if (color) DeleteObject(color); if (mask) DeleteObject(mask); }
    } cleanup{ info.hbmColor, info.hbmMask };

    BITMAP maskBm{};
    if (!GetObjectW(info.hbmMask, sizeof(maskBm), &maskBm))
        return false;

    out.hotspotX = static_cast<int>(info.xHotspot);
    out.hotspotY = static_cast<int>(info.yHotspot);

    if (info.hbmColor)
    {
        BITMAP colorBm{};
        if (!GetObjectW(info.hbmColor, sizeof(colorBm), &colorBm))
            return false;

        out.width  = colorBm.bmWidth;
        out.height = colorBm.bmHeight;

        std::vector<uint32_t> color;
        if (!ReadBitmapBits(info.hbmColor, out.width, out.height, color))
            return false;

        if (HasRealAlpha(color))
        {
            out.pixels = std::move(color);
            return true;
        }

        // 32-bit but no alpha channel: the AND mask carries transparency.
        std::vector<uint32_t> mask;
        if (!ReadBitmapBits(info.hbmMask, out.width, out.height, mask))
            return false;

        out.pixels.assign(color.size(), 0u);
        for (size_t i = 0; i < color.size(); ++i)
        {
            const bool transparent = (mask[i] & 0x00FFFFFFu) != 0u;   // AND = 1
            out.pixels[i] = transparent ? 0u : (color[i] | 0xFF000000u);
        }
        return true;
    }

    // ── Monochrome: hbmMask is 2x height, AND on top, XOR below ──
    out.width  = maskBm.bmWidth;
    out.height = maskBm.bmHeight / 2;
    if (out.width <= 0 || out.height <= 0)
        return false;

    std::vector<uint32_t> both;
    if (!ReadBitmapBits(info.hbmMask, out.width, maskBm.bmHeight, both))
        return false;

    const size_t plane = static_cast<size_t>(out.width) * out.height;
    out.pixels.assign(plane, 0u);

    for (size_t i = 0; i < plane; ++i)
    {
        const bool andBit = (both[i]         & 0x00FFFFFFu) != 0u;
        const bool xorBit = (both[i + plane] & 0x00FFFFFFu) != 0u;

        // AND=0,XOR=0 -> black.  AND=0,XOR=1 -> white.
        // AND=1,XOR=0 -> transparent.
        // AND=1,XOR=1 -> invert screen; we approximate with white, because we
        // draw into our own render target and cannot read the screen back.
        if (andBit && !xorBit)      out.pixels[i] = 0x00000000u;
        else if (andBit && xorBit)  out.pixels[i] = 0xFFFFFFFFu;
        else if (xorBit)            out.pixels[i] = 0xFFFFFFFFu;
        else                        out.pixels[i] = 0xFF000000u;
    }
    return true;
}

bool WriteBitmapFile(const CursorBitmap& bmp, const wchar_t* path)
{
    if (bmp.pixels.empty()) return false;

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    const DWORD dataSize = static_cast<DWORD>(bmp.pixels.size() * 4);

    fh.bfType    = 0x4D42;   // "BM"
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + dataSize;

    ih.biSize     = sizeof(ih);
    ih.biWidth    = bmp.width;
    ih.biHeight   = -bmp.height;   // top-down
    ih.biPlanes   = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bmp.pixels.data(), 1, dataSize, f);
    fclose(f);
    return true;
}

} // namespace BetterMagnifier
```

- [ ] **Step 3: Harness'ı ekle**

`src/main.cpp`'de `--self-check` bloğunun yanına:

```cpp
    if (std::wcsstr(GetCommandLineW(), L"--dump-cursors") != nullptr)
    {
        struct { const wchar_t* name; LPCWSTR id; } kShapes[] = {
            { L"arrow",  IDC_ARROW  },
            { L"ibeam",  IDC_IBEAM  },   // monochrome path
            { L"wait",   IDC_WAIT   },   // colour with alpha
            { L"hand",   IDC_HAND   },
            { L"sizeall",IDC_SIZEALL},
        };
        for (const auto& s : kShapes)
        {
            BetterMagnifier::CursorBitmap bmp;
            if (!BetterMagnifier::DecodeCursor(LoadCursorW(nullptr, s.id), bmp))
            {
                LOG_ERROR("DecodeCursor failed for {}", s.name);
                return 1;
            }
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%ls\\bm-cursor-%ls.bmp", _wgetenv(L"TEMP"), s.name);
            BetterMagnifier::WriteBitmapFile(bmp, path);
            LOG_INFO("cursor {}: {}x{} hotspot ({},{})",
                     s.name, bmp.width, bmp.height, bmp.hotspotX, bmp.hotspotY);
        }
        return 0;
    }
```

- [ ] **Step 4: Çıktıyı gözle doğrula**

```bash
.\bm.ps1
.\bin\Debug-x64\BetterMagnifier.exe --dump-cursors
```

Beklenen: çıkış kodu 0, `%TEMP%\bm-cursor-*.bmp` beş dosya. **Her birini aç ve bak.** Özellikle `bm-cursor-ibeam.bmp` — monokrom yolun doğru olduğunu ancak bu gösterir: tanınabilir bir I-beam görülmeli, düz siyah kare değil. Hotspot değerleri de logdan makul olmalı (ok için ~(0,0), I-beam için ~yarı yükseklik).

- [ ] **Step 5: Commit**

```bash
git add src/CursorRenderer.h src/CursorRenderer.cpp src/main.cpp BetterMagnifier.vcxproj BetterMagnifier.vcxproj.filters
git commit -m "feat(cursor): decode colour and monochrome cursors to BGRA"
```

---

### Task 8: Sprite'ı çiz ve ölçeklemeyi aç

Bu, özelliğin tamamlandığı task. **Faz 0 Yaklaşım 2'ye gönderdiyse yalnızca bu task yeniden yazılır** (sprite ayrı bir layered pencereye çizilir), diğerleri aynen kalır.

**Files:**
- Modify: `src/CursorRenderer.h`, `src/CursorRenderer.cpp` (`CursorRenderer` sınıfı)
- Modify: `src/D3DRenderer.h`, `src/D3DRenderer.cpp` (sprite geçişi)
- Modify: `src/App.cpp` (kare başına çizim + `SetEnabled` kapısı)

**Interfaces:**
- Consumes: `DecodeCursor` (Task 7), `ViewportSnapshot` (Task 5), `PointerInput::SetEnabled` (Task 6), `SystemCursor` (Task 2)
- Produces: `CursorRenderer::Initialize(ID3D11Device*, ID3D11DeviceContext*) -> bool`, `Draw(ID3D11RenderTargetView*, int, int, double, double, float) -> bool`, `Shutdown()`

- [ ] **Step 1: `CursorRenderer` sınıfını header'a ekle**

```cpp
class CursorRenderer
{
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // screenX/screenY are the sprite's top-left in render-target pixels,
    // hotspot already subtracted by the caller. Returns false when there is
    // nothing to draw (the system cursor is hidden, or decoding failed).
    bool Draw(ID3D11RenderTargetView* rtv, int targetWidth, int targetHeight,
              double screenX, double screenY, float scale);

private:
    // Keyed by HCURSOR: shape handles are stable and few, so a small cache
    // avoids a GetIconInfo plus two GetDIBits calls every frame. An animated
    // (.ani) cursor keeps one handle, so this caches its first frame and the
    // spinner looks frozen — accepted, see the spec's out-of-scope list.
    struct Entry {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int width = 0, height = 0, hotspotX = 0, hotspotY = 0;
    };
    std::unordered_map<HCURSOR, Entry> m_cache;

    bool EnsureCached(HCURSOR cursor, Entry& out);

    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_pointSampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState>   m_alphaBlend;
    Microsoft::WRL::ComPtr<ID3D11Buffer>       m_quadCB;
};
```

- [ ] **Step 2: Çizim yolunu implemente et**

Shader — `CursorRenderer.cpp`'de string sabiti olarak, mevcut `D3DRenderer::CompileShader` yardımcısıyla derlenir. Vertex buffer yok: köşeler `SV_VertexID`'den ve sabit tampondaki dikdörtgenden üretiliyor.

```hlsl
cbuffer QuadCB : register(b0)
{
    // Sprite rect in normalised device coordinates: x0, y0, x1, y1.
    float4 rect;
};

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    // Two triangles from a 4-vertex strip.
    float2 corner = float2((id & 1) ? rect.z : rect.x,
                           (id & 2) ? rect.w : rect.y);
    VSOut o;
    o.pos = float4(corner, 0.0f, 1.0f);
    o.uv  = float2((id & 1) ? 1.0f : 0.0f, (id & 2) ? 1.0f : 0.0f);
    return o;
}

Texture2D    gCursor : register(t0);
SamplerState gPoint  : register(s0);

float4 PSMain(VSOut i) : SV_TARGET
{
    // Point sampling, not bilinear: a magnified arrow reads better with crisp
    // edges than as a blur. This is why the cursor gets its own sampler.
    return gCursor.Sample(gPoint, i.uv);
}
```

`Initialize` içindeki üç state:

```cpp
    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &m_pointSampler))) return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable    = TRUE;
    // DecodeCursor emits premultiplied alpha, so the source factor is ONE.
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &m_alphaBlend))) return false;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth      = sizeof(float) * 4;
    cb.Usage          = D3D11_USAGE_DYNAMIC;
    cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &m_quadCB))) return false;
```

> `DecodeCursor` düz (premultiplied olmayan) BGRA üretiyor. `ONE / INV_SRC_ALPHA` premultiplied bekliyor, o yüzden **`DecodeCursor`'ın sonuna premultiply adımı ekle**: her piksel için `b,g,r *= a/255`. Alternatif olarak `SRC_ALPHA / INV_SRC_ALPHA` kullanılabilir; premultiply tercih ediliyor çünkü büyütülmüş kenarlarda halka oluşturmuyor. Task 7'nin `CursorBitmap` yorumu zaten "premultiplied" diyor — bu adım onu doğru yapıyor.

`Draw` gövdesi:

```cpp
bool CursorRenderer::Draw(ID3D11RenderTargetView* rtv, int targetWidth,
                          int targetHeight, double screenX, double screenY,
                          float scale)
{
    CURSORINFO ci{ sizeof(ci) };
    if (!GetCursorInfo(&ci) || ci.flags == 0 || !ci.hCursor)
        return false;   // the pointer is legitimately hidden; draw nothing

    Entry e;
    if (!EnsureCached(ci.hCursor, e))
        return false;

    // The hotspot is what the user is actually pointing with, so it, not the
    // sprite's corner, has to land on the computed screen position.
    const double w  = e.width  * static_cast<double>(scale);
    const double h  = e.height * static_cast<double>(scale);
    const double x0 = screenX - e.hotspotX * static_cast<double>(scale);
    const double y0 = screenY - e.hotspotY * static_cast<double>(scale);

    // Pixels -> normalised device coordinates. Y is flipped: NDC grows upward.
    const float rect[4] = {
        static_cast<float>( 2.0 * x0        / targetWidth  - 1.0),
        static_cast<float>( 1.0 - 2.0 * y0        / targetHeight),
        static_cast<float>( 2.0 * (x0 + w)  / targetWidth  - 1.0),
        static_cast<float>( 1.0 - 2.0 * (y0 + h)  / targetHeight),
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_quadCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    memcpy(mapped.pData, rect, sizeof(rect));
    m_context->Unmap(m_quadCB.Get(), 0);

    ID3D11RenderTargetView* targets[] = { rtv };
    m_context->OMSetRenderTargets(1, targets, nullptr);

    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_context->OMSetBlendState(m_alphaBlend.Get(), blendFactor, 0xFFFFFFFF);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->IASetInputLayout(nullptr);
    m_context->VSSetShader(m_vs.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, m_quadCB.GetAddressOf());
    m_context->PSSetShader(m_ps.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, e.srv.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_pointSampler.GetAddressOf());
    m_context->Draw(4, 0);

    // Leave blending off so the next frame's content pass is unaffected.
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    return true;
}
```

`EnsureCached`: `m_cache`'te ara; yoksa `DecodeCursor` + `CreateTexture2D` (`DXGI_FORMAT_B8G8R8A8_UNORM`, `D3D11_USAGE_IMMUTABLE`, `pSysMem = bmp.pixels.data()`, `SysMemPitch = width * 4`) + `CreateShaderResourceView`. Önbellek 32 girdiyi geçerse **tamamen boşalt** — imleç şekilleri az, LRU'ya değmez.

- [ ] **Step 3: Kare döngüsüne bağla ve kapıyı aç**

Önce `src/App.h`'ye üyeleri ekle:

```cpp
    CursorRenderer m_cursorRenderer;
    bool           m_pointerScalingActive = false;
```

Sonra `src/SettingsStore.h`'de `GeneralSettings`'e bu task'ın okuduğu üç alanı ekle. INI'ye yazılmaları Task 11'de; burada yalnızca bellekteki varsayılanlar tanımlanıyor, çünkü kapı mantığı onlar olmadan derlenmez:

```cpp
    // Scale mouse motion by 1/zoom so hand movement maps 1:1 on screen.
    bool  pointerScaling = true;

    // Sprite size relative to zoom. Above 1 draws a pointer larger than the
    // content scale, which low-vision users generally want.
    float cursorScale = 1.0f;

    // Permit the SetSystemCursor fallback when MagShowSystemCursor is
    // unavailable. Off by default: that fallback outlives the process, so a
    // kill from Task Manager would leave the user with no pointer.
    bool  allowUnsafeCursorHide = false;
```

`src/App.cpp`'de, içerik çizildikten sonra ve `Present`'ten önce:

```cpp
        // Draw our own pointer on top of the magnified content.
        //
        // Position: the sprite goes where the user should SEE the pointer,
        // (V - srcOrigin) * zoom. The real OS cursor stays at round(V), which
        // is the source pixel under that sprite — that identity is what keeps
        // clicks landing on what the user is pointing at.
        if (m_viewportSnapshot.pointerScaled.load(std::memory_order_relaxed))
        {
            const double vx = m_viewportSnapshot.pointerX.load(std::memory_order_relaxed);
            const double vy = m_viewportSnapshot.pointerY.load(std::memory_order_relaxed);
            const double sx = (vx - mon->bounds.left - srcRect.left) * zoom;
            const double sy = (vy - mon->bounds.top  - srcRect.top ) * zoom;

            const float scale = static_cast<float>(zoom) * m_settings.General().cursorScale;
            m_cursorRenderer.Draw(rtv, monW, monH, sx, sy, scale);
        }
```

`CursorRenderer::Draw` hotspot çıkarmayı kendisi yapar (önbellekte tutuyor).

Kapı — `App`'te zoom durumu her değiştiğinde:

```cpp
    // Pointer compositing requires hiding the real pointer. Without
    // MagShowSystemCursor the only way to do that is SetSystemCursor, which
    // survives the process — so it stays off unless the user opts in.
    const bool wantScaled = anyMonitorZoomed
                         && m_settings.General().pointerScaling
                         && (SystemCursor::MagPathAvailable()
                             || m_settings.General().allowUnsafeCursorHide);

    if (wantScaled != m_pointerScalingActive)
    {
        m_pointerScalingActive = wantScaled;
        if (wantScaled) SystemCursor::Hide(); else SystemCursor::Restore();
        m_inputThread.Pointer().SetEnabled(wantScaled);
    }
```

Zoom tamamen kapandığında `SystemCursor::Restore()` çağrıldığından emin ol — maruziyet penceresi tam olarak "aktif büyütme" olmalı.

- [ ] **Step 4: Elle doğrula**

```bash
.\bm.ps1 run
```

- [ ] Zoom açıkken büyütülmüş bir imleç görünüyor, gerçek küçük imleç yok
- [ ] **El hareketi ekranda 1:1** — 4x'te imleç fırlamıyor
- [ ] 2×, 4× ve 8×'te bir düğmeye tıkla: **gördüğün yere gidiyor**
- [ ] Metin alanında I-beam doğru çiziliyor
- [ ] Zoom'u kapat → gerçek imleç anında geri geliyor
- [ ] `Ctrl+Alt+Shift+Q` → imleç geri geliyor
- [ ] Uygulamayı Task Manager'dan öldür → imleç geri geliyor (`MagShowSystemCursor` yolundaysa)
- [ ] Kenar bandına it → görünüm kayıyor, imleç bantta duruyor
- [ ] Kaynak bitince imleç fiziksel kenara yürüyor ve yan monitöre geçiyor
- [ ] Geçtiğin monitörün görünümü bıraktığın yerde

- [ ] **Step 5: Commit**

```bash
git add src/CursorRenderer.h src/CursorRenderer.cpp src/D3DRenderer.h src/D3DRenderer.cpp src/App.h src/App.cpp
git commit -m "feat(cursor): composite the magnified pointer and enable input scaling"
```

---

### Task 9: Hook watchdog

Ölü bir hook artık yalnızca kısayolları değil, **imleci** de düşürüyor: gizli ve ölçeklenmemiş bir imleç kalır.

**Files:**
- Modify: `src/InputThread.h`, `src/InputThread.cpp`

**Interfaces:**
- Consumes: `PointerInput`, `SystemCursor`
- Produces: `InputThread` içinde 1 Hz `SetTimer` tabanlı canlılık kontrolü

- [ ] **Step 1: Watchdog'u ekle**

`InputThread::ThreadMain`'in mesaj döngüsünde `SetTimer(nullptr, 0, 1000, nullptr)` kur ve `WM_TIMER`'da:

```cpp
// The OS uninstalls a low-level hook silently when it exceeds
// LowLevelHooksTimeout. Before pointer compositing that cost us shortcuts;
// now it would leave the pointer hidden AND unscaled, so it has to be caught.
//
// There is no API to ask whether a hook is still installed, so we infer it:
// if the OS cursor has moved but no mouse event reached us since the last
// tick, the hook is gone.
POINT now{};
GetCursorPos(&now);
const bool cursorMoved = (now.x != m_lastSeenCursor.x || now.y != m_lastSeenCursor.y);
const bool eventsSeen  = m_mouseEventCounter.load(std::memory_order_relaxed) != m_lastEventCount;

if (cursorMoved && !eventsSeen)
{
    LOG_WARN("Mouse hook appears dead (cursor moved, no events) — reinstalling");
    RemoveHooks();
    if (!InstallHooks())
    {
        LOG_ERROR("Hook reinstall failed — restoring the system pointer");
        SystemCursor::Restore();
        m_pointer.SetEnabled(false);
    }
    else
    {
        m_pointer.Resync();
    }
}

m_lastSeenCursor = now;
m_lastEventCount = m_mouseEventCounter.load(std::memory_order_relaxed);
```

`m_mouseEventCounter`'ı (`std::atomic<uint64_t>`) `LowLevelMouseProc`'un başında artır. `m_lastSeenCursor` (`POINT`) ve `m_lastEventCount` (`uint64_t`) üye olarak ekle.

- [ ] **Step 2: Elle doğrula**

Watchdog'u kasten tetikle: `LowLevelMouseProc`'a geçici `Sleep(400)` koy (`LowLevelHooksTimeout`'u aşar), derle, çalıştır, fareyi oynat.

```bash
.\bm.ps1 run
.\bm.ps1 errors
```

Beklenen: log `Mouse hook appears dead ... reinstalling` yazıyor ve imleç kullanılabilir kalıyor. **`Sleep(400)`'ü sil ve tekrar derle.**

- [ ] **Step 3: Commit**

```bash
git add src/InputThread.h src/InputThread.cpp
git commit -m "feat(input): detect a silently uninstalled mouse hook and recover"
```

---

### Task 10: UIAccess — tespit, manifest ve kurulum betiği

**Files:**
- Create: `tools/install-uiaccess.ps1`, `tools/uninstall-uiaccess.ps1`
- Modify: `src/app.manifest`, `BetterMagnifier.vcxproj`
- Modify: `src/App.cpp` (tespit + z-order dallanması), `src/OverlayWindow.cpp`
- Modify: `docs/STATUS.md`

**Interfaces:**
- Consumes: yok
- Produces: `bool HasUIAccess()` (`App.cpp` anonim namespace), `StatusSnapshot::uiAccess` (`std::atomic<bool>`)

- [ ] **Step 1: Çalışma anı tespiti**

`src/App.cpp`'de:

```cpp
// UIAccess is granted by the token, not by the manifest alone: the binary must
// also be signed and live under a secure path. The same binary runs from bin/
// during development, where it has none of that, so this is checked at run
// time and both paths must work.
bool HasUIAccess()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    DWORD uiAccess = 0, size = 0;
    const BOOL ok = GetTokenInformation(token, TokenUIAccess,
                                        &uiAccess, sizeof(uiAccess), &size);
    CloseHandle(token);
    return ok && uiAccess != 0;
}
```

Başlangıçta bir kez çağır, `LOG_INFO("UIAccess: {}", ...)` ile logla, `StatusSnapshot`'a `std::atomic<bool> uiAccess{false}` ekleyip yaz.

- [ ] **Step 2: Z-order kavgasını dallandır**

`WM_APP_ASSERT_TOPMOST` handler'ının başına:

```cpp
    // With UIAccess our windows sit in a topmost band ordinary windows cannot
    // reach, so menus land below us without any fighting. Re-asserting topmost
    // during menu tracking is what cancelled the menu's mouse tracking, so
    // when we do not need it we must not do it.
    if (m_hasUIAccess)
        return 0;
```

`AppMessages.h:47-52`'deki yorumu, UIAccess'in bunu gereksiz kıldığını söyleyecek şekilde güncelle.

`BM_NO_TOPMOST_FIGHT` ortam anahtarını **sil** (`src/OverlayWindow.cpp` ve onu okuyan yer). Var oluş sebebi iki kötü popup modu arasında seçim yapmaktı; UIAccess o seçimi ortadan kaldırıyor. Kurulu olmayan durumda eski davranış zaten varsayılan olarak kalıyor, yani anahtarın koruduğu bir şey yok. `docs/STATUS.md`'deki ortam değişkenleri tablosundan da çıkar.

- [ ] **Step 3: Manifest**

`src/app.manifest`'te `requestedExecutionLevel`'ı şuna çevir:

```xml
<!-- uiAccess gives what administrator was being used for (reaching elevated
     windows) without the UAC prompt, and it is what lets the overlay sit above
     menus without fighting z-order. It only takes effect when the binary is
     signed and installed under %ProgramFiles% (see tools/install-uiaccess.ps1);
     everywhere else Windows ignores it and the app runs as a normal process. -->
<requestedExecutionLevel level="asInvoker" uiAccess="true" />
```

`BetterMagnifier.vcxproj`'daki dört `ItemDefinitionGroup`'ta `UACExecutionLevel`'ı `AsInvoker` yap ve `<UACUIAccess>true</UACUIAccess>` ekle.

- [ ] **Step 4: Kurulum betiğini yaz**

`tools/install-uiaccess.ps1`:

```powershell
#Requires -RunAsAdministrator
# Signs BetterMagnifier and installs it where UIAccess is honoured.
#
# UIAccess needs three things at once and fails silently if any is missing:
#   1. uiAccess="true" in the manifest  (already in the build)
#   2. an Authenticode signature chaining to LocalMachine\Root
#   3. a location standard users cannot write to (%ProgramFiles%)
param(
    [string]$Configuration = 'Release-x64',
    [string]$Subject       = 'CN=BetterMagnifier Dev'
)
$ErrorActionPreference = 'Stop'

$repo   = Split-Path $PSScriptRoot -Parent
$source = Join-Path $repo "bin\$Configuration\BetterMagnifier.exe"
if (-not (Test-Path $source)) { throw "Not built: $source" }

$cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $Subject -and $_.NotAfter -gt (Get-Date) } |
        Select-Object -First 1

if (-not $cert) {
    Write-Host "Creating code-signing certificate: $Subject"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
              -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)

    $tmp = Join-Path $env:TEMP 'bm-uiaccess.cer'
    Export-Certificate -Cert $cert -FilePath $tmp | Out-Null
    Import-Certificate -FilePath $tmp -CertStoreLocation Cert:\LocalMachine\Root            | Out-Null
    Import-Certificate -FilePath $tmp -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Remove-Item $tmp
}

$target = Join-Path $env:ProgramFiles 'BetterMagnifier'
New-Item -ItemType Directory -Force -Path $target | Out-Null

Get-ChildItem (Split-Path $source -Parent) -File |
    Where-Object { $_.Extension -in '.exe', '.dll', '.pri', '.winmd' } |
    ForEach-Object { Copy-Item $_.FullName -Destination $target -Force }

$exe = Join-Path $target 'BetterMagnifier.exe'
$sig = Set-AuthenticodeSignature -FilePath $exe -Certificate $cert
if ($sig.Status -ne 'Valid') { throw "Signing failed: $($sig.Status) $($sig.StatusMessage)" }

Write-Host "Installed and signed: $exe"
Write-Host "Launch it from there — running from bin\ will NOT have UIAccess."
```

`tools/uninstall-uiaccess.ps1`: `$env:ProgramFiles\BetterMagnifier` klasörünü ve sertifikayı üç depodan da siler.

- [ ] **Step 5: Doğrula**

```bash
.\bm.ps1 release
```

Yükseltilmiş bir PowerShell'de:

```powershell
.\tools\install-uiaccess.ps1
& "$env:ProgramFiles\BetterMagnifier\BetterMagnifier.exe"
```

- [ ] Logda `UIAccess: true`
- [ ] **UAC istemi yok** (artık admin gerekmiyor)
- [ ] Bağlam menüsü açıkken menü canlı ve doğru büyütülmüş
- [ ] `bin\Debug-x64`'ten çalıştır → logda `UIAccess: false`, uygulama yine çalışıyor

`docs/STATUS.md`'deki elevation bölümünü ve açık karar #2'yi güncelle.

- [ ] **Step 6: Commit**

```bash
git add tools/install-uiaccess.ps1 tools/uninstall-uiaccess.ps1 src/app.manifest src/App.cpp src/AppMessages.h src/StatusSnapshot.h BetterMagnifier.vcxproj docs/STATUS.md
git commit -m "feat(uiaccess): replace the administrator requirement with UIAccess"
```

---

### Task 11: Ayarların kalıcılığı

**Files:**
- Modify: `src/SettingsStore.h`, `src/SettingsStore.cpp`
- Modify: `src/App.cpp` (ayar uygulama)

**Interfaces:**
- Consumes: yok
- Produces: `GeneralSettings` üzerinde `followMode` (`FollowMode::EdgePush` eklenmiş), `edgeBandFraction`, `pointerScaling`, `pointerSpeed`, `cursorScale`, `allowUnsafeCursorHide`

- [ ] **Step 1: Başarısız assert'leri yaz**

`SettingsStoreSelfCheck()`'e:

```cpp
    // New pointer and edge-push settings survive a save/load round trip.
    {
        SettingsStore s;
        s.MutableGeneral().followMode           = FollowMode::EdgePush;
        s.MutableGeneral().edgeBandFraction     = 0.2f;
        s.MutableGeneral().pointerScaling       = false;
        s.MutableGeneral().pointerSpeed         = 1.5f;
        s.MutableGeneral().cursorScale          = 1.25f;
        s.MutableGeneral().allowUnsafeCursorHide = true;
        assert(s.Save());

        SettingsStore r;
        assert(r.Load());
        assert(r.General().followMode == FollowMode::EdgePush);
        assert(std::abs(r.General().edgeBandFraction - 0.2f)  < 1e-4f);
        assert(r.General().pointerScaling == false);
        assert(std::abs(r.General().pointerSpeed  - 1.5f)  < 1e-4f);
        assert(std::abs(r.General().cursorScale   - 1.25f) < 1e-4f);
        assert(r.General().allowUnsafeCursorHide == true);
    }

    // Nonsensical values fall back to defaults individually, they do not
    // poison the whole file.
    {
        SettingsStore s;
        s.MutableGeneral().edgeBandFraction = 5.0f;    // way out of range
        s.MutableGeneral().pointerSpeed     = -3.0f;
        assert(s.Save());

        SettingsStore r;
        assert(r.Load());
        assert(r.General().edgeBandFraction > 0.0f && r.General().edgeBandFraction <= 0.45f);
        assert(r.General().pointerSpeed > 0.0f);
    }
```

- [ ] **Step 2: Başarısızlığı doğrula**

```bash
.\bm.ps1
.\bin\Debug-x64\BetterMagnifier.exe --self-check
```

Beklenen: derleme hatası (`FollowMode::EdgePush` yok). Bu bu turdaki "kırmızı".

- [ ] **Step 3: Alanları ekle**

`SettingsStore.h`:

```cpp
enum class FollowMode
{
    Mouse,           // the view is centred on the pointer (legacy behaviour)
    MouseAndFocus,   // also follows EVENT_OBJECT_FOCUS
    EdgePush,        // the view holds still until the pointer reaches a band
};
```

`GeneralSettings`'e:

```cpp
    // EdgePush by default: the view holding still is far less disorienting
    // than one that slides on every mouse move, and with our own pointer the
    // click alignment that used to force Mouse mode is preserved either way.
    FollowMode followMode = FollowMode::EdgePush;

    // Band width as a fraction of the axis, clamped to [80, 300] screen px at
    // use time by ViewportController::BandPx.
    float edgeBandFraction = 0.12f;

    float pointerSpeed = 1.0f;
```

`pointerScaling`, `cursorScale` ve `allowUnsafeCursorHide` **Task 8'de eklendi** — burada yeniden tanımlama, yalnızca INI'ye bağla.

`SettingsStore.cpp`'de altı alanın da INI okuma/yazmasını mevcut deseni izleyerek ekle; `edgeBandFraction`'ı `[0.02, 0.45]`, `pointerSpeed`'i `[0.1, 5.0]`, `cursorScale`'i `[0.5, 4.0]` aralığına kırp. `followMode` metni: `"mouse"`, `"mouseandfocus"`, `"edgepush"`.

- [ ] **Step 4: Ayarları uygula ve testleri geçir**

`App`'in `WM_APP_SETTINGS_CHANGED` handler'ında:

```cpp
    EdgePushConfig cfg;
    cfg.enabled      = (m_settings.General().followMode == FollowMode::EdgePush);
    cfg.bandFraction = m_settings.General().edgeBandFraction;
    m_inputThread.ApplyEdgePushConfig(cfg);
    m_inputThread.Pointer().SetSpeed(m_settings.General().pointerSpeed);
```

`InputThread::ApplyEdgePushConfig(const EdgePushConfig&)` ekle (controller'a iletir + `PublishViewport`).

```bash
.\bm.ps1
.\bin\Debug-x64\BetterMagnifier.exe --self-check
.\bm.ps1 release
```

Beklenen: çıkış kodu 0, iki build de temiz.

- [ ] **Step 5: Commit**

```bash
git add src/SettingsStore.h src/SettingsStore.cpp src/App.cpp src/InputThread.h src/InputThread.cpp
git commit -m "feat(settings): persist edge-push, pointer scaling and cursor settings"
```

---

### Task 12: Panel — yeni ayarlar ve varsayılan açık

**Files:**
- Modify: `src/ControlPanel.cpp`
- Modify: `src/App.cpp` (`BM_PANEL` kapısının kaldırılması)
- Modify: `docs/STATUS.md`, `docs/PANEL-BLANK.md`

**Interfaces:**
- Consumes: Task 11'in `GeneralSettings` alanları, `StatusSnapshot::uiAccess` (Task 10)
- Produces: yok

- [ ] **Step 1: Mevcut paneli elle doğrula**

`BM_PANEL=1` ile aç ve STATUS.md'nin beklediği kontrolü yap:

```powershell
[Environment]::SetEnvironmentVariable('BM_PANEL', '1', 'User')
```

- [ ] Bir zoom slider'ını sürükle → **o monitörün** zoom'u değişiyor, diğeri değişmiyor
- [ ] Bir ayarı değiştir, uygulamayı kapat, tekrar aç → değişiklik duruyor
- [ ] Panel açıkken uygulamadan çık → temiz kapanıyor, asılı kalan süreç yok

Herhangi biri başarısızsa **burada dur ve düzelt**; panel varsayılan açılmadan önce bunlar geçmeli.

- [ ] **Step 2: Yeni ayar kontrollerini ekle**

`ControlPanel.cpp`'nin Ayarlar bölümüne, mevcut satır desenini izleyerek:

| Kontrol | Alan | Aralık |
|---|---|---|
| ComboBox "Follow mode" | `followMode` | Edge push / Centered on pointer / Edge push + keyboard focus |
| Slider "Edge band" | `edgeBandFraction` | 2% – 45%, yüzde olarak göster |
| ToggleSwitch "Scale pointer with zoom" | `pointerScaling` | — |
| Slider "Pointer speed" | `pointerSpeed` | 0.1 – 5.0 |
| Slider "Pointer size" | `cursorScale` | 0.5 – 4.0 |

`TextBox` ve `NumberBox` **kullanma** — `PANEL-BLANK.md`'ye göre gömülü `TextBox` içeren her kontrol stowed exception ile çöküyor. `Slider` ve `ComboBox` güvenli.

`SystemCursor::MagPathAvailable()` false ise "Scale pointer with zoom" anahtarının altına uyarı satırı ve `allowUnsafeCursorHide` için ayrı bir ToggleSwitch ekle:

> "Windows would not let us hide the pointer the safe way. The fallback stays in effect if this app is killed, and you would have no mouse pointer until you sign out."

`StatusSnapshot::uiAccess` false ise Ayarlar'ın en üstüne durum satırı:

> "Not installed for UIAccess. Menus may appear unmagnified. Run tools\install-uiaccess.ps1 as administrator to fix this."

Sıralama kuralı korunmalı: `SettingsStore` yaz → `Save()` → **sonra** `WM_APP_SETTINGS_CHANGED` gönder.

- [ ] **Step 3: Monitör kartlarına imleç göstergesi**

10 Hz yenileme döngüsünde, `ViewportSnapshot::pointerX/Y`'yi `ViewportController::MonitorIndexAt` ile eşleştirip aktif kartın başlığına bir nokta/işaret koy. Hangi kartın slider'ının etki edeceğini bu söylüyor.

- [ ] **Step 4: Kapıyı kaldır**

`src/App.cpp`'de `BM_PANEL` kontrolünü sil: tepsi menüsündeki "Settings..." her zaman görünsün. **Başlangıçta paneli otomatik açma** — `BM_PANEL=1`'in eski davranışıydı ve varsayılan olarak istenmiyor.

`docs/STATUS.md` ile `docs/PANEL-BLANK.md`'de paneli "OFF by default" diye tanımlayan bölümleri güncelle.

- [ ] **Step 5: Doğrula**

```bash
.\bm.ps1 release
```

```powershell
[Environment]::SetEnvironmentVariable('BM_PANEL', $null, 'User')
```

- [ ] Tepsi → sağ tık → Settings paneli açıyor, `BM_PANEL` gerekmiyor
- [ ] Follow mode "Centered on pointer"a çevrilince edge-push duruyor
- [ ] "Pointer speed" sürüklenince fare hissi anında değişiyor
- [ ] "Pointer size" sprite'ı büyütüyor
- [ ] Yeniden başlatınca hepsi duruyor
- [ ] `.\bm.ps1 errors` temiz

- [ ] **Step 6: Commit**

```bash
git add src/ControlPanel.cpp src/App.cpp docs/STATUS.md docs/PANEL-BLANK.md
git commit -m "feat(panel): expose pointer and edge-push settings, enable by default"
```

---

## Kapanış kontrol listesi

Task 12'den sonra, spec §10.4'ün elle listesini baştan sona koş:

- [ ] 2× / 4× / 8×'te tıklama hizası
- [ ] Bağlam menüsü canlı ve büyütülmüş, vurgu fareyi takip ediyor
- [ ] İki yönde monitör geçişi, her iki monitörde farklı zoom ile
- [ ] Görünüm hedef monitörde bırakıldığı yerde
- [ ] Task Manager'dan öldürüldükten sonra imleç geri geliyor
- [ ] Kilit → açma sonrası imleç ve hook'lar sağlam
- [ ] Panel değişikliği restart'tan sağ çıkıyor
- [ ] Metin alanında I-beam doğru çiziliyor
- [ ] `.\bm.ps1` ve `.\bm.ps1 release` ikisi de temiz
- [ ] `.\bin\Debug-x64\BetterMagnifier.exe --self-check` çıkış kodu 0

Sonra `docs/STATUS.md`'yi güncelle: açık kararlar 0–5 kapandı, ölçüm sonuçları yazıldı, kalan bilinen sınırlamalar (ham girdi tüketicileri, animasyonlu imleçler, UAC secure desktop) listelendi.
