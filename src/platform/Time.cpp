// ---------------------------------------------------------------------------
// Time.cpp - clocks and time formatting.
//
// "Utc" values are FILETIME-style uint64 (100 ns ticks since 1601-01-01).
// Local formatting converts through SYSTEMTIME and the OS time-zone tables
// so DST is right for the instant being formatted, not just for "now".
// The AME timestamp parser is a small hand-written scanner: no std::regex,
// no exceptions, and it tolerates the narrow no-break spaces newer Windows
// builds put in front of "AM"/"PM".
// ---------------------------------------------------------------------------
#include "platform/Time.h"

#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace hh::platform {

namespace {

/// FILETIME ticks per unit.
constexpr uint64_t kTicksPerMillisecond = 10'000ull;
constexpr uint64_t kTicksPerSecond = 10'000'000ull;
constexpr uint64_t kTicksPerMinute = 600'000'000ull;
constexpr uint64_t kTicksPerDay = 864'000'000'000ull;

/// 1970-01-01 expressed as FILETIME ticks.
constexpr uint64_t kUnixEpochTicks = 116'444'736'000'000'000ull;

/// FileTimeToSystemTime rejects values with the top bit set.
constexpr uint64_t kMaxFileTime = 0x7FFFFFFFFFFFFFFFull;

/// Month abbreviations for the friendly format.
constexpr const wchar_t* kMonthNames[12] = {
    L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
    L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec",
};

/**
 * @brief QueryPerformanceFrequency, fetched once. It is constant for the
 *        lifetime of the system; a bogus zero is replaced by one so the
 *        divisions below can never fault.
 */
uint64_t qpcFrequency() noexcept
{
    static const uint64_t frequency = [] {
        LARGE_INTEGER value{};
        if (!::QueryPerformanceFrequency(&value) || value.QuadPart <= 0) {
            return uint64_t{1};
        }
        return static_cast<uint64_t>(value.QuadPart);
    }();
    return frequency;
}

/**
 * @brief Raw QPC counter (0 on the impossible failure path).
 */
uint64_t qpcCounter() noexcept
{
    LARGE_INTEGER value{};
    if (!::QueryPerformanceCounter(&value) || value.QuadPart < 0) {
        return 0;
    }
    return static_cast<uint64_t>(value.QuadPart);
}

/**
 * @brief Converts a FILETIME uint64 into a UTC SYSTEMTIME.
 */
bool toUtcSystemTime(uint64_t utc, SYSTEMTIME& out) noexcept
{
    if (utc > kMaxFileTime) {
        return false;
    }
    const FILETIME ft = uint64ToFileTime(utc);
    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&ft, &st)) {
        return false;
    }
    out = st;
    return true;
}

/**
 * @brief Converts a FILETIME uint64 into local SYSTEMTIME plus the UTC
 *        offset (in minutes, east positive) that applied at that instant.
 *
 * The offset is derived from the actual local/UTC difference, which is
 * exact for that date; GetTimeZoneInformation is only a fallback because it
 * describes "now" rather than the formatted instant.
 */
