// ---------------------------------------------------------------------------
// Win.h - the one place that pulls in the operating system's base headers.
//
// Every platform/core header includes this instead of <windows.h> directly so
// the lean-and-mean / NOMINMAX / STRICT defines are guaranteed to be set the
// same way in every translation unit.
//
// On macOS (and any other POSIX host) there is no <windows.h>. The engine
// still speaks a small, deliberate subset of the Win32 vocabulary there:
//
//   * scalar typedefs (DWORD, UINT, HRESULT, ...) so structs such as
//     SidecarEvent keep one layout and one spelling on every platform;
//   * Win32 error *numbers* (ERROR_FILE_NOT_FOUND, ...). The POSIX platform
//     layer maps errno onto these, so checks like
//         if (err.win32 == ERROR_SHARING_VIOLATION) ...
//     mean exactly the same thing everywhere;
//   * the FILE_ACTION_* / FILE_NOTIFY_* constants DirectoryWatch reports.
//
// Nothing here emulates Win32 *behaviour* - no fake HANDLEs, no fake
// WaitForMultipleObjects. Behaviour lives behind real abstractions in the
// platform layer (Event.h, FileIo.h, Process.h, ...).
// ---------------------------------------------------------------------------
#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef STRICT
#define STRICT
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#else  // ---- POSIX (macOS) -------------------------------------------------------

#include <cstddef>
#include <cstdint>

// ===========================================================================
// Scalar vocabulary
//
// Sized to match the Win32 definitions so persisted numbers (job exit codes,
// sidecar pids) round-trip identically between the two builds.
// ===========================================================================
//
// BOOL is deliberately absent: Objective-C owns that name on Apple platforms.
// HRESULT is the same 32-bit signed type CoreFoundation's CFPlugInCOM.h uses,
// so the two typedefs agree when both headers land in one .mm file.
using DWORD = std::uint32_t;     ///< 32-bit unsigned, as on Windows
using UINT = unsigned int;       ///< native unsigned int
using LONG = std::int32_t;       ///< Win32 LONG is always 32 bits
using WORD = std::uint16_t;      ///< 16-bit unsigned
using HRESULT = std::int32_t;    ///< COM-style status (only S_OK / E_FAIL are produced)
using COLORREF = std::uint32_t;  ///< 0x00BBGGRR, same packing as Win32

// S_OK / E_FAIL / SUCCEEDED are intentionally *not* defined here: CoreFoundation's
// CFPlugInCOM.h owns those macro names on macOS, and no portable code needs
// them (an Error's hr field is simply 0 when not applicable).

/// "Wait forever" for every timeout parameter in the platform layer.
inline constexpr DWORD INFINITE = 0xFFFFFFFFu;

// ===========================================================================
// Win32 error numbers
//
// The POSIX layer translates errno into these (see platform::win32FromErrno),
// so engine code that classifies failures does not need two spellings.
// ===========================================================================
inline constexpr DWORD ERROR_SUCCESS = 0;
inline constexpr DWORD ERROR_INVALID_FUNCTION = 1;
inline constexpr DWORD ERROR_FILE_NOT_FOUND = 2;
inline constexpr DWORD ERROR_PATH_NOT_FOUND = 3;
inline constexpr DWORD ERROR_TOO_MANY_OPEN_FILES = 4;
inline constexpr DWORD ERROR_ACCESS_DENIED = 5;
inline constexpr DWORD ERROR_INVALID_HANDLE = 6;
inline constexpr DWORD ERROR_NOT_ENOUGH_MEMORY = 8;
inline constexpr DWORD ERROR_NOT_SAME_DEVICE = 17;
inline constexpr DWORD ERROR_WRITE_PROTECT = 19;
inline constexpr DWORD ERROR_NOT_READY = 21;
inline constexpr DWORD ERROR_SHARING_VIOLATION = 32;
inline constexpr DWORD ERROR_LOCK_VIOLATION = 33;
inline constexpr DWORD ERROR_HANDLE_EOF = 38;
inline constexpr DWORD ERROR_NOT_SUPPORTED = 50;
inline constexpr DWORD ERROR_BAD_NETPATH = 53;
inline constexpr DWORD ERROR_FILE_EXISTS = 80;
inline constexpr DWORD ERROR_INVALID_PARAMETER = 87;
inline constexpr DWORD ERROR_BROKEN_PIPE = 109;
inline constexpr DWORD ERROR_DISK_FULL = 112;
inline constexpr DWORD ERROR_INSUFFICIENT_BUFFER = 122;
inline constexpr DWORD ERROR_INVALID_NAME = 123;
inline constexpr DWORD ERROR_DIR_NOT_EMPTY = 145;
inline constexpr DWORD ERROR_BUSY = 170;
inline constexpr DWORD ERROR_ALREADY_EXISTS = 183;
inline constexpr DWORD ERROR_FILENAME_EXCED_RANGE = 206;
inline constexpr DWORD ERROR_NO_DATA = 232;
inline constexpr DWORD ERROR_DIRECTORY = 267;
inline constexpr DWORD ERROR_OPERATION_ABORTED = 995;
inline constexpr DWORD ERROR_IO_PENDING = 997;
inline constexpr DWORD ERROR_CANCELLED = 1223;
inline constexpr DWORD ERROR_TIMEOUT = 1460;

