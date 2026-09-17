// ---------------------------------------------------------------------------
// Utf.cpp - UTF-8 <-> UTF-16 conversion and small wide-string helpers.
//
// Conversions go through MultiByteToWideChar / WideCharToMultiByte. A strict
// pass (MB_ERR_INVALID_CHARS / WC_ERR_INVALID_CHARS) is tried first; when the
// input contains garbage the lenient pass replaces it with U+FFFD, and a
// hand-rolled byte mapper is the last line of defence so no caller ever sees
// an exception or an empty string for non-empty input.
// ---------------------------------------------------------------------------
#include "platform/Utf.h"

#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hh::platform {

namespace {

/// Largest byte / code-unit count the Win32 conversion APIs accept (int).
constexpr size_t kMaxConvertible = static_cast<size_t>(INT_MAX);

/// The Unicode replacement character used for undecodable input.
constexpr wchar_t kReplacement = L'\xFFFD';

/**
 * @brief True for the ASCII whitespace set handled by trim(): space, tab,
 *        CR, LF, VT and FF.
 */
template <class Ch>
constexpr bool isAsciiSpace(Ch c) noexcept
{
    return c == static_cast<Ch>(' ') || c == static_cast<Ch>('\t') || c == static_cast<Ch>('\r')
        || c == static_cast<Ch>('\n') || c == static_cast<Ch>('\v') || c == static_cast<Ch>('\f');
}

/**
 * @brief Shared trim implementation for both character widths.
 */
template <class View>
View trimImpl(View s) noexcept
{
    size_t begin = 0;
    size_t end = s.size();
    // Walk in from both ends while the characters are whitespace.
    while (begin < end && isAsciiSpace(s[begin])) {
        ++begin;
    }
    while (end > begin && isAsciiSpace(s[end - 1])) {
        --end;
    }
    return s.substr(begin, end - begin);
}

/**
 * @brief One MultiByteToWideChar round trip (size query + convert).
 *
 * @return Converted text, or empty when the API failed. Since every input
 *         byte yields at least one code unit (or a failure), an empty result
 *         for non-empty input unambiguously means failure.
 */
std::wstring multiByteToWide(UINT codePage, DWORD flags, std::string_view bytes)
{
    if (bytes.empty() || bytes.size() > kMaxConvertible) {
        return {};
    }
    const int inLength = static_cast<int>(bytes.size());

    // First call measures, second call converts.
    const int needed = ::MultiByteToWideChar(codePage, flags, bytes.data(), inLength, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(codePage, flags, bytes.data(), inLength, out.data(), needed);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<size_t>(written));
    return out;
}

/**
 * @brief One WideCharToMultiByte round trip (size query + convert).
 *
 * @return Converted bytes, or empty on failure (see multiByteToWide).
 */
std::string wideToMultiByte(UINT codePage, DWORD flags, std::wstring_view wide)
{
    if (wide.empty() || wide.size() > kMaxConvertible) {
        return {};
    }
    const int inLength = static_cast<int>(wide.size());

    // First call measures, second call converts.
    const int needed = ::WideCharToMultiByte(codePage, flags, wide.data(), inLength, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(codePage, flags, wide.data(), inLength, out.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<size_t>(written));
    return out;
}

/**
 * @brief Last-resort byte mapper: ASCII passes through, everything else
 *        becomes U+FFFD. Only reached when the OS converter itself fails.
 */
std::wstring bytesToWideLossy(std::string_view bytes)
{
    std::wstring out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u < 0x80 ? static_cast<wchar_t>(u) : kReplacement);
    }
    return out;
}

/**
 * @brief Last-resort UTF-16 -> UTF-8 mapper: ASCII passes through, anything
 *        else becomes the UTF-8 encoding of U+FFFD (EF BF BD).
 */
std::string wideToUtf8Lossy(std::wstring_view wide)
{
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t c : wide) {
        if (static_cast<uint32_t>(c) < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.append("\xEF\xBF\xBD");
        }
    }
    return out;
}

