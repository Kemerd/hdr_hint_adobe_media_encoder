// ---------------------------------------------------------------------------
// Logger.h - rolling text log shared by every thread.
//
//   HH_LOG_INFO(L"Tailer", L"read {} bytes from {}", n, path);
//
// Format strings are wide (std::format with wchar_t). Arguments must be wide
// strings or numbers; convert UTF-8 with hh::platform::toWide first.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Win.h"

#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace hh {

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

/**
 * @brief Process-wide logger. Thread-safe. Writes UTF-8 lines:
 *        "2026-09-16 14:25:06.123 -07:00 [INFO ] [T1234] [Tailer] message"
 */
class Logger {
public:
    static Logger& instance();

    /**
     * @brief Opens <directory>\hdrhint.log, rotating older files first.
     * @param maxSizeKb  rotate when the current file exceeds this size
     * @param keepFiles  number of rotated files to keep (hdrhint.1.log ...)
     */
    Result<void> open(const std::wstring& directory, LogLevel level, uint32_t maxSizeKb, uint32_t keepFiles);
    /// Flushes and closes the file. Logging afterwards goes to OutputDebugString only.
    void close();

    void setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    [[nodiscard]] bool enabled(LogLevel level) const noexcept;

    /// Writes one line (no trailing newline needed). Safe from any thread.
    void write(LogLevel level, std::wstring_view component, std::wstring_view message);

    /// Directory the log lives in (empty when closed).
    [[nodiscard]] std::wstring directory() const;
    /// Full path of the current log file (empty when closed).
    [[nodiscard]] std::wstring filePath() const;

    /// "trace|debug|info|warn|error" -> level (case-insensitive), else fallback.
    static LogLevel parseLevel(std::wstring_view text, LogLevel fallback);
    /// Level -> fixed-width tag ("INFO ").
    static const wchar_t* levelTag(LogLevel level) noexcept;

private:
    Logger() = default;
    void rotateLocked();
    void flushLocked();

    mutable std::mutex mutex_;
#if defined(_WIN32)
    HANDLE file_ = nullptr;          ///< FILE_APPEND_DATA handle (nullptr = closed)
#else
    int file_ = -1;                  ///< O_APPEND descriptor (-1 = closed)
#endif
    std::wstring directory_;
    std::wstring path_;
    LogLevel level_ = LogLevel::Info;
    uint32_t maxSizeKb_ = 2048;
    uint32_t keepFiles_ = 5;
    uint64_t bytesWritten_ = 0;
    uint64_t lastFlushMs_ = 0;
};

} // namespace hh

// Convenience macros -------------------------------------------------------
#define HH_LOG_AT(lvl, comp, ...)                                                          \
    do {                                                                                   \
        if (::hh::Logger::instance().enabled(lvl)) {                                       \
            ::hh::Logger::instance().write((lvl), (comp), ::std::format(__VA_ARGS__));     \
        }                                                                                  \
    } while (0)

#define HH_LOG_TRACE(comp, ...) HH_LOG_AT(::hh::LogLevel::Trace, comp, __VA_ARGS__)
#define HH_LOG_DEBUG(comp, ...) HH_LOG_AT(::hh::LogLevel::Debug, comp, __VA_ARGS__)
#define HH_LOG_INFO(comp, ...)  HH_LOG_AT(::hh::LogLevel::Info,  comp, __VA_ARGS__)
#define HH_LOG_WARN(comp, ...)  HH_LOG_AT(::hh::LogLevel::Warn,  comp, __VA_ARGS__)
#define HH_LOG_ERROR(comp, ...) HH_LOG_AT(::hh::LogLevel::Error, comp, __VA_ARGS__)