// ===========================================================================
// Directory-change vocabulary (platform::DirectoryWatch reports these)
// ===========================================================================
inline constexpr DWORD FILE_ACTION_ADDED = 0x1;
inline constexpr DWORD FILE_ACTION_REMOVED = 0x2;
inline constexpr DWORD FILE_ACTION_MODIFIED = 0x3;
inline constexpr DWORD FILE_ACTION_RENAMED_OLD_NAME = 0x4;
inline constexpr DWORD FILE_ACTION_RENAMED_NEW_NAME = 0x5;

inline constexpr DWORD FILE_NOTIFY_CHANGE_FILE_NAME = 0x001;
inline constexpr DWORD FILE_NOTIFY_CHANGE_DIR_NAME = 0x002;
inline constexpr DWORD FILE_NOTIFY_CHANGE_ATTRIBUTES = 0x004;
inline constexpr DWORD FILE_NOTIFY_CHANGE_SIZE = 0x008;
inline constexpr DWORD FILE_NOTIFY_CHANGE_LAST_WRITE = 0x010;
inline constexpr DWORD FILE_NOTIFY_CHANGE_CREATION = 0x040;

/// File attribute bits reported in platform::DirEntry::attributes.
inline constexpr DWORD FILE_ATTRIBUTE_READONLY = 0x001;
inline constexpr DWORD FILE_ATTRIBUTE_HIDDEN = 0x002;
inline constexpr DWORD FILE_ATTRIBUTE_SYSTEM = 0x004;
inline constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 0x010;
inline constexpr DWORD FILE_ATTRIBUTE_NORMAL = 0x080;
inline constexpr DWORD FILE_ATTRIBUTE_TEMPORARY = 0x100;
inline constexpr DWORD FILE_ATTRIBUTE_REPARSE_POINT = 0x400;
inline constexpr DWORD FILE_ATTRIBUTE_OFFLINE = 0x1000;

// ===========================================================================
// Code pages accepted by platform::fromCodePage / toCodePage
// ===========================================================================
inline constexpr UINT CP_ACP = 0;        ///< "the system ANSI code page" -> Windows-1252 on macOS
inline constexpr UINT CP_UTF8 = 65001;

// ===========================================================================
// Geometry records shared by a few engine/UI signatures
// ===========================================================================
struct POINT {
    LONG x = 0;
    LONG y = 0;
};
struct SIZE {
    LONG cx = 0;
    LONG cy = 0;
};
struct RECT {
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
};

/// COLORREF helpers with the Win32 byte order (0x00BBGGRR).
constexpr COLORREF RGB(unsigned r, unsigned g, unsigned b) noexcept {
    return static_cast<COLORREF>((r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16));
}
constexpr unsigned GetRValue(COLORREF c) noexcept { return c & 0xFFu; }
constexpr unsigned GetGValue(COLORREF c) noexcept { return (c >> 8) & 0xFFu; }
constexpr unsigned GetBValue(COLORREF c) noexcept { return (c >> 16) & 0xFFu; }

#endif  // _WIN32

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
