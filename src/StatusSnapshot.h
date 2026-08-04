#pragma once

// =============================================================================
// StatusSnapshot.h — Motor -> GUI canli durum aktarimi
// =============================================================================
// Render thread her frame buraya yazar, GUI thread 10 Hz okur.
//
// Neden mutex degil atomic?
//   Render thread'in hot path'inde kilit almasi frame suresini kestirilemez
//   yapar — projenin butun amaci laggsizlik. std::atomic<float> ve
//   std::atomic<bool> x64'te lock-free.
//
// Neden 10 Hz okuma yeter?
//   Ayar panelinde 60 Hz gostergeye kimse bakmiyor. 10 Hz'de cekisme sifira
//   iner, insan gozu farki gormez.
//
// Tutarlilik notu: alanlar tek tek atomic, yapinin TAMAMI atomic degil.
// Yani GUI ayni monitorun zoomLevel'ini yeni, isActive'ini bir frame eski
// okuyabilir. Gosterge icin kabul edilebilir — kritik karar alinmiyor.
//
// Python analojisi: her alan icin ayri bir thread-safe kutucuk.
// Yazan beklemez, okuyan beklemez.
// =============================================================================

#ifndef BETTER_MAGNIFIER_STATUS_SNAPSHOT_H
#define BETTER_MAGNIFIER_STATUS_SNAPSHOT_H

#include <atomic>
#include <array>
#include <cstddef>

namespace BetterMagnifier {

struct MonitorStatus
{
    // ── Canli alanlar (her frame guncellenir) ──
    std::atomic<float> zoomLevel{1.0f};
    std::atomic<bool>  isActive{false};
    std::atomic<bool>  isFrozen{false};

    // DXGICapture::IsInitialized() && !NeedsReinit()
    std::atomic<bool>  captureOk{false};

    // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) basarili miydi?
    // false ise overlay kendini yakalar — feedback loop riski, panelde uyari.
    std::atomic<bool>  captureExcluded{false};

    std::atomic<float> fps{0.0f};

    // ── Statik bilgi (sadece init ve WM_DISPLAYCHANGE'de guncellenir) ──
    static constexpr size_t kNameCapacity = 64;

    // deviceName atomic DEGIL: sadece WM_DISPLAYCHANGE aninda yazilir.
    // Kotu senaryoda GUI yarim yazilmis bir isim okur — etiket bozuk gorunur,
    // bir sonraki tick'te duzelir. Cokme riski yok: sabit boyutlu dizi,
    // her zaman null-terminated yaziliyor (wcsncpy_s + _TRUNCATE).
    wchar_t             deviceName[kNameCapacity]{};

    std::atomic<int>    width{0};
    std::atomic<int>    height{0};
    std::atomic<int>    refreshRate{0};
    std::atomic<int>    dpiPercent{100};
    std::atomic<bool>   isPrimary{false};
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

    // HotkeyManager::Reregister sonucu. bit 0 = toggle basarisiz,
    // bit 1 = freeze basarisiz. Panel bunu kirmizi uyari satirinda gosterecek.
    std::atomic<unsigned> hotkeyFailedMask{0};

private:
    std::array<MonitorStatus, kMaxMonitors> m_monitors{};
};

} // namespace BetterMagnifier

#endif // BETTER_MAGNIFIER_STATUS_SNAPSHOT_H