/**
 * @brief Maps each byte to the same code point (ISO-8859-1 view of the
 *        bytes). Used when a caller names a code page the OS rejects.
 */
std::wstring bytesToWideLatin1(std::string_view bytes)
{
    std::wstring out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Encoding conversion
// ---------------------------------------------------------------------------

/**
 * @brief UTF-8 -> UTF-16 with U+FFFD substitution for invalid sequences.
 */
std::wstring toWide(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    // Inputs beyond INT_MAX bytes cannot be handed to the API in one go;
    // clamp rather than fail (a split sequence at the cut becomes U+FFFD).
    if (utf8.size() > kMaxConvertible) {
        utf8 = utf8.substr(0, kMaxConvertible);
    }

    // Strict pass: rejects malformed sequences outright.
    std::wstring strict = multiByteToWide(CP_UTF8, MB_ERR_INVALID_CHARS, utf8);
    if (!strict.empty()) {
        return strict;
    }

    // Lenient pass: the OS substitutes U+FFFD for every bad sequence.
    std::wstring lenient = multiByteToWide(CP_UTF8, 0, utf8);
    if (!lenient.empty()) {
        return lenient;
    }

    // The converter itself failed (should not happen): map by hand.
    return bytesToWideLossy(utf8);
}

/**
 * @brief UTF-16 -> UTF-8 with U+FFFD substitution for unpaired surrogates.
 */
std::string toUtf8(std::wstring_view wide)
{
    if (wide.empty()) {
        return {};
    }
    // Same clamp as toWide: a split surrogate pair becomes U+FFFD.
    if (wide.size() > kMaxConvertible) {
        wide = wide.substr(0, kMaxConvertible);
    }

    // Strict pass first, lenient pass on failure.
    std::string strict = wideToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide);
    if (!strict.empty()) {
        return strict;
    }
    std::string lenient = wideToMultiByte(CP_UTF8, 0, wide);
    if (!lenient.empty()) {
        return lenient;
    }

    // Converter failure: hand-mapped fallback.
    return wideToUtf8Lossy(wide);
}

/**
 * @brief Decodes text in a legacy code page (e.g. CP_ACP / CP_OEMCP).
 *
 * MB_ERR_INVALID_CHARS is only legal for a handful of code pages, so the
 * legacy path always runs lenient. UTF-8 is routed through toWide() to get
 * the strict/lenient behaviour, and an unknown code page degrades to a
 * Latin-1 view of the bytes rather than dropping the text.
 */
std::wstring fromCodePage(std::string_view bytes, UINT codePage)
{
    if (bytes.empty()) {
        return {};
    }
    if (codePage == CP_UTF8) {
        return toWide(bytes);
    }
    if (bytes.size() > kMaxConvertible) {
        bytes = bytes.substr(0, kMaxConvertible);
    }

    // Legacy pages: the OS maps undefined bytes to a best-fit character.
    std::wstring out = multiByteToWide(codePage, 0, bytes);
    if (!out.empty()) {
        return out;
    }
    return bytesToWideLatin1(bytes);
}

// ---------------------------------------------------------------------------
// Case mapping and comparison
// ---------------------------------------------------------------------------

/**
 * @brief Upper-cases a copy with CharUpperBuffW (file-system style, 1:1).
 */
