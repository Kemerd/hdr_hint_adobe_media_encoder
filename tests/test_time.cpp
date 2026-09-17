// ---------------------------------------------------------------------------
// test_time.cpp - clocks and time helpers: the AME timestamp parser in its
// locale variants, "HH:MM:SS" parsing and formatting, FILETIME / Unix
// conversions and the monotonic clock.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "platform/Time.h"
#include "platform/Win.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace platform = hh::platform;

namespace {

/**
 * @brief Renders a parsed timestamp as local "YYYY-MM-DD HH:MM:SS" using the
 *        OS conversions directly (FileTimeToSystemTime followed by
 *        SystemTimeToTzSpecificLocalTime), so the parser is checked against
 *        Windows rather than against its own formatting code.
 */
std::wstring localText(const std::optional<uint64_t>& utc) {
    if (!utc) { return L"(nullopt)"; }
    const FILETIME ft = platform::uint64ToFileTime(*utc);
    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&ft, &st)) { return L"(FileTimeToSystemTime failed)"; }
    SYSTEMTIME local{};
    if (!::SystemTimeToTzSpecificLocalTime(nullptr, &st, &local)) { return L"(SystemTimeToTzSpecificLocalTime failed)"; }
    return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
                       local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
}

} // namespace

// ---- parseAmeTimestamp -----------------------------------------------------------

/**
 * @brief The en-US AME form "MM/DD/YYYY hh:mm:ss PM" parses and converts
 *        back to the same local wall-clock time.
 */
HH_TEST(Time_parseAmeTimestampUsLocale) {
    const std::optional<uint64_t> t = platform::parseAmeTimestamp(L"09/16/2026 01:51:48 PM");
    CHECK(t.has_value());
    CHECK_WEQ(localText(t), L"2026-09-16 13:51:48");

    // The same instant spelled with lower-case and without the leading zero.
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"09/16/2026 1:51:48 pm")), L"2026-09-16 13:51:48");
    // A 24-hour time without a designator is taken as-is.
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"09/16/2026 13:51:48")), L"2026-09-16 13:51:48");
}

/**
 * @brief Single-digit month/day/hour and "AM" work: "9/6/2026 1:51:48 AM"
 *        is September 6th at 01:51:48.
 */
HH_TEST(Time_parseAmeTimestampSingleDigitFields) {
    const std::optional<uint64_t> t = platform::parseAmeTimestamp(L"9/6/2026 1:51:48 AM");
    CHECK(t.has_value());
    CHECK_WEQ(localText(t), L"2026-09-06 01:51:48");
}

/**
 * @brief The dotted European form is day-first: "16.09.2026 13:51:48".
 */
HH_TEST(Time_parseAmeTimestampDayFirstWithDots) {
    const std::optional<uint64_t> t = platform::parseAmeTimestamp(L"16.09.2026 13:51:48");
    CHECK(t.has_value());
    CHECK_WEQ(localText(t), L"2026-09-16 13:51:48");
    // Dots are day-first even for an ambiguous pair, whatever the preference.
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"05.06.2026 10:00:00", false)), L"2026-06-05 10:00:00");
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"05.06.2026 10:00:00", true)), L"2026-06-05 10:00:00");
}

/**
 * @brief The ISO form "YYYY-MM-DD HH:mm:ss" (with a space or a 'T') is
 *        always year-first.
 */
HH_TEST(Time_parseAmeTimestampIsoForm) {
    const std::optional<uint64_t> t = platform::parseAmeTimestamp(L"2026-09-16 13:51:48");
    CHECK(t.has_value());
    CHECK_WEQ(localText(t), L"2026-09-16 13:51:48");
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"2026-09-16T13:51:48")), L"2026-09-16 13:51:48");
    // Year-first ignores the day-first preference entirely.
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"2026-05-06 10:00:00", true)), L"2026-05-06 10:00:00");
    // Surrounding whitespace and a fraction of a second are tolerated.
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"  2026-09-16 13:51:48.250  ")), L"2026-09-16 13:51:48");
}

/**
 * @brief Text that is not a timestamp, or a timestamp with out-of-range
 *        fields, gives nullopt rather than a bogus instant.
 */
