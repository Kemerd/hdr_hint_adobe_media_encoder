// ---------------------------------------------------------------------------
// AmeLogLocator.cpp - finds AME's encoding logs on this machine.
//
// Adobe Media Encoder keeps one folder per major version under Documents:
//
//   <Documents>\Adobe\Adobe Media Encoder\<ver>\AMEEncodingLog.txt   (2019+)
//   <Documents>\Adobe Media Encoder\<ver>\AMEEncodingLog.txt         (legacy)
//
// where <ver> is "26.0", "25.0", "2020" and so on: digits and dots only.
// Failed items are additionally written to AMEEncodingErrorLog.txt in the
// same folder, so every main log gets its error companion listed right
// after it. User overrides come first and are kept even when the file does
// not exist yet, because the tailer watches their folders for creation.
// ---------------------------------------------------------------------------
#include "core/AmeLogLocator.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/Utf.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hh {

namespace {

/// Component tag for every log line in this file.
constexpr const wchar_t* kLog = L"LogLocator";

/// File names AME uses; compared case-insensitively.
constexpr std::wstring_view kMainLogName = L"AMEEncodingLog.txt";
constexpr std::wstring_view kErrorLogName = L"AMEEncodingErrorLog.txt";

/// Sanity caps so a hostile config or a junk-filled Documents folder cannot
/// turn discovery into a long stall.
constexpr size_t kMaxOverrides = 32;
constexpr size_t kMaxVersionDirs = 64;

/// A main log and its optional error companion, kept together for sorting.
struct LogGroup {
    LogCandidate main;
    LogCandidate error;
    bool hasMain = false;
    bool hasError = false;
    uint64_t sortKey = 0;   ///< newest last-write of the pair
};

/**
 * @brief True when a folder name looks like an AME version ("26.0", "2020").
 *
 * Only digits and dots are allowed and at least one digit must be present,
 * so "." or ".." style junk never qualifies.
 */
[[nodiscard]] bool isVersionName(std::wstring_view name) noexcept {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    bool sawDigit = false;
    for (const wchar_t c : name) {
        if (c >= L'0' && c <= L'9') {
            sawDigit = true;
        } else if (c != L'.') {
            return false;
        }
    }
    return sawDigit;
}

/**
 * @brief Case-insensitive "is this path already listed?" check.
 */
[[nodiscard]] bool containsPath(const std::vector<LogCandidate>& list, std::wstring_view path) {
    for (const LogCandidate& c : list) {
        if (platform::iequals(c.path, path)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Builds a candidate for @p path, stat'ing it for existence and time.
 *
 * Missing files still produce a candidate (exists=false) so overrides can be
 * watched for creation; callers decide whether to keep those.
 */
[[nodiscard]] LogCandidate makeCandidate(std::wstring path, std::wstring version, bool isErrorLog) {
    LogCandidate c;
    c.path = std::move(path);
    c.directory = path::parent(c.path);
    c.version = std::move(version);
    c.isErrorLog = isErrorLog;

    // Only a regular file counts as "exists"; a directory of the same name
    // would make the tailer's CreateFileW fail forever.
    c.exists = platform::isFile(c.path);
    if (c.exists) {
        const auto lw = platform::lastWriteUtc(c.path);
        if (lw) {
            c.lastWriteUtc = lw.value();
        } else {
            HH_LOG_DEBUG(kLog, L"lastWriteUtc failed for '{}': {}", c.path, lw.error().toString());
        }
    }
    return c;
}

/**
 * @brief Turns a raw override into an absolute log file path.
 *
 * A folder override is accepted too and resolved to its AMEEncodingLog.txt,
 * which is what people tend to paste from Explorer. Empty on junk input.
 */
[[nodiscard]] std::wstring normalizeOverride(std::wstring_view raw) {
    const std::wstring_view trimmed = platform::trim(raw);
    if (trimmed.empty()) {
        return {};
    }

    // GetFullPathNameW resolves relative pieces; fall back to a separator
    // clean-up when it refuses the input (e.g. reserved names).
    std::wstring full = platform::fullPath(trimmed);
    if (full.empty()) {
        full = path::normalizeSeparators(trimmed);
    }
    if (full.empty()) {
        return {};
    }

    // Strip a trailing separator so parent()/fileName() behave.
    while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) {
        full.pop_back();
    }

    // A directory override means "the main log inside that folder".
    if (platform::isDirectory(full)) {
        return path::join(full, kMainLogName);
    }
    return full;
}

/**
 * @brief Appends the user overrides (in the given order) to @p out.
 *
 * Every override is kept even when missing. A main-log override also brings
 * its error companion along when that one is present on disk.
 */
void collectOverrides(const std::vector<std::wstring>& overrides, std::vector<LogCandidate>& out) {
    size_t accepted = 0;
    for (const std::wstring& raw : overrides) {
        if (accepted >= kMaxOverrides) {
            HH_LOG_WARN(kLog, L"more than {} log overrides configured; ignoring the rest", kMaxOverrides);
            break;
        }
        const std::wstring full = normalizeOverride(raw);
        if (full.empty()) {
            HH_LOG_DEBUG(kLog, L"ignoring empty/invalid log override '{}'", raw);
            continue;
        }
        // The same path twice is listed once.
        if (containsPath(out, full)) {
            HH_LOG_DEBUG(kLog, L"duplicate log override '{}' skipped", full);
            continue;
        }

        // Classify by file name; anything not named like the error log is
        // treated as a main log so custom file names still work.
        const std::wstring name = path::fileName(full);
        const bool isError = platform::iequals(name, kErrorLogName);
        LogCandidate main = makeCandidate(full, std::wstring(), isError);
        HH_LOG_DEBUG(kLog, L"override '{}' ({}, {})", main.path, isError ? L"error log" : L"main log",
                     main.exists ? L"present" : L"missing");
        out.push_back(std::move(main));
        ++accepted;

        // A main log named the standard way gets its sibling error log too.
        if (!isError && platform::iequals(name, kMainLogName)) {
            const std::wstring sibling = path::join(path::parent(full), kErrorLogName);
            if (!containsPath(out, sibling) && platform::isFile(sibling)) {
                out.push_back(makeCandidate(sibling, std::wstring(), true));
            }
        }
    }
}

/**
 * @brief Scans one root ("...\Adobe Media Encoder") for version folders and
 *        collects the logs inside them as groups.
 * @param taken paths that are already listed (overrides), skipped here
 */
void scanRoot(const std::wstring& root, const std::vector<LogCandidate>& taken, std::vector<LogGroup>& groups) {
    if (root.empty()) {
        return;
    }
    // Most machines only have one of the two roots; a missing one is normal.
    if (!platform::isDirectory(root)) {
        HH_LOG_DEBUG(kLog, L"root '{}' not present", root);
        return;
    }

    const auto listing = platform::listDirectory(root);
    if (!listing) {
        HH_LOG_WARN(kLog, L"cannot list '{}': {}", root, listing.error().toString());
        return;
    }

    // Walk the version folders; anything else in the root is ignored.
    size_t seen = 0;
    for (const platform::DirEntry& entry : listing.value()) {
        if (!entry.isDirectory || !isVersionName(entry.name)) {
            continue;
        }
        if (seen >= kMaxVersionDirs) {
            HH_LOG_WARN(kLog, L"more than {} version folders under '{}'; ignoring the rest", kMaxVersionDirs, root);
            break;
        }
        ++seen;

        const std::wstring dir = path::join(root, entry.name);
        LogGroup group;

        // Main log first; only existing files are listed for discovered folders.
        const std::wstring mainPath = path::join(dir, kMainLogName);
        if (!containsPath(taken, mainPath) && platform::isFile(mainPath)) {
            group.main = makeCandidate(mainPath, entry.name, false);
            group.hasMain = true;
            group.sortKey = group.main.lastWriteUtc;
        }

        // Error companion, same rules.
        const std::wstring errorPath = path::join(dir, kErrorLogName);
        if (!containsPath(taken, errorPath) && platform::isFile(errorPath)) {
            group.error = makeCandidate(errorPath, entry.name, true);
            group.hasError = true;
            // A folder with only an error log sorts by that file's time.
            if (!group.hasMain) {
                group.sortKey = group.error.lastWriteUtc;
            }
        }

        if (group.hasMain || group.hasError) {
            groups.push_back(std::move(group));
        } else {
            HH_LOG_DEBUG(kLog, L"version folder '{}' has no logs", dir);
        }
    }
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

/**
 * @brief Lists every AME log worth tailing: overrides first (in order), then
 *        the discovered logs newest-first, each followed by its error log.
 */
std::vector<LogCandidate> discoverAmeLogs(const std::vector<std::wstring>& overrides) {
    std::vector<LogCandidate> out;
    out.reserve(overrides.size() * 2 + 8);

    // ---- User overrides lead the list --------------------------------------
    collectOverrides(overrides, out);

    // ---- Standard locations under Documents --------------------------------
    const std::wstring documents = platform::documentsFolder();
    std::vector<LogGroup> groups;
    if (documents.empty()) {
        HH_LOG_WARN(kLog, L"Documents folder unavailable; only overrides are used");
    } else {
        scanRoot(path::join(path::join(documents, L"Adobe"), L"Adobe Media Encoder"), out, groups);
        scanRoot(path::join(documents, L"Adobe Media Encoder"), out, groups);
    }

    // Newest activity first; stable so equal stamps keep directory order.
    std::stable_sort(groups.begin(), groups.end(),
                     [](const LogGroup& a, const LogGroup& b) { return a.sortKey > b.sortKey; });

    // Flatten: main log, then its error companion.
    for (LogGroup& g : groups) {
        if (g.hasMain) {
            out.push_back(std::move(g.main));
        }
        if (g.hasError) {
            out.push_back(std::move(g.error));
        }
    }

    HH_LOG_DEBUG(kLog, L"discovery finished: {} candidate(s), {} override(s)", out.size(), overrides.size());
    return out;
}

/**
 * @brief Picks the existing main log with the newest last-write time.
 *
 * Ties keep the earlier entry, which favours overrides over discovered logs.
 */
const LogCandidate* newestMainLog(const std::vector<LogCandidate>& candidates) {
    const LogCandidate* best = nullptr;
    for (const LogCandidate& c : candidates) {
        if (c.isErrorLog || !c.exists || c.path.empty()) {
            continue;
        }
        if (best == nullptr || c.lastWriteUtc > best->lastWriteUtc) {
            best = &c;
        }
    }
    return best;
}

} // namespace hh
