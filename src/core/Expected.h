// ---------------------------------------------------------------------------
// Expected.h - error-as-value types used across the engine.
//
// MSVC's std::expected needs /std:c++latest, so this is a small stand-in with
// the same spirit: a Result<T> either holds a T or an Error, never throws
// across thread boundaries, and reads naturally at call sites:
//
//     auto r = readAll(path);
//     if (!r) { log(r.error().toString()); return; }
//     use(r.value());
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <optional>
#include <string>
#include <utility>

namespace hh {

/**
 * @brief A failure description: Win32 code and/or HRESULT plus a human message.
 */
struct Error {
    DWORD win32 = 0;              ///< GetLastError() style code (0 when not applicable; errno is mapped onto it on POSIX)
    HRESULT hr = 0;               ///< HRESULT (0 == S_OK when not applicable)
    std::wstring message;         ///< context + system text, ready for logs/UI

    /// Builds an Error from GetLastError() with a context prefix.
    static Error fromLastError(std::wstring_view context);
    /// Builds an Error from an explicit Win32 code with a context prefix.
    static Error fromWin32(DWORD code, std::wstring_view context);
    /// Builds an Error from an HRESULT with a context prefix.
    static Error fromHr(HRESULT hr, std::wstring_view context);
    /// Builds an Error that carries only a message.
    static Error text(std::wstring message);
#if !defined(_WIN32)
    /// Builds an Error from an errno value: win32 holds the mapped Win32
    /// number, the message holds strerror() so nothing is lost in translation.
    static Error fromErrno(int err, std::wstring_view context);
#endif

    /// Formats "context: system message (0x...)" for display.
    [[nodiscard]] std::wstring toString() const;
};

/// Looks up the system message for a Win32 error code (trimmed, single line).
std::wstring win32ErrorText(DWORD code);
/// Looks up the system message for an HRESULT (trimmed, single line).
std::wstring hresultText(HRESULT hr);

#if !defined(_WIN32)
/// Maps an errno value onto the closest Win32 error number (0 stays 0).
DWORD win32FromErrno(int err) noexcept;
#endif

/**
 * @brief Holds either a value of type T or an Error.
 */
template <class T>
class Result {
public:
    /// Success.
    Result(T value) : value_(std::move(value)) {}                     // NOLINT(google-explicit-constructor)
    /// Failure.
    Result(Error error) : error_(std::move(error)), failed_(true) {}   // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return *value_; }
    [[nodiscard]] const T& value() const& { return *value_; }
    [[nodiscard]] T&& value() && { return std::move(*value_); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }

    /// Value or a fallback when failed.
    [[nodiscard]] T valueOr(T fallback) const& { return failed_ ? std::move(fallback) : *value_; }

private:
    std::optional<T> value_;
    Error error_;
    bool failed_ = false;
};

/**
 * @brief Result specialisation for operations without a value.
 */
template <>
class Result<void> {
public:
    Result() = default;                                                 // success
    Result(Error error) : error_(std::move(error)), failed_(true) {}    // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }

    /// Convenience success value.
    static Result success() { return Result(); }

private:
    Error error_;
    bool failed_ = false;
};

} // namespace hh
