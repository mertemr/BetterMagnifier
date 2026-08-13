#pragma once

// Thread-safe logger built on std::format and std::source_location.
//
// Writes to a file and to the debugger output at once; every line carries a
// timestamp, thread id, level and file:line. Flushed per line, because the log
// matters most when the process is about to die and buffered output would take
// the interesting part with it.
//
//   LOG_INFO ("Overlay created: {}x{}", w, h);
//   LOG_ERROR("DXGI capture failed: 0x{:08X}", hr);
//   LOG_DEBUG("Frame time: {} ms", deltaMs);   // compiled out in Release

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

enum class LogLevel : int
{
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Fatal = 4,
    Off   = 5
};

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
// One log file and one mutex for the whole process. A magic static gives
// thread-safe lazy construction and destruction at exit, which closes and
// flushes the stream without anyone having to remember to.
class Logger
{
public:
    static Logger& Instance()
    {
        static Logger instance;
        return instance;
    }

    // Call once at startup. Entries below minLevel are dropped.
    bool Initialize(const std::filesystem::path& logDirectory = L".",
                    LogLevel minLevel = LogLevel::Debug)
    {
        std::lock_guard lock(m_mutex);

        m_minLevel = minLevel;

        std::error_code ec;
        std::filesystem::create_directories(logDirectory, ec);
        if (ec)
        {
            OutputDebugStringW(L"[Logger] Could not create the log directory\n");
            return false;
        }

        // One file per run, named for the start time: BetterMagnifier_2026-03-02_11-40-50.log
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
            OutputDebugStringW(L"[Logger] Could not open the log file\n");
            return false;
        }

        m_initialized = true;

        WriteRaw("════════════════════════════════════════════════════════════");
        WriteRaw(std::format("  BetterMagnifier Logger Initialized"));
        WriteRaw(std::format("  Log File: {}", fullPath.string()));
        WriteRaw("════════════════════════════════════════════════════════════");

        return true;
    }

    // source_location is taken as a parameter rather than read here: it has to
    // be captured at the call site, which is what the LOG_* macros do.
    template<typename... Args>
    void Log(LogLevel level,
             const std::source_location& loc,
             std::format_string<Args...> fmt,
             Args&&... args)
    {
        if (level < m_minLevel)
            return;

        std::string message = std::format(fmt, std::forward<Args>(args)...);

        const auto now = std::chrono::system_clock::now();
        const auto timeT = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::tm localTm{};
        localtime_s(&localTm, &timeT);

        // Filename only; source_location gives the full build path, which is
        // noise in every line.
        std::string_view fileFullPath = loc.file_name();
        auto lastSlash = fileFullPath.find_last_of("\\/");
        std::string_view fileName = (lastSlash != std::string_view::npos)
            ? fileFullPath.substr(lastSlash + 1)
            : fileFullPath;

        auto threadId = std::this_thread::get_id();

        // [14:30:45.123] [INFO ] [T:12345] [main.cpp:42] message
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

        {
            std::lock_guard lock(m_mutex);

            if (m_fileStream.is_open())
            {
                m_fileStream << logLine << '\n';

                // Flushed per line. Volume here is low enough that it costs
                // nothing measurable, and the lines worth reading are usually
                // the ones written just before something killed the process.
                m_fileStream.flush();
            }

            std::wstring wLogLine(logLine.begin(), logLine.end());
            wLogLine += L'\n';
            OutputDebugStringW(wLogLine.c_str());

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

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    // Unformatted, for the banner lines. Assumes the mutex is already held.
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

    // std::thread::id has no numeric conversion, so the hash stands in. Only
    // needs to be stable and distinct within a run, not meaningful.
    static unsigned int GetThreadIdAsInt(std::thread::id id)
    {
        return static_cast<unsigned int>(std::hash<std::thread::id>{}(id));
    }

    std::mutex    m_mutex;
    std::ofstream m_fileStream;
    LogLevel      m_minLevel    = LogLevel::Debug;
    bool          m_initialized = false;
};

} // namespace BetterMagnifier

// Macros rather than functions with a defaulted source_location parameter.
// A default argument is evaluated at the call site and would work, but the
// macro keeps the capture visible at the point of use and matches what every
// other logging header in this ecosystem does.

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

// Compiled out entirely in Release, arguments included, so a LOG_DEBUG on a hot
// path costs nothing there.
#ifdef _DEBUG
    #define LOG_DEBUG(fmt, ...)                                                 \
        BetterMagnifier::Logger::Instance().Log(                                \
            BetterMagnifier::LogLevel::Debug,                                   \
            std::source_location::current(),                                    \
            fmt, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(fmt, ...) ((void)0)
#endif

// ── BM_SELFCHECK — assertion for the scriptable self-check suite ──
//
// Deliberately not <cassert>. In a /SUBSYSTEM:WINDOWS build _wassert opens a
// message box, and a process sitting on a dialog hangs whatever script ran it
// — the exact failure the self-check exists to prevent. _set_error_mode and
// _CrtSetReportMode do not cover _wassert, which was measured, not assumed.
//
// Logs the failed expression (the logger flushes every line, so it survives)
// and exits with 2. Callers gate on the exit code: 0 pass, 2 assertion failed.
#ifdef _DEBUG
    #define BM_SELFCHECK(expr)                                                  \
        do {                                                                    \
            if (!(expr))                                                        \
            {                                                                   \
                LOG_FATAL("SELF-CHECK FAILED: {}", #expr);                      \
                _exit(2);                                                       \
            }                                                                   \
        } while (0)
#else
    #define BM_SELFCHECK(expr) ((void)0)
#endif

#endif // BETTER_MAGNIFIER_LOGGER_H