HH_TEST(Time_parseAmeTimestampRejectsGarbage) {
    CHECK_FALSE(platform::parseAmeTimestamp(L"garbage").has_value());
    CHECK_FALSE(platform::parseAmeTimestamp(L"").has_value());
    CHECK_FALSE(platform::parseAmeTimestamp(L"   ").has_value());
    CHECK_FALSE(platform::parseAmeTimestamp(L"09/16/2026").has_value());              // no time
    CHECK_FALSE(platform::parseAmeTimestamp(L"13:51:48").has_value());                // no date
    CHECK_FALSE(platform::parseAmeTimestamp(L"13/13/2026 10:00:00").has_value());     // no such month/day
    CHECK_FALSE(platform::parseAmeTimestamp(L"02/30/2026 10:00:00").has_value());     // Feb 30
    CHECK_FALSE(platform::parseAmeTimestamp(L"09/16/2026 25:00:00").has_value());     // hour 25
    CHECK_FALSE(platform::parseAmeTimestamp(L"09/16/2026 13:60:00").has_value());     // minute 60
    CHECK_FALSE(platform::parseAmeTimestamp(L"09/16/2026 13:51:48 XM").has_value());  // bad designator
    CHECK_FALSE(platform::parseAmeTimestamp(L"09/16/2026 13:51:48 PM").has_value());  // 13 PM
    CHECK_FALSE(platform::parseAmeTimestamp(L"09/16/2026 13:51:48 extra").has_value());
    CHECK_FALSE(platform::parseAmeTimestamp(L"09-16/2026 13:51:48").has_value());     // mixed separators
}

/**
 * @brief An ambiguous slash date obeys preferDayFirst: "05/06/2026" is
 *        June 5th day-first and May 6th month-first (the default).
 */
HH_TEST(Time_parseAmeTimestampDayFirstPreference) {
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"05/06/2026 10:00:00", true)), L"2026-06-05 10:00:00");
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"05/06/2026 10:00:00", false)), L"2026-05-06 10:00:00");
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"05/06/2026 10:00:00")), L"2026-05-06 10:00:00");

    // A value above 12 settles it regardless of the preference.
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"16/09/2026 13:51:48", false)), L"2026-09-16 13:51:48");
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"09/16/2026 13:51:48", true)), L"2026-09-16 13:51:48");
}

/**
 * @brief 12-hour edge cases: 12 AM is midnight, 12 PM is noon, and the
 *        narrow no-break space newer Windows builds put before AM/PM is
 *        accepted.
 */
HH_TEST(Time_parseAmeTimestampTwelveHourEdges) {
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"09/16/2026 12:00:00 AM")), L"2026-09-16 00:00:00");
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"09/16/2026 12:00:00 PM")), L"2026-09-16 12:00:00");
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"09/16/2026 11:59:59 PM")), L"2026-09-16 23:59:59");
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"09/16/2026 01:51:48 PM")), L"2026-09-16 13:51:48");
    CHECK_WEQ(localText(platform::parseAmeTimestamp(L"09/16/2026 01:51:48 p.m.")), L"2026-09-16 13:51:48");
    // 0 AM does not exist on a 12-hour clock.
    CHECK_FALSE(platform::parseAmeTimestamp(L"09/16/2026 00:10:00 AM").has_value());
}

// ---- parseHms / formatDuration -----------------------------------------------------

/**
 * @brief parseHms reads "HH:MM:SS" (hours may be a single digit) into
 *        milliseconds; two-component or malformed text is rejected.
 */
HH_TEST(Time_parseHms) {
    const std::optional<uint64_t> a = platform::parseHms(L"00:12:34");
    if (CHECK(a.has_value())) { CHECK_EQ(*a, 754000u); }

    const std::optional<uint64_t> b = platform::parseHms(L"1:02:03");
    if (CHECK(b.has_value())) { CHECK_EQ(*b, 3723000u); }

    const std::optional<uint64_t> zero = platform::parseHms(L"00:00:00");
    if (CHECK(zero.has_value())) { CHECK_EQ(*zero, 0u); }

    // Multi-day encodes have more than two hour digits.
    const std::optional<uint64_t> longRun = platform::parseHms(L"100:00:00");
    if (CHECK(longRun.has_value())) { CHECK_EQ(*longRun, 360000000u); }

    // An optional fraction contributes milliseconds.
    const std::optional<uint64_t> frac = platform::parseHms(L"00:00:01.250");
    if (CHECK(frac.has_value())) { CHECK_EQ(*frac, 1250u); }

    // "MM:SS" is not the AME encoding-time form: rejected.
    CHECK_FALSE(platform::parseHms(L"12:34").has_value());
    CHECK_FALSE(platform::parseHms(L"").has_value());
    CHECK_FALSE(platform::parseHms(L"abc").has_value());
    CHECK_FALSE(platform::parseHms(L"00:60:00").has_value());
    CHECK_FALSE(platform::parseHms(L"00:00:60").has_value());
    CHECK_FALSE(platform::parseHms(L"00:12:34 x").has_value());
    CHECK_FALSE(platform::parseHms(L"00:123:34").has_value());
}

/**
 * @brief formatDuration renders "HH:MM:SS" with zero padding and lets the
 *        hours grow past two digits.
 */
