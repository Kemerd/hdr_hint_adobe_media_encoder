// ---------------------------------------------------------------------------
// Time.h - clocks and time formatting.
//
// "Utc" values throughout the project are FILETIME-style uint64 (100 ns ticks
// since 1601-01-01 UTC). Monotonic time is milliseconds from QPC.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <cstdint>
#include <optional>
#include <string>

namespace hh::platform {

/// Current wall-clock time as FILETIME uint64.
uint64_t nowUtc();
/// Monotonic milliseconds (QueryPerformanceCounter based).
uint64_t nowMonotonicMs();
/// Monotonic seconds as double (for animation clocks).
double nowMonotonicSeconds();

/// FILETIME <-> uint64 helpers.
uint64_t fileTimeToUint64(const FILETIME& ft);
FILETIME uint64ToFileTime(uint64_t v);

/// FILETIME uint64 -> Unix milliseconds (and back).
int64_t utcToUnixMs(uint64_t utc);
uint64_t unixMsToUtc(int64_t unixMs);

/// "2026-09-16 14:25:06.123 -07:00" in local time.
std::wstring formatLocalIso(uint64_t utc, bool withMillis = true);
/// "2026-09-16T21:25:06Z".
std::wstring formatUtcIso(uint64_t utc);
/// Short human form for the UI ("Today 14:25", "Sep 16 14:25").
std::wstring formatFriendly(uint64_t utc);
/// "00:12:34" from milliseconds.
std::wstring formatDuration(uint64_t millis);

/**
 * @brief Parses AME's locale-formatted log timestamp into UTC.
 *
 * Accepts "MM/DD/YYYY hh:mm:ss AM", "M/D/YYYY H:mm:ss", "DD.MM.YYYY HH:mm:ss",
 * "YYYY-MM-DD HH:mm:ss" and "D/M/YYYY H:mm:ss" (with @p preferDayFirst
 * deciding ambiguous slash dates). The text is interpreted as local time.
 */
std::optional<uint64_t> parseAmeTimestamp(std::wstring_view text, bool preferDayFirst = false);

/// Parses "HH:MM:SS" (AME "Encoding Time") into milliseconds.
std::optional<uint64_t> parseHms(std::wstring_view text);

} // namespace hh::platform