std::wstring toUpperInvariant(std::wstring_view s)
{
    std::wstring out(s);
    if (out.empty()) {
        return out;
    }
    // CharUpperBuffW works on an explicit length, so embedded NULs survive.
    if (out.size() <= static_cast<size_t>(UINT32_MAX)) {
        ::CharUpperBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    return out;
}

/**
 * @brief Lower-cases a copy with CharLowerBuffW.
 */
std::wstring toLowerInvariant(std::wstring_view s)
{
    std::wstring out(s);
    if (out.empty()) {
        return out;
    }
    if (out.size() <= static_cast<size_t>(UINT32_MAX)) {
        ::CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    return out;
}

/**
 * @brief Ordinal case-insensitive equality via CompareStringOrdinal.
 *
 * Ordinal folding is one code unit to one code unit, so differing lengths
 * can never compare equal and are rejected before the API call.
 */
bool iequals(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    if (a.size() > kMaxConvertible) {
        return false;
    }
    const int result = ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                              b.data(), static_cast<int>(b.size()), TRUE);
    return result == CSTR_EQUAL;
}

/**
 * @brief Case-insensitive prefix check (ordinal).
 */
bool istartsWith(std::wstring_view s, std::wstring_view prefix)
{
    if (prefix.size() > s.size()) {
        return false;
    }
    return iequals(s.substr(0, prefix.size()), prefix);
}

/**
 * @brief Case-insensitive suffix check (ordinal).
 */
bool iendsWith(std::wstring_view s, std::wstring_view suffix)
{
    if (suffix.size() > s.size()) {
        return false;
    }
    return iequals(s.substr(s.size() - suffix.size()), suffix);
}

/**
 * @brief Case-insensitive substring search.
 *
 * Both sides are upper-cased with the same 1:1 mapping, so offsets in the
 * folded copy line up with offsets in the original.
 */
size_t ifind(std::wstring_view haystack, std::wstring_view needle)
{
    if (needle.empty()) {
        return 0;
    }
    if (needle.size() > haystack.size()) {
        return std::wstring::npos;
    }
    const std::wstring foldedHay = toUpperInvariant(haystack);
    const std::wstring foldedNeedle = toUpperInvariant(needle);
    return foldedHay.find(foldedNeedle);
}

/**
 * @brief Case-insensitive contains check.
 */
bool icontains(std::wstring_view haystack, std::wstring_view needle)
{
    return ifind(haystack, needle) != std::wstring::npos;
}

// ---------------------------------------------------------------------------
// Trimming, splitting, joining, replacing
// ---------------------------------------------------------------------------

/**
 * @brief Trims ASCII whitespace from both ends (wide).
 */
std::wstring_view trim(std::wstring_view s)
{
    return trimImpl(s);
}

/**
 * @brief Trims ASCII whitespace from both ends (narrow).
 */
std::string_view trim(std::string_view s)
{
    return trimImpl(s);
}

/**
 * @brief Splits on a single separator character.
 *
 * With @p skipEmpty set, runs of separators and leading/trailing separators
 * produce no empty pieces; otherwise every piece is kept, so "a,,b" gives
 * three entries.
 */
std::vector<std::wstring> split(std::wstring_view s, wchar_t sep, bool skipEmpty)
{
    std::vector<std::wstring> parts;
    if (s.empty()) {
        return parts;
    }

    // Walk piece by piece; "start" is the beginning of the current piece.
    size_t start = 0;
    while (start <= s.size()) {
        const size_t pos = s.find(sep, start);
        const size_t end = (pos == std::wstring_view::npos) ? s.size() : pos;
        std::wstring_view piece = s.substr(start, end - start);
        if (!piece.empty() || !skipEmpty) {
            parts.emplace_back(piece);
        }
        if (pos == std::wstring_view::npos) {
            break;
        }
        start = pos + 1;
    }
    return parts;
}

/**
 * @brief Joins parts with a separator between (not after) them.
 */
std::wstring join(const std::vector<std::wstring>& parts, std::wstring_view sep)
{
    // Reserve once to avoid repeated reallocation on long lists.
    size_t total = 0;
    for (const auto& p : parts) {
        total += p.size() + sep.size();
    }
    std::wstring out;
    out.reserve(total);

    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.append(sep);
        }
        out.append(parts[i]);
    }
    return out;
}

/**
 * @brief Replaces every occurrence of @p from with @p to (non-overlapping,
 *        left to right). An empty @p from returns the input unchanged.
 */
std::wstring replaceAll(std::wstring_view s, std::wstring_view from, std::wstring_view to)
{
    if (from.empty() || s.empty()) {
        return std::wstring(s);
    }
    std::wstring out;
    out.reserve(s.size());

    // Copy the text between matches, then the replacement, until no match.
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t hit = s.find(from, pos);
        if (hit == std::wstring_view::npos) {
            out.append(s.substr(pos));
            break;
        }
        out.append(s.substr(pos, hit - pos));
        out.append(to);
        pos = hit + from.size();
    }
    return out;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