bool toLocalSystemTime(uint64_t utc, SYSTEMTIME& localOut, int& offsetMinutesOut) noexcept
{
    SYSTEMTIME utcSt{};
    if (!toUtcSystemTime(utc, utcSt)) {
        return false;
    }
    SYSTEMTIME local{};
    if (!::SystemTimeToTzSpecificLocalTime(nullptr, &utcSt, &local)) {
        return false;
    }

    // Offset from the round trip; both sides truncated to whole milliseconds
    // so the difference is an exact multiple of the minute.
    int offsetMinutes = 0;
    FILETIME localFt{};
    if (::SystemTimeToFileTime(&local, &localFt)) {
        const int64_t localMs = static_cast<int64_t>(fileTimeToUint64(localFt) / kTicksPerMillisecond);
        const int64_t utcMs = static_cast<int64_t>(utc / kTicksPerMillisecond);
        const int64_t diffMs = localMs - utcMs;
        const int64_t half = (diffMs >= 0) ? 30'000 : -30'000;
        offsetMinutes = static_cast<int>((diffMs + half) / 60'000);
    } else {
        // Fallback: the current bias (Bias is west-positive, hence the negation).
        TIME_ZONE_INFORMATION tz{};
        const DWORD zone = ::GetTimeZoneInformation(&tz);
        LONG bias = tz.Bias;
        if (zone == TIME_ZONE_ID_DAYLIGHT) {
            bias += tz.DaylightBias;
        } else if (zone == TIME_ZONE_ID_STANDARD) {
            bias += tz.StandardBias;
        }
        offsetMinutes = static_cast<int>(-bias);
    }

    localOut = local;
    offsetMinutesOut = offsetMinutes;
    return true;
}

/**
 * @brief Converts a local SYSTEMTIME to FILETIME ticks (UTC).
 *
 * TzSpecificLocalTimeToSystemTime applies the zone rules for that date;
 * when it fails (exotic zones), the current bias is applied by hand.
 */
std::optional<uint64_t> localSystemTimeToUtc(const SYSTEMTIME& local) noexcept
{
    SYSTEMTIME utcSt{};
    if (::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utcSt)) {
        FILETIME ft{};
        if (::SystemTimeToFileTime(&utcSt, &ft)) {
            return fileTimeToUint64(ft);
        }
    }

    // Manual fallback: treat the local time as UTC and shift by the bias.
    FILETIME rawFt{};
    if (!::SystemTimeToFileTime(&local, &rawFt)) {
        return std::nullopt;
    }
    TIME_ZONE_INFORMATION tz{};
    const DWORD zone = ::GetTimeZoneInformation(&tz);
    if (zone == TIME_ZONE_ID_INVALID) {
        return fileTimeToUint64(rawFt);
    }
    LONG bias = tz.Bias;
    if (zone == TIME_ZONE_ID_DAYLIGHT) {
        bias += tz.DaylightBias;
    } else if (zone == TIME_ZONE_ID_STANDARD) {
        bias += tz.StandardBias;
    }
    const int64_t ticks = static_cast<int64_t>(fileTimeToUint64(rawFt))
                        + static_cast<int64_t>(bias) * static_cast<int64_t>(kTicksPerMinute);
    if (ticks < 0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(ticks);
}

/**
 * @brief Days in a month, leap years included.
 */
unsigned daysInMonth(unsigned year, unsigned month) noexcept
{
    static constexpr unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29u : 28u;
    }
    return kDays[month - 1];
}

// ---------------------------------------------------------------------------
// Scanner primitives shared by parseAmeTimestamp and parseHms
// ---------------------------------------------------------------------------

/**
 * @brief Whitespace as it appears in locale-formatted timestamps: ASCII
 *        space/tab plus NBSP (U+00A0), figure space (U+2007) and the narrow
 *        NBSP (U+202F) that ICU-based formatting inserts before AM/PM.
 */
constexpr bool isTimestampSpace(wchar_t c) noexcept
{
    return c == L' ' || c == L'\t' || c == L'\x00A0' || c == L'\x2007' || c == L'\x202F';
}

constexpr bool isDigit(wchar_t c) noexcept
{
    return c >= L'0' && c <= L'9';
}

constexpr bool isAsciiLetter(wchar_t c) noexcept
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

/**
 * @brief Minimal cursor over a string view.
 */
struct Scanner {
    std::wstring_view text;
    size_t pos = 0;

    [[nodiscard]] bool atEnd() const noexcept { return pos >= text.size(); }
    [[nodiscard]] wchar_t peek() const noexcept { return atEnd() ? L'\0' : text[pos]; }

    /// Skips timestamp whitespace.
    void skipSpaces() noexcept
    {
        while (!atEnd() && isTimestampSpace(text[pos])) {
            ++pos;
        }
    }

