// ---------------------------------------------------------------------------
// AmeLogLocator.h - finds every AMEEncodingLog.txt on this machine.
//
//   <Documents>\Adobe\Adobe Media Encoder\<ver>\AMEEncodingLog.txt   (AME 2019+)
//   <Documents>\Adobe Media Encoder\<ver>\AMEEncodingLog.txt         (legacy)
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hh {

struct LogCandidate {
    std::wstring path;          ///< full path of the log file
    std::wstring directory;     ///< its folder (watched for changes)
    std::wstring version;       ///< "26.0" etc. (empty for overrides)
    uint64_t lastWriteUtc = 0;
    bool exists = false;
    bool isErrorLog = false;    ///< AMEEncodingErrorLog.txt companion
};

/**
 * @brief Discovers log files. Overrides are listed first and always kept even
 *        when missing (they are watched for creation).
 */
std::vector<LogCandidate> discoverAmeLogs(const std::vector<std::wstring>& overrides);

/// The candidate with the newest last-write time (main logs only), if any.
const LogCandidate* newestMainLog(const std::vector<LogCandidate>& candidates);

} // namespace hh