/**
 * @brief Parses a decimal integer with optional sign; overflow and any
 *        non-digit character reject the whole input.
 */
std::optional<long long> parseInt(std::wstring_view s)
{
    const std::wstring_view t = trim(s);
    if (t.empty()) {
        return std::nullopt;
    }

    // Optional leading sign, then at least one digit is required.
    size_t i = 0;
    bool negative = false;
    if (t[0] == L'+' || t[0] == L'-') {
        negative = (t[0] == L'-');
        i = 1;
    }
    if (i >= t.size()) {
        return std::nullopt;
    }

    // Accumulate unsigned so the full negative range is reachable.
    unsigned long long acc = 0;
    for (; i < t.size(); ++i) {
        const wchar_t c = t[i];
        if (c < L'0' || c > L'9') {
            return std::nullopt;
        }
        const unsigned long long digit = static_cast<unsigned long long>(c - L'0');
        if (acc > (ULLONG_MAX - digit) / 10ull) {
            return std::nullopt;
        }
        acc = acc * 10ull + digit;
    }

    // Range check against long long on each side of zero.
    constexpr unsigned long long kMaxPositive = static_cast<unsigned long long>(LLONG_MAX);
    constexpr unsigned long long kMaxNegative = kMaxPositive + 1ull;
    if (negative) {
        if (acc > kMaxNegative) {
            return std::nullopt;
        }
        if (acc == kMaxNegative) {
            return LLONG_MIN;
        }
        return -static_cast<long long>(acc);
    }
    if (acc > kMaxPositive) {
        return std::nullopt;
    }
    return static_cast<long long>(acc);
}

/**
 * @brief Parses an invariant-culture floating point number.
 *
 * The character set is validated up front (digits, sign, '.', exponent) so
 * hex floats, "inf"/"nan" and localised separators are rejected. The
 * validated ASCII text is then handed to std::from_chars, which is locale
 * independent and reports exactly how much it consumed; any leftover means
 * junk and the whole value is rejected.
 */
std::optional<double> parseDouble(std::wstring_view s)
{
    const std::wstring_view t = trim(s);
    if (t.empty() || t.size() > 512) {
        return std::nullopt;
    }

    // Validate and narrow in one pass.
    std::string narrow;
    narrow.reserve(t.size());
    bool sawDigit = false;
    for (const wchar_t c : t) {
        if (c >= L'0' && c <= L'9') {
            sawDigit = true;
        } else if (c != L'+' && c != L'-' && c != L'.' && c != L'e' && c != L'E') {
            return std::nullopt;
        }
        narrow.push_back(static_cast<char>(c));
    }
    if (!sawDigit) {
        return std::nullopt;
    }

    // from_chars does not accept a leading '+', strip it ourselves.
    const char* begin = narrow.data();
    const char* end = begin + narrow.size();
    if (*begin == '+') {
        ++begin;
    }
    if (begin == end) {
        return std::nullopt;
    }

    // Whole-string consumption is the "no junk" guarantee.
    double value = 0.0;
    const std::from_chars_result r = std::from_chars(begin, end, value, std::chars_format::general);
    if (r.ec != std::errc() || r.ptr != end) {
        return std::nullopt;
    }
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

/**
 * @brief Parses the usual boolean spellings, case-insensitively.
 */
std::optional<bool> parseBool(std::wstring_view s)
{
    const std::wstring_view t = trim(s);
    if (t.empty() || t.size() > 8) {
        return std::nullopt;
    }

    // Truthy spellings.
    if (iequals(t, L"true") || iequals(t, L"1") || iequals(t, L"yes") || iequals(t, L"on")) {
        return true;
    }
    // Falsy spellings.
    if (iequals(t, L"false") || iequals(t, L"0") || iequals(t, L"no") || iequals(t, L"off")) {
        return false;
    }
    return std::nullopt;
}

} // namespace hh::platform
