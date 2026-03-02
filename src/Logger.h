#pragma once

// =============================================================================
// Logger.h — Thread-Safe, RAII-Based Logging System
// =============================================================================
//
// Python analojisi:
//   Python'da: logging.getLogger(__name__).info("mesaj")
//   Burada:    LOG_INFO("mesaj")  veya  LOG_INFO("değer: {}", 42)
//
// Özellikler:
//   1. Thread-safe (mutex ile korunur — Python'daki threading.Lock gibi)
//   2. Hem dosyaya hem VS Output penceresine yazar (OutputDebugStringW)
//   3. Her satırda: timestamp, thread ID, log seviyesi, dosya:satır
//   4. Release build'de LOG_DEBUG tamamen kaldırılır (zero overhead)
//   5. Singleton pattern — tek bir global Logger instance
//   6. RAII — Logger yıkıldığında dosya otomatik kapanır
//
// Kullanım:
//   LOG_INFO("Uygulama başlatıldı");
//   LOG_WARN("Zoom level sınırda: {}", zoomLevel);
//   LOG_ERROR("DXGI capture başarısız: 0x{:08X}", hr);
//   LOG_DEBUG("Frame time: {}ms", deltaMs);  // Release'de yok olur
//
// =============================================================================

#ifndef BETTER_MAGNIFIER_LOGGER_H
#define BETTER_MAGNIFIER_LOGGER_H

#include <string>
#include <string_view>
#include <format>
#include <source_location>
#include <fstream>
#include <mutex>
#include <chrono>
#include <thread>
#include <filesystem>
#include <iostream>
#include <windows.h>

namespace BetterMagnifier {

// ─────────────────────────────────────────────────────────────────────────────
// Log Seviyeleri
// ─────────────────────────────────────────────────────────────────────────────
enum class LogLevel : int
{
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Fatal = 4,
    Off   = 5  // Logging'i tamamen kapat
};

// Seviye ismini string olarak almak için (log satırında gösterilir)
constexpr std::string_view LogLevelToString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    default:              return "?????";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Logger Singleton
// ─────────────────────────────────────────────────────────────────────────────
// Python'da:  logger = logging.getLogger()
// C++'ta:     Logger& log = Logger::Instance()
//
// Singleton neden? Uygulama genelinde tek bir log dosyası ve tek bir mutex.
// Farklı thread'lerden aynı dosyaya yazmak data race oluşturur — mutex bunu engeller.
//
// RAII neden? Logger stackte veya static storage'da yaşar.
// Program sonlandığında destructor otomatik çalışır → dosya kapanır, flush yapılır.
// Python'da "with open() as f:" gibi düşün — ama scope program ömrü kadar geniş.
// ─────────────────────────────────────────────────────────────────────────────
class Logger
{
public:
    // ── Singleton Access ──
    // Python'da:  logger = logging.getLogger()
    // C++'ta:     Logger::Instance()
    // "static local" pattern: İlk çağrıda oluşturulur, program sonunda yıkılır.
    // C++11'den beri thread-safe garantili (magic statics).
    static Logger& Instance()
    {
        static Logger instance;
        return instance;
    }

    // ── Initialization ──
    // Programın en başında bir kez çağrılır.
    // logDirectory: Log dosyasının yazılacağı dizin.
    // minLevel: Bu seviyenin altındaki loglar yazılmaz.
    bool Initialize(const std::filesystem::path& logDirectory = L".",
                    LogLevel minLevel = LogLevel::Debug)
    {
        std::lock_guard lock(m_mutex);  // Python: with self._lock:

        m_minLevel = minLevel;

        // Log dizinini oluştur (yoksa)
        std::error_code ec;
        std::filesystem::create_directories(logDirectory, ec);
        if (ec)
        {
            OutputDebugStringW(L"[Logger] Log dizini oluşturulamadı!\n");
            return false;
        }

        // Dosya adı: BetterMagnifier_2026-03-02_11-40-50.log
        const auto now = std::chrono::system_clock::now();
        const auto timeT = std::chrono::system_clock::to_time_t(now);
        std::tm localTm{};
        localtime_s(&localTm, &timeT);

        auto filename = std::format(L"BetterMagnifier_{:04d}-{:02d}-{:02d}_{:02d}-{:02d}-{:02d}.log",
            localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
            localTm.tm_hour, localTm.tm_min, localTm.tm_sec);

        auto fullPath = logDirectory / filename;

        m_fileStream.open(fullPath, std::ios::out | std::ios::app);
        if (!m_fileStream.is_open())
        {
            OutputDebugStringW(L"[Logger] Log dosyası açılamadı!\n");
            return false;
        }

        m_initialized = true;

        // İlk log satırı
        WriteRaw("════════════════════════════════════════════════════════════");
        WriteRaw(std::format("  BetterMagnifier Logger Initialized"));
        WriteRaw(std::format("  Log File: {}", fullPath.string()));
        WriteRaw("════════════════════════════════════════════════════════════");

        return true;
    }

