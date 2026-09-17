// ---------------------------------------------------------------------------
// Utf.h - UTF-8 <-> UTF-16 conversion and small wide-string helpers.
//
// Convention for the whole project: anything that touches a Win32 API is a
// std::wstring; anything serialised (JSON, INI, IPC, log file) is UTF-8.
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
/// Decodes text in a legacy code page (e.g. the user's ANSI page) to UTF-16.
std::wstring fromCodePage(std::string_view bytes, UINT codePage);

/// Upper-cases using the file-system style (CharUpperW), for path keys.
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