    /// Consumes @p c when it is next.
    bool accept(wchar_t c) noexcept
    {
        if (!atEnd() && text[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }

    /**
     * @brief Reads 1..maxDigits decimal digits.
     * @param value      Parsed number.
     * @param digitCount Digits consumed (lets the caller spot a 4-digit year).
     */
    bool readNumber(unsigned maxDigits, unsigned& value, unsigned& digitCount) noexcept
    {
        value = 0;
        digitCount = 0;
        while (!atEnd() && isDigit(text[pos]) && digitCount < maxDigits) {
            value = value * 10u + static_cast<unsigned>(text[pos] - L'0');
            ++pos;
            ++digitCount;
        }
        // Too many digits in a row is junk, not a number.
        if (digitCount == 0 || (!atEnd() && isDigit(text[pos]))) {
            return false;
        }
        return true;
    }
};

/**
 * @brief Reads an optional AM/PM designator ("AM", "pm", "a.m.").
 * @return 0 = none, 1 = AM, 2 = PM, -1 = unrecognised token.
 */
int readMeridiem(Scanner& sc) noexcept
{
    sc.skipSpaces();
    if (sc.atEnd() || !isAsciiLetter(sc.peek())) {
        return 0;
    }

    // Collect letters and dots ("a.m."), then strip the dots.
    wchar_t letters[4] = {};
    unsigned count = 0;
    while (!sc.atEnd() && (isAsciiLetter(sc.peek()) || sc.peek() == L'.')) {
        const wchar_t c = sc.peek();
        ++sc.pos;
        if (c == L'.') {
            continue;
        }
        if (count >= 2) {
            return -1;
        }
        // ASCII upper-case by hand; the scanner never touches the locale.
        letters[count++] = (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - (L'a' - L'A')) : c;
    }
    if (count != 2 || letters[1] != L'M') {
        return -1;
    }
    if (letters[0] == L'A') {
        return 1;
    }
    if (letters[0] == L'P') {
        return 2;
    }
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------

/**
 * @brief Current wall-clock time as FILETIME uint64 (sub-microsecond).
 */
uint64_t nowUtc()
{
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    return fileTimeToUint64(ft);
}

/**
 * @brief Monotonic milliseconds from QPC. Split into whole and fractional
 *        seconds so the multiplication cannot overflow for decades.
 */
uint64_t nowMonotonicMs()
{
    const uint64_t counter = qpcCounter();
    const uint64_t frequency = qpcFrequency();
    const uint64_t wholeSeconds = counter / frequency;
    const uint64_t remainder = counter % frequency;
    return wholeSeconds * 1000ull + (remainder * 1000ull) / frequency;
}

/**
 * @brief Monotonic seconds as double (animation clocks).
 */
double nowMonotonicSeconds()
{
    return static_cast<double>(qpcCounter()) / static_cast<double>(qpcFrequency());
}

/**
 * @brief FILETIME -> uint64.
 */
uint64_t fileTimeToUint64(const FILETIME& ft)
{
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
}

/**
 * @brief uint64 -> FILETIME.
 */
FILETIME uint64ToFileTime(uint64_t v)
{
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFull);
    ft.dwHighDateTime = static_cast<DWORD>(v >> 32);
    return ft;
}

/**
 * @brief FILETIME uint64 -> Unix milliseconds (negative before 1970).
 */
int64_t utcToUnixMs(uint64_t utc)
{
    const int64_t utcMs = static_cast<int64_t>(utc / kTicksPerMillisecond);
    const int64_t epochMs = static_cast<int64_t>(kUnixEpochTicks / kTicksPerMillisecond);
    return utcMs - epochMs;
}

/**
 * @brief Unix milliseconds -> FILETIME uint64, clamped to the representable
 *        range (before 1601 -> 0).
 */
uint64_t unixMsToUtc(int64_t unixMs)
{
    const int64_t epochMs = static_cast<int64_t>(kUnixEpochTicks / kTicksPerMillisecond);
    // Below the FILETIME epoch: nothing sensible to return.
    if (unixMs < -epochMs) {
        return 0;
    }
    // Above the signed range: clamp instead of wrapping.
    const int64_t maxMs = static_cast<int64_t>(kMaxFileTime / kTicksPerMillisecond) - epochMs;
    if (unixMs > maxMs) {
        return kMaxFileTime;
    }
    return static_cast<uint64_t>(unixMs + epochMs) * kTicksPerMillisecond;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

/**
 * @brief "2026-09-16 14:25:06.123 -07:00" in local time.
 *
 * Falls back to the UTC form when the local conversion is impossible
 * (out-of-range tick values), so log lines never lose their timestamp.
 */
std::wstring formatLocalIso(uint64_t utc, bool withMillis)
{
    SYSTEMTIME local{};
    int offsetMinutes = 0;
    if (!toLocalSystemTime(utc, local, offsetMinutes)) {
        return formatUtcIso(utc);
    }

    // Offset pieces: sign, hours, minutes.
    const wchar_t sign = (offsetMinutes < 0) ? L'-' : L'+';
    const unsigned absOffset = static_cast<unsigned>(std::abs(offsetMinutes));
    const unsigned offsetHours = absOffset / 60u;
    const unsigned offsetMins = absOffset % 60u;

    if (withMillis) {
        const unsigned millis = static_cast<unsigned>((utc / kTicksPerMillisecond) % 1000ull);
        return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} {}{:02}:{:02}",
                           local.wYear, local.wMonth, local.wDay,
                           local.wHour, local.wMinute, local.wSecond, millis,
                           sign, offsetHours, offsetMins);
    }
    return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02} {}{:02}:{:02}",
                       local.wYear, local.wMonth, local.wDay,
                       local.wHour, local.wMinute, local.wSecond,
                       sign, offsetHours, offsetMins);
}

