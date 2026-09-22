// ---------------------------------------------------------------------------
// Expected.cpp - Error construction and system message lookup.
//
// Every failure in the engine ends up as an hh::Error, so this file is the
// one place that talks to FormatMessageW. Messages are normalised to a single
// trimmed line ("The system cannot find the file specified") so they can be
// dropped straight into a log line or a UI label.
//
// On POSIX there is no system message table for Win32 numbers. errno values
// are mapped onto the closest Win32 number (so classification code keeps one
// spelling) while the human text comes straight from strerror().
// ---------------------------------------------------------------------------
#include "core/Expected.h"

#if !defined(_WIN32)
#include "platform/Utf.h"

#include <cerrno>
#include <cstring>
#endif

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace hh {

namespace {

/**
 * @brief Collapses raw FormatMessage output into one trimmed line.
 *
 * FormatMessage likes to append "\r\n", sometimes embeds hard line breaks
 * and always ends with a period. All of that is folded: whitespace runs
 * become one space, and trailing spaces / periods are dropped.
 *
 * @param raw  Text exactly as FormatMessageW produced it.
 * @return     Single-line, trimmed text (may be empty).
 */
std::wstring tidySystemText(std::wstring_view raw)
{
    std::wstring out;
    out.reserve(raw.size());

    // Collapse every whitespace run (including CR/LF/TAB) into one space,
    // and never emit a leading space.
    bool pendingSpace = false;
    for (const wchar_t ch : raw) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == L' ') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(L' ');
            pendingSpace = false;
        }
        out.push_back(ch);
    }

    // Drop the trailing period(s) and any whitespace left in front of them.
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) {
        out.pop_back();
    }
    return out;
}

#if defined(_WIN32)
/**
 * @brief Runs FormatMessageW against the system message table.
 *
 * Tries the caller's default language first and falls back to US English
 * when the neutral lookup has no resource (common on trimmed installs).
 *
 * @param messageId  Win32 error code or HRESULT bit pattern.
 * @return           Tidied message, or empty when the id is unknown.
 */
