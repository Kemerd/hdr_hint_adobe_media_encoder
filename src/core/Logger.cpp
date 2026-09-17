// ---------------------------------------------------------------------------
// Logger.cpp - rolling UTF-8 text log shared by every thread.
//
// One line per call, written through a FILE_APPEND_DATA handle so lines from
// different threads never interleave. Rotation happens in-line the moment the
// current file grows past the cap: hdrhint.log -> hdrhint.1.log -> ... and the
// oldest file beyond keepFiles is dropped.
//
// Everything that touches the handle or the counters is mutex-guarded. The
// logger is usable before open() and after close(); in that state the lines
// go to the debugger (OutputDebugString) only.
// ---------------------------------------------------------------------------
#include "core/Logger.h"

#include "platform/FileIo.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <algorithm>
#include <string>

namespace hh {

namespace {

// File naming and the flush cadence for the non-urgent levels.
constexpr const wchar_t* kLogFileName = L"hdrhint.log";
constexpr uint64_t kFlushIntervalMs = 2000;

// Sanity bounds so a broken settings file cannot make us rotate on every
// line or keep hundreds of old logs around.
constexpr uint32_t kMinSizeKb = 16;
constexpr uint32_t kMaxSizeKb = 1024 * 1024;
constexpr uint32_t kMaxKeepFiles = 99;

/**
 * @brief Builds the path of a rotated file: index 0 is the live log,
 *        index n is "hdrhint.n.log".
 */
std::wstring rotatedName(const std::wstring& directory, uint32_t index) {
    if (index == 0) {
        return directory + L"\\" + kLogFileName;
    }
    return directory + L"\\hdrhint." + std::to_wstring(index) + L".log";
}

/**
 * @brief Removes trailing path separators (keeps a bare drive root usable).
 */
std::wstring stripTrailingSeparators(std::wstring s) {
    while (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    return s;
}

/**
 * @brief Opens the log for appending. Readers (log viewers) and the rotation
 *        rename are allowed through the share mode.
 * @return nullptr on failure (GetLastError() is left intact).
 */
HANDLE openAppend(const std::wstring& path) {
    if (path.empty()) {
        return nullptr;
    }
    const std::wstring extended = platform::toExtendedPath(path);
    if (extended.empty()) {
        return nullptr;
    }
    HANDLE h = ::CreateFileW(extended.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    return h;
}

/**
 * @brief Size of an open file, 0 when it cannot be determined.
 */
uint64_t sizeOfHandle(HANDLE h) {
    if (h == nullptr) {
        return 0;
    }
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(h, &li) || li.QuadPart < 0) {
        return 0;
    }
    return static_cast<uint64_t>(li.QuadPart);
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton / lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief The one process-wide logger. Construction is thread-safe (magic static).
 */
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

/**
 * @brief Opens (or re-opens) the log file inside @p directory.
 *
 * The directory is created on demand. When the existing file is already over
 * the cap it is rotated before the handle is opened so a fresh session never
 * starts on a bloated log.
 */
Result<void> Logger::open(const std::wstring& directory, LogLevel level, uint32_t maxSizeKb, uint32_t keepFiles) {
    if (platform::trim(directory).empty()) {
        return Error::text(L"Logger::open: empty directory");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // A second open() (e.g. after the log settings changed) closes the old handle first.
    if (file_ != nullptr) {
        flushLocked();
        ::CloseHandle(file_);
        file_ = nullptr;
    }

    // Make sure the folder exists before touching any file inside it.
    const std::wstring dir = stripTrailingSeparators(directory);
    if (auto made = platform::createDirectories(dir); !made) {
        return made.error();
    }

    // Record the configuration (clamped to sane bounds).
    directory_ = dir;
    path_ = rotatedName(dir, 0);
    level_ = level;
    maxSizeKb_ = std::clamp(maxSizeKb, kMinSizeKb, kMaxSizeKb);
    keepFiles_ = std::min(keepFiles, kMaxKeepFiles);
    bytesWritten_ = 0;

    // Rotate first when the previous session left an oversized file behind.
    const uint64_t existing = platform::fileSize(path_).valueOr(0);
    if (existing > static_cast<uint64_t>(maxSizeKb_) * 1024ull) {
        rotateLocked();  // opens the fresh file itself
    } else {
        file_ = openAppend(path_);
        bytesWritten_ = existing;
    }

    // rotateLocked() reports problems to the debugger only; surface them here.
    if (file_ == nullptr) {
        Error err = Error::fromLastError(L"Logger: CreateFileW(" + path_ + L")");
        directory_.clear();
        path_.clear();
        return err;
    }

    lastFlushMs_ = platform::nowMonotonicMs();
    return {};
}

/**
 * @brief Flushes and closes the file. Later writes go to the debugger only.
 */
void Logger::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr) {
        flushLocked();
        ::CloseHandle(file_);
        file_ = nullptr;
    }
    directory_.clear();
    path_.clear();
    bytesWritten_ = 0;
}

// ---------------------------------------------------------------------------
// Level handling
// ---------------------------------------------------------------------------

void Logger::setLevel(LogLevel level) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::level() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

bool Logger::enabled(LogLevel level) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(level) >= static_cast<int>(level_);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/**
 * @brief Formats and appends one line.
 *
 * The line is composed outside the lock (timestamp formatting is the costly
 * part); the lock covers only the WriteFile, the flush decision and rotation.
 */
void Logger::write(LogLevel level, std::wstring_view component, std::wstring_view message) {
    if (!enabled(level)) {
        return;
    }

    // "YYYY-MM-DD HH:MM:SS.mmm +HH:MM [LEVEL] [T<tid>] [Component] message"
    std::wstring line;
    line.reserve(80 + component.size() + message.size());
    line += platform::formatLocalIso(platform::nowUtc(), true);
    line += L" [";
    line += levelTag(level);
    line += L"] [T";
    line += std::to_wstring(::GetCurrentThreadId());
    line += L"] [";
    line.append(component.empty() ? std::wstring_view(L"App") : component);
    line += L"] ";
    line.append(message);
    line += L"\r\n";

    const bool important = static_cast<int>(level) >= static_cast<int>(LogLevel::Warn);
    const std::string utf8 = platform::toUtf8(line);

    std::lock_guard<std::mutex> lock(mutex_);

    // Not open (yet / any more): the debugger is the only sink we have.
    if (file_ == nullptr) {
        ::OutputDebugStringW(line.c_str());
        return;
    }

    // Warnings and errors are mirrored to the debugger so they show up live.
    if (important) {
        ::OutputDebugStringW(line.c_str());
    }

    // Append the UTF-8 bytes. A failure has nowhere to go but the debugger.
    DWORD written = 0;
    const DWORD toWrite = static_cast<DWORD>(std::min<size_t>(utf8.size(), 0x7FFFFFFFu));
    if (!::WriteFile(file_, utf8.data(), toWrite, &written, nullptr)) {
        ::OutputDebugStringW(L"[Logger] WriteFile failed for the log file\r\n");
        return;
    }
    bytesWritten_ += written;

    // Force to disk for Warn+ and periodically for everything else.
    const uint64_t now = platform::nowMonotonicMs();
    if (important || now < lastFlushMs_ || (now - lastFlushMs_) >= kFlushIntervalMs) {
        flushLocked();
    }

    // Roll over once the cap is exceeded.
    if (bytesWritten_ > static_cast<uint64_t>(maxSizeKb_) * 1024ull) {
        rotateLocked();
    }
}

/**
 * @brief Directory the log lives in (empty when closed).
 */
std::wstring Logger::directory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return directory_;
}

/**
 * @brief Full path of the live log file (empty when closed).
 */
std::wstring Logger::filePath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

/**
 * @brief "trace|debug|info|warn|error" -> level, case-insensitive; @p fallback otherwise.
 *        "warning" and "err" are accepted as well since they show up in hand-edited files.
 */
LogLevel Logger::parseLevel(std::wstring_view text, LogLevel fallback) {
    const std::wstring_view t = platform::trim(text);
    if (t.empty()) {
        return fallback;
    }
    if (platform::iequals(t, L"trace")) return LogLevel::Trace;
    if (platform::iequals(t, L"debug")) return LogLevel::Debug;
    if (platform::iequals(t, L"info"))  return LogLevel::Info;
    if (platform::iequals(t, L"warn") || platform::iequals(t, L"warning")) return LogLevel::Warn;
    if (platform::iequals(t, L"error") || platform::iequals(t, L"err")) return LogLevel::Error;
    return fallback;
}

/**
 * @brief Fixed-width (5 chars) tag for the line prefix.
 */
const wchar_t* Logger::levelTag(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return L"TRACE";
    case LogLevel::Debug: return L"DEBUG";
    case LogLevel::Info:  return L"INFO ";
    case LogLevel::Warn:  return L"WARN ";
    case LogLevel::Error: return L"ERROR";
    }
    return L"?????";
}

// ---------------------------------------------------------------------------
// Private helpers (caller holds mutex_)
// ---------------------------------------------------------------------------

/**
 * @brief Shifts the rotated files down one slot, renames the live file to
 *        ".1" and opens a fresh live file. Failures are reported to the
 *        debugger; the logger keeps working on whatever file it can open.
 */
void Logger::rotateLocked() {
    if (directory_.empty() || path_.empty()) {
        return;
    }

    // Close the live handle so the rename can proceed.
    if (file_ != nullptr) {
        flushLocked();
        ::CloseHandle(file_);
        file_ = nullptr;
    }

    bool renamed = false;
    if (keepFiles_ == 0) {
        // Nothing is kept: simply discard the current file.
        renamed = platform::deleteFile(path_).ok();
    } else {
        // Drop the oldest slot, then move n-1 -> n for the rest.
        (void)platform::deleteFile(rotatedName(directory_, keepFiles_));
        for (uint32_t i = keepFiles_; i >= 2; --i) {
            const std::wstring from = rotatedName(directory_, i - 1);
            const std::wstring to = rotatedName(directory_, i);
            if (platform::exists(from)) {
                if (auto mv = platform::moveReplace(from, to); !mv) {
                    ::OutputDebugStringW((L"[Logger] rotate: " + mv.error().toString() + L"\r\n").c_str());
                }
            }
        }
        // Finally the live file becomes ".1".
        if (auto mv = platform::moveReplace(path_, rotatedName(directory_, 1)); mv) {
            renamed = true;
        } else {
            ::OutputDebugStringW((L"[Logger] rotate: " + mv.error().toString() + L"\r\n").c_str());
        }
    }

    // Re-open (a brand-new file when the rename worked, the old one otherwise).
    file_ = openAppend(path_);
    if (file_ == nullptr) {
        ::OutputDebugStringW(L"[Logger] rotate: could not re-open the log file\r\n");
        bytesWritten_ = 0;
        return;
    }

    // When the rename failed we keep appending to the oversized file. Reset
    // the counter anyway so the next attempt waits for another cap's worth
    // instead of retrying on every single line.
    bytesWritten_ = renamed ? sizeOfHandle(file_) : 0;
    lastFlushMs_ = platform::nowMonotonicMs();
}

/**
 * @brief FlushFileBuffers on the live handle and remembers when.
 */
void Logger::flushLocked() {
    if (file_ != nullptr) {
        ::FlushFileBuffers(file_);
    }
    lastFlushMs_ = platform::nowMonotonicMs();
}

} // namespace hh