/**
 * @brief "2026-09-16T21:25:06Z" (empty for unrepresentable values).
 */
std::wstring formatUtcIso(uint64_t utc)
{
    SYSTEMTIME st{};
    if (!toUtcSystemTime(utc, st)) {
        return {};
    }
    return std::format(L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/**
 * @brief Short human form: "Today 14:25", "Yesterday 14:25", "Sep 16 14:25"
 *        or "Sep 16, 2025 14:25" for other years.
 */
std::wstring formatFriendly(uint64_t utc)
{
    SYSTEMTIME local{};
    int offset = 0;
    if (!toLocalSystemTime(utc, local, offset)) {
        return {};
    }

    // Local calendar date of "now" and of "24 hours ago" for the labels.
    const uint64_t now = nowUtc();
    SYSTEMTIME today{};
    SYSTEMTIME yesterday{};
    const bool haveToday = toLocalSystemTime(now, today, offset);
    const bool haveYesterday = (now >= kTicksPerDay) && toLocalSystemTime(now - kTicksPerDay, yesterday, offset);

    const auto sameDay = [](const SYSTEMTIME& a, const SYSTEMTIME& b) noexcept {
        return a.wYear == b.wYear && a.wMonth == b.wMonth && a.wDay == b.wDay;
    };

    if (haveToday && sameDay(local, today)) {
        return std::format(L"Today {:02}:{:02}", local.wHour, local.wMinute);
    }
    if (haveYesterday && sameDay(local, yesterday)) {
        return std::format(L"Yesterday {:02}:{:02}", local.wHour, local.wMinute);
    }

    // Month index is 1-based in SYSTEMTIME; guard before indexing the table.
    const wchar_t* month = (local.wMonth >= 1 && local.wMonth <= 12) ? kMonthNames[local.wMonth - 1] : L"???";
    if (haveToday && local.wYear == today.wYear) {
        return std::format(L"{} {} {:02}:{:02}", month, local.wDay, local.wHour, local.wMinute);
    }
    return std::format(L"{} {}, {} {:02}:{:02}", month, local.wDay, local.wYear, local.wHour, local.wMinute);
}

/**
 * @brief "00:12:34" from milliseconds (hours grow past 99 if needed).
 */
std::wstring formatDuration(uint64_t millis)
{
    const uint64_t totalSeconds = millis / 1000ull;
    const uint64_t hours = totalSeconds / 3600ull;
    const uint64_t minutes = (totalSeconds % 3600ull) / 60ull;
    const uint64_t seconds = totalSeconds % 60ull;
    return std::format(L"{:02}:{:02}:{:02}", hours, minutes, seconds);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

/**
 * @brief Parses AME's locale-formatted timestamp into FILETIME ticks (UTC).
 *
 * Grammar (whitespace between tokens is flexible):
 *   date  := N sep N sep N            sep in { '/', '.', '-' }, both the same
 *   time  := H ':' M [':' S ['.' frac]]
 *   ampm  := "AM" | "PM" | "a.m." | "p.m."   (optional, case-insensitive)
 *
 * Date interpretation:
 *   - first number has 4 digits            -> Y-M-D (ISO, ja-JP, sv-SE)
 *   - '.' separator                        -> D.M.Y (de-DE, ru-RU, ...)
 *   - '-' separator, year last             -> D-M-Y (nl-NL, ...)
 *   - '/' separator                        -> a value above 12 decides,
 *                                             otherwise @p preferDayFirst
 *   - a 2-digit year is taken as 20xx
 */
std::optional<uint64_t> parseAmeTimestamp(std::wstring_view text, bool preferDayFirst)
{
    if (text.empty() || text.size() > 64) {
        return std::nullopt;
    }
    Scanner sc{text, 0};
    sc.skipSpaces();

    // ---- date: three numbers with one repeated separator ------------------
    unsigned first = 0;
    unsigned second = 0;
    unsigned third = 0;
    unsigned firstDigits = 0;
    unsigned secondDigits = 0;
    unsigned thirdDigits = 0;
    if (!sc.readNumber(4, first, firstDigits)) {
        return std::nullopt;
    }
    const wchar_t sep = sc.peek();
    if (sep != L'/' && sep != L'.' && sep != L'-') {
        return std::nullopt;
    }
    ++sc.pos;
    if (!sc.readNumber(4, second, secondDigits)) {
        return std::nullopt;
    }
    if (!sc.accept(sep)) {
        return std::nullopt;
    }
    if (!sc.readNumber(4, third, thirdDigits)) {
        return std::nullopt;
    }

    // Decide which number is which.
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (firstDigits == 4) {
        // Year first: the order is fixed regardless of separator.
        year = first;
        month = second;
        day = third;
    } else {
        year = third;
        if (thirdDigits <= 2) {
            year += 2000u;
        }
        // Values above 12 can only be a day; otherwise apply the locale rule.
        bool dayFirst = false;
        if (sep == L'.' || sep == L'-') {
            dayFirst = true;
        } else {
            dayFirst = preferDayFirst;
        }
        if (first > 12 && second <= 12) {
            dayFirst = true;
        } else if (second > 12 && first <= 12) {
            dayFirst = false;
        }
        if (dayFirst) {
            day = first;
            month = second;
        } else {
            month = first;
            day = second;
        }
    }

    // ---- separator between date and time (spaces, or ISO 'T') ------------
    sc.skipSpaces();
    if (sc.peek() == L'T' || sc.peek() == L't') {
        ++sc.pos;
    }
    sc.skipSpaces();

    // ---- time: H:MM[:SS[.fff]] --------------------------------------------
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned secondOfMinute = 0;
    unsigned millis = 0;
    unsigned digits = 0;
    if (!sc.readNumber(2, hour, digits)) {
        return std::nullopt;
    }
    if (!sc.accept(L':')) {
        return std::nullopt;
    }
    if (!sc.readNumber(2, minute, digits)) {
        return std::nullopt;
    }
    if (sc.accept(L':')) {
        if (!sc.readNumber(2, secondOfMinute, digits)) {
            return std::nullopt;
        }
        // Optional fraction: keep the first three digits as milliseconds.
        if (sc.peek() == L'.' || sc.peek() == L',') {
            ++sc.pos;
            unsigned fraction = 0;
            unsigned fractionDigits = 0;
            while (!sc.atEnd() && isDigit(sc.peek())) {
                if (fractionDigits < 3) {
                    fraction = fraction * 10u + static_cast<unsigned>(sc.peek() - L'0');
                    ++fractionDigits;
                }
                ++sc.pos;
            }
            if (fractionDigits == 0) {
                return std::nullopt;
            }
            while (fractionDigits < 3) {
                fraction *= 10u;
                ++fractionDigits;
            }
            millis = fraction;
        }
    }

    // ---- optional AM/PM ---------------------------------------------------
    const int meridiem = readMeridiem(sc);
    if (meridiem < 0) {
        return std::nullopt;
    }
    if (meridiem != 0) {
        // 12-hour clock: 12 AM is midnight, 12 PM is noon.
        if (hour < 1 || hour > 12) {
            return std::nullopt;
        }
        if (meridiem == 1 && hour == 12) {
            hour = 0;
        } else if (meridiem == 2 && hour != 12) {
            hour += 12u;
        }
    }

    // Nothing but whitespace may follow.
    sc.skipSpaces();
    if (!sc.atEnd()) {
        return std::nullopt;
    }

    // ---- range validation -------------------------------------------------
    if (year < 1601 || year > 30827 || month < 1 || month > 12) {
        return std::nullopt;
    }
    if (day < 1 || day > daysInMonth(year, month)) {
        return std::nullopt;
    }
    if (hour > 23 || minute > 59 || secondOfMinute > 59) {
        return std::nullopt;
    }

    // ---- local SYSTEMTIME -> UTC ticks ------------------------------------
    SYSTEMTIME local{};
    local.wYear = static_cast<WORD>(year);
    local.wMonth = static_cast<WORD>(month);
    local.wDay = static_cast<WORD>(day);
    local.wHour = static_cast<WORD>(hour);
    local.wMinute = static_cast<WORD>(minute);
    local.wSecond = static_cast<WORD>(secondOfMinute);
    local.wMilliseconds = static_cast<WORD>(millis);
    return localSystemTimeToUtc(local);
}

/**
 * @brief Parses "H:MM:SS" / "HH:MM:SS" (optionally ".fff") into milliseconds.
 */
std::optional<uint64_t> parseHms(std::wstring_view text)
{
    if (text.empty() || text.size() > 32) {
        return std::nullopt;
    }
    Scanner sc{text, 0};
    sc.skipSpaces();

    // Hours may run long for multi-day encodes; minutes/seconds are 1-2 digits.
    unsigned hours = 0;
    unsigned minutes = 0;
    unsigned seconds = 0;
    unsigned digits = 0;
    if (!sc.readNumber(7, hours, digits)) {
        return std::nullopt;
    }
    if (!sc.accept(L':')) {
        return std::nullopt;
    }
    if (!sc.readNumber(2, minutes, digits)) {
        return std::nullopt;
    }
    if (!sc.accept(L':')) {
        return std::nullopt;
    }
    if (!sc.readNumber(2, seconds, digits)) {
        return std::nullopt;
    }

    // Optional fraction (first three digits are milliseconds).
    unsigned millis = 0;
    if (sc.peek() == L'.' || sc.peek() == L',') {
        ++sc.pos;
        unsigned fractionDigits = 0;
        while (!sc.atEnd() && isDigit(sc.peek())) {
            if (fractionDigits < 3) {
                millis = millis * 10u + static_cast<unsigned>(sc.peek() - L'0');
                ++fractionDigits;
            }
            ++sc.pos;
        }
        if (fractionDigits == 0) {
            return std::nullopt;
        }
        while (fractionDigits < 3) {
            millis *= 10u;
            ++fractionDigits;
        }
    }

    // Trailing whitespace only.
    sc.skipSpaces();
    if (!sc.atEnd()) {
        return std::nullopt;
    }
    if (minutes > 59 || seconds > 59) {
        return std::nullopt;
    }

    const uint64_t total = (static_cast<uint64_t>(hours) * 3600ull
                          + static_cast<uint64_t>(minutes) * 60ull
                          + static_cast<uint64_t>(seconds)) * 1000ull
                         + static_cast<uint64_t>(millis);
    return total;
}

} // namespace hh::platform