std::wstring formatSystemMessage(DWORD messageId)
{
    constexpr DWORD kFlags = FORMAT_MESSAGE_ALLOCATE_BUFFER
                           | FORMAT_MESSAGE_FROM_SYSTEM
                           | FORMAT_MESSAGE_IGNORE_INSERTS
                           | FORMAT_MESSAGE_MAX_WIDTH_MASK;

    // FormatMessage allocates the buffer for us with LocalAlloc; the pointer
    // is written through the "buffer" argument, hence the ugly cast.
    LPWSTR buffer = nullptr;
    DWORD length = ::FormatMessageW(kFlags, nullptr, messageId, 0,
                                    reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    // Second attempt with an explicit US English language id.
    if (length == 0 || buffer == nullptr) {
        if (buffer != nullptr) {
            ::LocalFree(buffer);
            buffer = nullptr;
        }
        length = ::FormatMessageW(kFlags, nullptr, messageId,
                                  MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                  reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    }

    // Still nothing: the id is simply not a system message.
    if (length == 0 || buffer == nullptr) {
        if (buffer != nullptr) {
            ::LocalFree(buffer);
        }
        return {};
    }

    std::wstring text = tidySystemText(std::wstring_view(buffer, static_cast<size_t>(length)));
    ::LocalFree(buffer);
    return text;
}
#else
/**
 * @brief The errno value that best explains a (mapped) Win32 number.
 *
 * Inverse of win32FromErrno for the codes the POSIX layer produces, so
 * win32ErrorText() can still hand back strerror() wording.
 */
int errnoForWin32(DWORD code) noexcept
{
    switch (code) {
    case ERROR_FILE_NOT_FOUND:       return ENOENT;
    case ERROR_PATH_NOT_FOUND:       return ENOENT;
    case ERROR_TOO_MANY_OPEN_FILES:  return EMFILE;
    case ERROR_ACCESS_DENIED:        return EACCES;
    case ERROR_INVALID_HANDLE:       return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:    return ENOMEM;
    case ERROR_NOT_SAME_DEVICE:      return EXDEV;
    case ERROR_WRITE_PROTECT:        return EROFS;
    case ERROR_NOT_READY:            return EIO;
    case ERROR_SHARING_VIOLATION:    return EBUSY;
    case ERROR_LOCK_VIOLATION:       return EWOULDBLOCK;
    case ERROR_NOT_SUPPORTED:        return ENOTSUP;
    case ERROR_BAD_NETPATH:          return EHOSTUNREACH;
    case ERROR_FILE_EXISTS:          return EEXIST;
    case ERROR_ALREADY_EXISTS:       return EEXIST;
    case ERROR_INVALID_PARAMETER:    return EINVAL;
    case ERROR_BROKEN_PIPE:          return EPIPE;
    case ERROR_DISK_FULL:            return ENOSPC;
    case ERROR_INVALID_NAME:         return EINVAL;
    case ERROR_DIR_NOT_EMPTY:        return ENOTEMPTY;
    case ERROR_BUSY:                 return EBUSY;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_DIRECTORY:            return ENOTDIR;
    case ERROR_OPERATION_ABORTED:    return ECANCELED;
    case ERROR_CANCELLED:            return ECANCELED;
    case ERROR_TIMEOUT:              return ETIMEDOUT;
    default:                         return 0;
    }
}

/**
 * @brief strerror() for one errno value, as a tidied wide string.
 */
std::wstring errnoText(int err)
{
    if (err == 0) {
        return {};
    }
    // strerror_r (the XSI flavour on macOS) never touches shared state.
    char buffer[256] = {};
    if (::strerror_r(err, buffer, sizeof(buffer)) != 0 || buffer[0] == '\0') {
        return {};
    }
    return tidySystemText(platform::toWide(buffer));
}
#endif

/**
 * @brief Joins a context prefix and a system message as "context: message".
 *
 * Either side may be empty; the separator is only inserted when both exist.
 */
std::wstring joinContext(std::wstring_view context, std::wstring_view systemText)
{
    if (context.empty()) {
        return std::wstring(systemText);
    }
    if (systemText.empty()) {
        return std::wstring(context);
    }
    std::wstring out;
    out.reserve(context.size() + 2 + systemText.size());
    out.append(context);
    out.append(L": ");
    out.append(systemText);
    return out;
}

/**
 * @brief Formats a 32-bit code as "0x0000002A".
 */
std::wstring hexCode(uint32_t code)
{
    return std::format(L"0x{:08X}", code);
}

} // namespace

// ---------------------------------------------------------------------------
// Free lookups
// ---------------------------------------------------------------------------

/**
 * @brief Looks up the system message for a Win32 error code.
 *
 * Unknown codes produce "Unknown error 0x...." so callers always get
 * something readable.
 */
std::wstring win32ErrorText(DWORD code)
{
#if defined(_WIN32)
    std::wstring text = formatSystemMessage(code);
#else
    // POSIX: explain the code with the errno it was mapped from.
    std::wstring text = (code == ERROR_SUCCESS) ? std::wstring(L"The operation completed successfully")
                                                : errnoText(errnoForWin32(code));
#endif
    if (text.empty()) {
        text = L"Unknown error " + hexCode(static_cast<uint32_t>(code));
    }
    return text;
}

/**
 * @brief Looks up the system message for an HRESULT.
 *
 * FACILITY_WIN32 HRESULTs are unwrapped to their Win32 code so the text is
 * identical to what GetLastError() would have produced. Other facilities
 * (COM, DXGI, ...) go straight to the system table, which knows the common
 * E_* values.
 */
std::wstring hresultText(HRESULT hr)
{
#if !defined(_WIN32)
    // No COM on POSIX: only the success value has a name.
    if (hr == 0) {
        return L"The operation completed successfully";
    }
    return L"Unknown HRESULT " + hexCode(static_cast<uint32_t>(hr));
#else
    // Wrapped Win32 code: reuse the Win32 path for consistent wording.
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return win32ErrorText(static_cast<DWORD>(HRESULT_CODE(hr)));
    }

    // S_OK and friends still deserve a sensible label.
    if (hr == S_OK) {
        return L"The operation completed successfully";
    }

    std::wstring text = formatSystemMessage(static_cast<DWORD>(hr));
    if (text.empty()) {
        text = L"Unknown HRESULT " + hexCode(static_cast<uint32_t>(hr));
    }
    return text;
#endif
}

#if !defined(_WIN32)
/**
 * @brief errno -> the Win32 number the engine's classification code expects.
 *
 * Only the distinctions the engine actually makes are preserved ("gone",
 * "someone else has it", "not allowed", ...). Anything unknown becomes
 * ERROR_INVALID_FUNCTION, which is non-zero (so it still reads as a failure)
 * and never matches a benign-error test by accident.
 */
