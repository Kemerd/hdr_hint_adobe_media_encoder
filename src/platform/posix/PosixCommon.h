// ---------------------------------------------------------------------------
// posix/PosixCommon.h - helpers shared by the POSIX platform sources only.
//
// Not part of the public platform API: engine code never includes this.
// It centralises the three conversions every POSIX source needs - wide
// path <-> UTF-8 bytes, errno -> hh::Error, and struct timespec ->
// FILETIME-style ticks - so each one is written (and reviewed) once.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Utf.h"
#include "platform/Win.h"

#include <cerrno>
#include <cstdint>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace hh::platform::posix {

/// 1970-01-01 as FILETIME ticks (100 ns since 1601-01-01).
inline constexpr uint64_t kUnixEpochTicks = 116'444'736'000'000'000ull;

/**
 * @brief Wide path -> the UTF-8 bytes the kernel expects.
 *
 * Embedded NULs would silently truncate the path at the syscall boundary,
 * so they are rejected here by returning an empty string (every caller
 * treats an empty native path as "invalid").
 */
inline std::string toNative(std::wstring_view path) {
    if (path.find(L'\0') != std::wstring_view::npos) {
        return {};
    }
    return toUtf8(path);
}

/**
 * @brief UTF-8 bytes from the kernel -> wide text, precomposed (NFC).
 */
inline std::wstring fromNative(std::string_view bytes) {
    return normalizeNfc(toWide(bytes));
}

/**
 * @brief timespec -> FILETIME-style ticks (0 for anything before 1601).
 */
inline uint64_t ticksFromTimespec(const timespec& ts) noexcept {
    const int64_t ticks = static_cast<int64_t>(ts.tv_sec) * 10'000'000ll + static_cast<int64_t>(ts.tv_nsec / 100)
                        + static_cast<int64_t>(kUnixEpochTicks);
    return ticks < 0 ? 0 : static_cast<uint64_t>(ticks);
}

/// Last-write time of a stat record.
inline uint64_t lastWriteTicks(const struct stat& st) noexcept {
#if defined(__APPLE__)
    return ticksFromTimespec(st.st_mtimespec);
#else
    return ticksFromTimespec(st.st_mtim);
#endif
}

/// Creation ("birth") time of a stat record; falls back to the change time.
inline uint64_t creationTicks(const struct stat& st) noexcept {
#if defined(__APPLE__)
    return ticksFromTimespec(st.st_birthtimespec);
#else
    return ticksFromTimespec(st.st_ctim);
#endif
}

/**
 * @brief Error from the current errno with a "context path" prefix.
 */
inline Error errnoError(std::wstring_view context, int err = errno) {
    return Error::fromErrno(err, context);
}

/**
 * @brief close() that survives EINTR without retrying (POSIX leaves the
 *        descriptor state unspecified after EINTR; retrying could close a
 *        descriptor another thread just received).
 */
inline void closeQuietly(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace hh::platform::posix