HH_TEST(Time_formatDuration) {
    CHECK_WEQ(platform::formatDuration(754000), L"00:12:34");
    CHECK_WEQ(platform::formatDuration(0), L"00:00:00");
    CHECK_WEQ(platform::formatDuration(999), L"00:00:00");        // sub-second truncates
    CHECK_WEQ(platform::formatDuration(3723999), L"01:02:03");
    CHECK_WEQ(platform::formatDuration(360000000), L"100:00:00");

    // parseHms and formatDuration are inverses on whole seconds.
    const std::optional<uint64_t> parsed = platform::parseHms(platform::formatDuration(754000));
    if (CHECK(parsed.has_value())) { CHECK_EQ(*parsed, 754000u); }
}

// ---- FILETIME / Unix conversions -----------------------------------------------

/**
 * @brief fileTimeToUint64 and uint64ToFileTime are exact inverses and split
 *        the value into the expected low/high words.
 */
HH_TEST(Time_fileTimeRoundTrip) {
    const uint64_t value = 0x0123456789ABCDEFull;
    const FILETIME ft = platform::uint64ToFileTime(value);
    CHECK_EQ(ft.dwLowDateTime, 0x89ABCDEFu);
    CHECK_EQ(ft.dwHighDateTime, 0x01234567u);
    CHECK_EQ(platform::fileTimeToUint64(ft), value);

    // The other direction.
    FILETIME raw{};
    raw.dwLowDateTime = 0xFFFFFFFFu;
    raw.dwHighDateTime = 0x7FFFFFFFu;
    CHECK_EQ(platform::fileTimeToUint64(raw), 0x7FFFFFFFFFFFFFFFull);
    const FILETIME back = platform::uint64ToFileTime(platform::fileTimeToUint64(raw));
    CHECK_EQ(back.dwLowDateTime, raw.dwLowDateTime);
    CHECK_EQ(back.dwHighDateTime, raw.dwHighDateTime);

    // Edge values.
    for (const uint64_t v : {0ull, 1ull, 0xFFFFFFFFull, 0x100000000ull, platform::nowUtc()}) {
        CHECK_EQ(platform::fileTimeToUint64(platform::uint64ToFileTime(v)), v);
    }
}

/**
 * @brief utcToUnixMs and unixMsToUtc round-trip a 2026 instant, agree on the
 *        epoch offset and clamp values before 1601.
 */
HH_TEST(Time_unixMsRoundTrip) {
    const int64_t unixMs = 1789432106000;
    const uint64_t utc = platform::unixMsToUtc(unixMs);
    CHECK_EQ(platform::utcToUnixMs(utc), unixMs);
    // Ticks are 100 ns: (ms + epoch offset) * 10 000.
    CHECK_EQ(utc, (static_cast<uint64_t>(unixMs) + 11644473600000ull) * 10000ull);

    // The Unix epoch and the FILETIME epoch.
    CHECK_EQ(platform::utcToUnixMs(116444736000000000ull), 0);
    CHECK_EQ(platform::unixMsToUtc(0), 116444736000000000ull);
    CHECK_EQ(platform::utcToUnixMs(0), -11644473600000);

    // Before 1601 there is nothing to represent: clamped to zero.
    CHECK_EQ(platform::unixMsToUtc(-11644473600000 - 1), 0u);
    CHECK_EQ(platform::unixMsToUtc(-11644473600000), 0u);

    // Sub-millisecond ticks are truncated, not rounded.
    CHECK_EQ(platform::utcToUnixMs(utc + 9999), unixMs);
    CHECK_EQ(platform::utcToUnixMs(utc + 10000), unixMs + 1);

    // "Now" survives the trip to the millisecond.
    const uint64_t now = platform::nowUtc();
    const int64_t nowMs = platform::utcToUnixMs(now);
    CHECK_EQ(platform::utcToUnixMs(platform::unixMsToUtc(nowMs)), nowMs);
    CHECK(nowMs > 0);
}

// ---- monotonic clock ---------------------------------------------------------------

/**
 * @brief nowMonotonicMs never goes backwards and advances while we sleep;
 *        nowMonotonicSeconds is the same clock in seconds.
 */
HH_TEST(Time_monotonicClockNeverGoesBackwards) {
    const uint64_t a = platform::nowMonotonicMs();
    const uint64_t b = platform::nowMonotonicMs();
    CHECK(b >= a);

    // A short sleep must show up as elapsed time.
    ::Sleep(15);
    const uint64_t c = platform::nowMonotonicMs();
    CHECK(c >= b);
    CHECK(c - a >= 5);

    // The seconds clock agrees with the milliseconds clock.
    const double s1 = platform::nowMonotonicSeconds();
    const double s2 = platform::nowMonotonicSeconds();
    CHECK(s1 > 0.0);
    CHECK(s2 >= s1);
    CHECK_NEAR(s2, static_cast<double>(platform::nowMonotonicMs()) / 1000.0, 0.5);
}