DWORD win32FromErrno(int err) noexcept
{
    switch (err) {
    case 0:            return ERROR_SUCCESS;
    case ENOENT:       return ERROR_FILE_NOT_FOUND;
    case ENOTDIR:      return ERROR_PATH_NOT_FOUND;
    case EMFILE:
    case ENFILE:       return ERROR_TOO_MANY_OPEN_FILES;
    case EACCES:
    case EPERM:        return ERROR_ACCESS_DENIED;
    case EBADF:        return ERROR_INVALID_HANDLE;
    case ENOMEM:       return ERROR_NOT_ENOUGH_MEMORY;
    case EXDEV:        return ERROR_NOT_SAME_DEVICE;
    case EROFS:        return ERROR_WRITE_PROTECT;
    case EIO:          return ERROR_NOT_READY;
    case EBUSY:
    case ETXTBSY:      return ERROR_SHARING_VIOLATION;
    case EAGAIN:       return ERROR_LOCK_VIOLATION;
    case ENOTSUP:      return ERROR_NOT_SUPPORTED;
    case EHOSTUNREACH:
    case ENETUNREACH:
    case EHOSTDOWN:    return ERROR_BAD_NETPATH;
    case EEXIST:       return ERROR_ALREADY_EXISTS;
    case EINVAL:       return ERROR_INVALID_PARAMETER;
    case EPIPE:        return ERROR_BROKEN_PIPE;
    case ENOSPC:
    case EDQUOT:       return ERROR_DISK_FULL;
    case ENOTEMPTY:    return ERROR_DIR_NOT_EMPTY;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case EISDIR:       return ERROR_DIRECTORY;
    case ECANCELED:    return ERROR_OPERATION_ABORTED;
    case ETIMEDOUT:    return ERROR_TIMEOUT;
    default:           return ERROR_INVALID_FUNCTION;
    }
}
#endif

// ---------------------------------------------------------------------------
// Error factories
// ---------------------------------------------------------------------------

/**
 * @brief Captures GetLastError() immediately, before anything else can
 *        clobber it, then delegates to fromWin32.
 */
Error Error::fromLastError(std::wstring_view context)
{
#if defined(_WIN32)
    const DWORD code = ::GetLastError();
    return fromWin32(code, context);
#else
    // errno is the POSIX "last error"; keep the precise strerror() wording.
    return fromErrno(errno, context);
#endif
}

/**
 * @brief Builds an Error from an explicit Win32 code.
 *
 * The HRESULT mirror is filled in as well so callers that propagate
 * HRESULTs (COM, DirectX) do not have to convert themselves.
 */
Error Error::fromWin32(DWORD code, std::wstring_view context)
{
    Error e;
    e.win32 = code;
#if defined(_WIN32)
    e.hr = HRESULT_FROM_WIN32(code);
#endif
    e.message = joinContext(context, win32ErrorText(code));
    return e;
}

/**
 * @brief Builds an Error from an HRESULT.
 *
 * When the HRESULT wraps a Win32 code the win32 field is populated too, so
 * `error.win32 == ERROR_FILE_NOT_FOUND` style checks keep working.
 */
Error Error::fromHr(HRESULT hr, std::wstring_view context)
{
    Error e;
    e.hr = hr;
#if defined(_WIN32)
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        e.win32 = static_cast<DWORD>(HRESULT_CODE(hr));
    }
#endif
    e.message = joinContext(context, hresultText(hr));
    return e;
}

#if !defined(_WIN32)
/**
 * @brief Builds an Error from errno: mapped Win32 number, strerror() text.
 */
Error Error::fromErrno(int err, std::wstring_view context)
{
    Error e;
    e.win32 = win32FromErrno(err);
    std::wstring text = errnoText(err);
    if (text.empty()) {
        text = win32ErrorText(e.win32);
    }
    e.message = joinContext(context, text);
    return e;
}
#endif

/**
 * @brief Builds an Error that carries only a message (no system code).
 */
Error Error::text(std::wstring message)
{
    Error e;
    e.message = std::move(message);
    return e;
}

/**
 * @brief Renders "context: system message (0x00000002)".
 *
 * The hex suffix shows the Win32 code when there is one, otherwise the
 * HRESULT; text-only errors are returned verbatim. When the message already
 * spells out the code (unknown-code fallback) it is not repeated.
 */
std::wstring Error::toString() const
{
    // Pick the most specific code we have.
    std::wstring code;
    if (win32 != 0) {
        code = hexCode(static_cast<uint32_t>(win32));
    } else if (hr != 0) {
        code = hexCode(static_cast<uint32_t>(hr));
    }

    // Nothing to append, or the message already carries the code.
    if (code.empty() || message.find(code) != std::wstring::npos) {
        return message;
    }
    if (message.empty()) {
        return code;
    }
    return std::format(L"{} ({})", message, code);
}

} // namespace hh
