// ---------------------------------------------------------------------------
// Utf.h - UTF-8 <-> UTF-16 conversion and small wide-string helpers.
//
// Convention for the whole project: anything that touches an OS API is a
// std::wstring; anything serialised (JSON, INI, IPC, log file) is UTF-8.
//
// wchar_t is UTF-16 on Windows and UTF-32 on macOS. Every helper here is
// written against code points, not code units, so callers never need to
// care; the only place the width shows is fromUtf16(), which decodes raw
// UTF-16 (AME's log files) into whatever wchar_t is on this platform.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <string>
#include <string_view>
#include <vector>

namespace hh::platform {

/// UTF-8 -> UTF-16. Invalid sequences are replaced with U+FFFD, never thrown.
std::wstring toWide(std::string_view utf8);
/// UTF-16 -> UTF-8. Unpaired surrogates are replaced with U+FFFD, never thrown.
std::string toUtf8(std::wstring_view wide);
/// Decodes text in a legacy code page (e.g. the user's ANSI page) to wide text.
std::wstring fromCodePage(std::string_view bytes, UINT codePage);

/**
 * @brief Decodes UTF-16 code units (host byte order) into a wide string.
 *
 * Windows: a straight copy. macOS: surrogate pairs are combined into one
 * UTF-32 code point and unpaired surrogates become U+FFFD.
 */
std::wstring fromUtf16(const char16_t* units, size_t count);

/**
 * @brief Unicode canonical composition (NFC) of a path or file name.
 *
 * macOS file systems hand back decomposed names (NFD) for text that AME's
 * log, the user and the rest of the engine spell precomposed. Keys built
 * from both must agree, so names coming from the file system pass through
 * here. Windows keeps names exactly as typed, so this is the identity there.
 */
std::wstring normalizeNfc(std::wstring_view s);

/// Upper-cases 1:1 per code point (CharUpperW on Windows), for path keys.
std::wstring toUpperInvariant(std::wstring_view s);
/// Lower-cases using CharLowerW.
std::wstring toLowerInvariant(std::wstring_view s);

/// Ordinal case-insensitive comparison (CompareStringOrdinal ignoreCase).
bool iequals(std::wstring_view a, std::wstring_view b);
/// Case-insensitive prefix / suffix / contains checks (ordinal).
bool istartsWith(std::wstring_view s, std::wstring_view prefix);
bool iendsWith(std::wstring_view s, std::wstring_view suffix);
bool icontains(std::wstring_view haystack, std::wstring_view needle);
/// Case-insensitive find; returns std::wstring::npos when absent.
size_t ifind(std::wstring_view haystack, std::wstring_view needle);

/// Trims ASCII whitespace (space, tab, CR, LF, VT, FF) from both ends.
std::wstring_view trim(std::wstring_view s);
std::string_view trim(std::string_view s);

/// Splits on a single separator; empty pieces are kept unless @p skipEmpty.
std::vector<std::wstring> split(std::wstring_view s, wchar_t sep, bool skipEmpty = true);
/// Joins with a separator.
std::wstring join(const std::vector<std::wstring>& parts, std::wstring_view sep);

/// Replaces every occurrence of @p from with @p to.
std::wstring replaceAll(std::wstring_view s, std::wstring_view from, std::wstring_view to);

/// Parses a decimal integer; returns std::nullopt on any junk.
std::optional<long long> parseInt(std::wstring_view s);
/// Parses a decimal floating point number (invariant culture, '.' separator).
std::optional<double> parseDouble(std::wstring_view s);

/// Parses "true/false/1/0/yes/no/on/off" (case-insensitive, also Python "True").
std::optional<bool> parseBool(std::wstring_view s);

} // namespace hh::platform
