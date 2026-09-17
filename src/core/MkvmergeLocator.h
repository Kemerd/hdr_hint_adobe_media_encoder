// ---------------------------------------------------------------------------
// MkvmergeLocator.h - finds and version-checks mkvmerge.exe.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <string>

namespace hh {

struct MkvmergeInfo {
    std::wstring path;             ///< full path (empty when not found)
    int major = 0;
    int minor = 0;
    std::wstring versionLine;      ///< "mkvmerge v82.0 ('I'm The President') 64-bit"
    bool found = false;
    bool ok = false;               ///< found and major >= minMajor
    bool supportsUiLanguage = false;///< "--ui-language en" accepted
    std::wstring error;            ///< why not ok

    /// "v82.0" or "not found".
    [[nodiscard]] std::wstring shortVersion() const;
};

/**
 * @brief Search order: configured path, %ProgramFiles%\MKVToolNix,
 *        %ProgramFiles(x86)%\MKVToolNix, PATH, <exe>\mkvtoolnix.
 *        The bundled v9.3.1 in the repo is deliberately never used.
 */
MkvmergeInfo locateMkvmerge(const std::wstring& configuredPath, int minMajorVersion);

/// Parses "mkvmerge v82.0 (...)" into major/minor. False when it does not match.
bool parseMkvmergeVersion(std::wstring_view line, int& major, int& minor);

} // namespace hh