    // ── Ana Log Fonksiyonu ──
    // Python'da:  logger.info("mesaj %s", değer)
    // C++'ta:     Log(LogLevel::Info, loc, "mesaj {}", değer)
    //
    // std::format kullanıyoruz — Python f-string'in C++20 karşılığı.
    // source_location → __FILE__ ve __LINE__'ın modern, type-safe hali.
    // Makro yerine fonksiyon parametresi olarak geçiyor.
    template<typename... Args>
    void Log(LogLevel level,
             const std::source_location& loc,
             std::format_string<Args...> fmt,
             Args&&... args)
    {
        // Minimum seviye kontrolü — bu seviyenin altındakiler ignore edilir
        if (level < m_minLevel)
            return;

        // Mesajı formatla
        std::string message = std::format(fmt, std::forward<Args>(args)...);

        // Timestamp al
        const auto now = std::chrono::system_clock::now();
        const auto timeT = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::tm localTm{};
        localtime_s(&localTm, &timeT);

        // Dosya adından sadece filename kısmını al (path olmadan)
        std::string_view fileFullPath = loc.file_name();
        auto lastSlash = fileFullPath.find_last_of("\\/");
        std::string_view fileName = (lastSlash != std::string_view::npos)
            ? fileFullPath.substr(lastSlash + 1)
            : fileFullPath;

        // Thread ID
        auto threadId = std::this_thread::get_id();

        // Final log satırı formatı:
        // [14:30:45.123] [INFO ] [T:12345] [main.cpp:42] Mesaj burada
        std::string logLine = std::format(
            "[{:02d}:{:02d}:{:02d}.{:03d}] [{}] [T:{:5}] [{}:{}] {}",
            localTm.tm_hour, localTm.tm_min, localTm.tm_sec,
            static_cast<int>(ms.count()),
            LogLevelToString(level),
            GetThreadIdAsInt(threadId),
            fileName,
            loc.line(),
            message
        );

        // Thread-safe yazma
        {
            std::lock_guard lock(m_mutex);

            // 1. Dosyaya yaz (varsa)
            if (m_fileStream.is_open())
            {
                m_fileStream << logLine << '\n';
                // Her satırda flush — bu app'te log frequency düşük,
                // performans etkisi yok. Crash/kill durumunda veri kaybını önler.
                m_fileStream.flush();
            }

            // 2. VS Output penceresine yaz (Debug build'de çok faydalı!)
            // OutputDebugStringW, Visual Studio'nun "Output" penceresinde görünür.
            // Python'daki print() gibi ama IDE'nin debug output'una gider.
            std::wstring wLogLine(logLine.begin(), logLine.end());
            wLogLine += L'\n';
            OutputDebugStringW(wLogLine.c_str());

            // 3. Console'a da yaz (varsa — debug sırasında console attach edilebilir)
            // Error ve üstünü stderr'e, diğerlerini stdout'a
            if (level >= LogLevel::Error)
                std::cerr << logLine << '\n';
            else
                std::cout << logLine << '\n';
        }
    }

    // ── Flush ──
    void Flush()
    {
        std::lock_guard lock(m_mutex);
        if (m_fileStream.is_open())
            m_fileStream.flush();
    }

    // ── Log Level Ayarla ──
    void SetMinLevel(LogLevel level)
    {
        m_minLevel = level;
    }

    LogLevel GetMinLevel() const
    {
        return m_minLevel;
    }

    // ── Shutdown ──
    void Shutdown()
    {
        std::lock_guard lock(m_mutex);
        if (m_fileStream.is_open())
        {
            WriteRaw("════════════════════════════════════════════════════════════");
            WriteRaw("  Logger Shutdown");
            WriteRaw("════════════════════════════════════════════════════════════");
            m_fileStream.flush();
            m_fileStream.close();
        }
        m_initialized = false;
    }

    bool IsInitialized() const { return m_initialized; }

private:
    // ── Constructor/Destructor (Singleton — private) ──
    Logger() = default;

    ~Logger()
    {
        Shutdown();
    }

    // Singleton — kopyalama ve taşıma yasak
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    // ── Raw yazma (header/footer için) ──
    void WriteRaw(std::string_view text)
    {
        if (m_fileStream.is_open())
        {
            m_fileStream << text << '\n';
            m_fileStream.flush();
        }
        std::wstring wText(text.begin(), text.end());
        wText += L'\n';
        OutputDebugStringW(wText.c_str());
    }

    // ── Thread ID'yi okunabilir integer'a çevir ──
    static unsigned int GetThreadIdAsInt(std::thread::id id)
    {
        // std::thread::id doğrudan integer'a çevrilemez.
        // Bu hack ile hash'ini alıp unsigned int'e cast ediyoruz.
        return static_cast<unsigned int>(std::hash<std::thread::id>{}(id));
    }

    // ── Üye Değişkenler ──
    std::mutex          m_mutex;                            // Thread safety
    std::ofstream       m_fileStream;                       // Log dosyası
    LogLevel            m_minLevel = LogLevel::Debug;       // Minimum log seviyesi
    bool                m_initialized = false;              // Init edildi mi?
};

} // namespace BetterMagnifier

// =============================================================================
// LOG MAKROLARI
// =============================================================================
//
// Neden fonksiyon değil de makro?
// → source_location::current() çağrıldığı yerin bilgisini yakalar.
//   Eğer helper fonksiyon içine koysaydık, her zaman helper'ın
//   satır numarasını gösterirdi — log'da işe yaramaz.
//
// C++20 ile source_location default parametre olarak da geçilebilir,
// ama makro yaklaşımı mevcut C++ ekosisteminde daha yaygın ve tanıdık.
//
// Kullanım:
//   LOG_INFO("Uygulama başlatılıyor...");
//   LOG_WARN("Zoom: {} — sınıra yaklaşıldı", zoomLevel);
//   LOG_ERROR("DXGI hatası: 0x{:08X}", hr);
//   LOG_DEBUG("Frame delta: {}ms", dt);   // Release'de no-op
//
// =============================================================================

#define LOG_INFO(fmt, ...)                                                      \
    BetterMagnifier::Logger::Instance().Log(                                    \
        BetterMagnifier::LogLevel::Info,                                        \
        std::source_location::current(),                                        \
        fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)                                                      \
    BetterMagnifier::Logger::Instance().Log(                                    \
        BetterMagnifier::LogLevel::Warn,                                        \
        std::source_location::current(),                                        \
        fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...)                                                     \
    BetterMagnifier::Logger::Instance().Log(                                    \
        BetterMagnifier::LogLevel::Error,                                       \
        std::source_location::current(),                                        \
        fmt, ##__VA_ARGS__)

#define LOG_FATAL(fmt, ...)                                                     \
    BetterMagnifier::Logger::Instance().Log(                                    \
        BetterMagnifier::LogLevel::Fatal,                                       \
        std::source_location::current(),                                        \
        fmt, ##__VA_ARGS__)

// ── LOG_DEBUG — Release build'de tamamen yok olur ──
// Python'da:  if __debug__: logger.debug(...)
// C++'ta:     #ifdef _DEBUG bloğu. Release build'de preprocessor bu satırları
//             tamamen kaldırır — zero overhead, hiçbir runtime maliyeti yok.
#ifdef _DEBUG
    #define LOG_DEBUG(fmt, ...)                                                 \
        BetterMagnifier::Logger::Instance().Log(                                \
            BetterMagnifier::LogLevel::Debug,                                   \
            std::source_location::current(),                                    \
            fmt, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(fmt, ...) ((void)0)
#endif

#endif // BETTER_MAGNIFIER_LOGGER_H
