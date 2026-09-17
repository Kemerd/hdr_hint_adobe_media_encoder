// ---------------------------------------------------------------------------
// MkvmergeLocator.cpp - finds mkvmerge.exe and checks that it is new enough.
//
// The search walks a fixed list of candidate locations, runs "--version" on
// each one that exists and keeps the first candidate whose major version is
// at least the requested minimum. A candidate that is too old is remembered
// as a fallback so the UI can still say "v9.3 is too old" instead of the
// less helpful "not found" when nothing better turns up.
// ---------------------------------------------------------------------------
#include "core/MkvmergeLocator.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/Process.h"
#include "platform/Utf.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace hh {

namespace {

/// Component tag used for every log line in this file.
constexpr const wchar_t* kLog = L"MkvmergeLocator";

/// How long "--version" may take before we give up on a candidate.
constexpr DWORD kVersionProbeTimeoutMs = 10000;

/// Upper bound on the digits we accept for a version component (avoids overflow).
constexpr size_t kMaxVersionDigits = 6;

/**
 * @brief Resolves a bare executable name through the Win32 search path.
 *
 * SearchPathW with a null path argument looks in the application directory,
 * the current directory, the system directories and finally every PATH entry.
 * The buffer is grown once when the API reports that it was too small.
 *
 * @param name  executable name without extension ("mkvmerge")
 * @param ext   extension including the dot (".exe")
 * @return full path, or empty when nothing was found
 */
std::wstring searchPathFor(const wchar_t* name, const wchar_t* ext) {
    // Guard the inputs; a null name would make SearchPathW fail anyway.
    if (name == nullptr || *name == L'\0') {
        return {};
    }

    // Start with a MAX_PATH buffer and grow once if the API asks for more.
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD needed = ::SearchPathW(nullptr, name, ext, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (needed == 0) {
            // Not found (or an API failure); either way there is nothing usable.
            return {};
        }
        if (needed < buffer.size()) {
            // Success: 'needed' is the length without the terminator.
            return std::wstring(buffer.data(), needed);
        }
        // The buffer was too small; 'needed' includes the terminator this time.
        buffer.assign(static_cast<size_t>(needed) + 1u, L'\0');
    }
    return {};
}

/**
 * @brief Extracts the line that carries "mkvmerge v" from captured output.
 *
 * The version banner is normally the first line, but we scan every line so a
 * stray warning printed before it does not break detection.
 */
std::wstring findVersionLine(const std::string& utf8Output) {
    // Convert once; the output is tiny (one or two lines).
    const std::wstring wide = platform::toWide(utf8Output);

    // Walk the text line by line, tolerating both \r\n and \n endings.
    size_t pos = 0;
    while (pos < wide.size()) {
        size_t end = wide.find_first_of(L"\r\n", pos);
        if (end == std::wstring::npos) {
            end = wide.size();
        }
        const std::wstring_view line = platform::trim(std::wstring_view(wide).substr(pos, end - pos));
        if (platform::ifind(line, L"mkvmerge v") != std::wstring::npos) {
            return std::wstring(line);
        }
        pos = end + 1;
    }
    return {};
}

/**
 * @brief Runs "<candidate> --version" and fills the version fields of @p info.
 * @return true when the banner parsed and the candidate is usable at all
 */
bool probeVersion(const std::wstring& candidate, MkvmergeInfo& info) {
    // Run the executable with a short timeout; a hung candidate is skipped.
    auto capture = platform::runCapture(candidate, {L"--version"}, kVersionProbeTimeoutMs);
    if (!capture) {
        HH_LOG_WARN(kLog, L"could not run '{}' --version: {}", candidate, capture.error().toString());
        return false;
    }
    if (capture.value().timedOut) {
        HH_LOG_WARN(kLog, L"'{}' --version timed out after {} ms", candidate, kVersionProbeTimeoutMs);
        return false;
    }

    // Pull the banner line out of the captured text and parse it.
    const std::wstring line = findVersionLine(capture.value().output);
    if (line.empty()) {
        HH_LOG_WARN(kLog, L"'{}' --version printed no recognisable banner (exit {})", candidate, capture.value().exitCode);
        return false;
    }

    int major = 0;
    int minor = 0;
    if (!parseMkvmergeVersion(line, major, minor)) {
        HH_LOG_WARN(kLog, L"could not parse version from '{}'", line);
        return false;
    }

    // Record what we learned; 'ok' is decided by the caller against minMajor.
    info.path = candidate;
    info.major = major;
    info.minor = minor;
    info.versionLine = line;
    info.found = true;
    return true;
}

/**
 * @brief Checks whether "--ui-language en" is accepted by this build.
 *
 * Older builds without the English catalogue exit non-zero, in which case the
 * runner simply omits the flag (the #GUI# markers are stable either way).
 */
bool probeUiLanguage(const std::wstring& candidate) {
    auto capture = platform::runCapture(candidate, {L"--ui-language", L"en", L"--version"}, kVersionProbeTimeoutMs);
    if (!capture) {
        HH_LOG_WARN(kLog, L"ui-language probe failed to launch: {}", capture.error().toString());
        return false;
    }
    if (capture.value().timedOut) {
        HH_LOG_WARN(kLog, L"ui-language probe timed out");
        return false;
    }
    return capture.value().exitCode == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// MkvmergeInfo
// ---------------------------------------------------------------------------

std::wstring MkvmergeInfo::shortVersion() const {
    // Without a located binary there is no version to show.
    if (!found) {
        return L"not found";
    }
    return std::format(L"v{}.{}", major, minor);
}

// ---------------------------------------------------------------------------
// parseMkvmergeVersion
// ---------------------------------------------------------------------------

bool parseMkvmergeVersion(std::wstring_view line, int& major, int& minor) {
    // Defaults so a failed parse never leaves stale values behind.
    major = 0;
    minor = 0;

    // Locate the "mkvmerge v" marker (case-insensitive to be forgiving).
    constexpr std::wstring_view kMarker = L"mkvmerge v";
    const size_t at = platform::ifind(line, kMarker);
    if (at == std::wstring::npos) {
        return false;
    }
    size_t pos = at + kMarker.size();

    // Hand-rolled digit scanner for the major component.
    auto scanNumber = [&](int& out) -> bool {
        size_t digits = 0;
        long long value = 0;
        while (pos < line.size() && line[pos] >= L'0' && line[pos] <= L'9') {
            if (digits >= kMaxVersionDigits) {
                return false;   // absurdly long number: not a version
            }
            value = value * 10 + (line[pos] - L'0');
            ++pos;
            ++digits;
        }
        if (digits == 0) {
            return false;
        }
        out = static_cast<int>(value);
        return true;
    };

    // "82" then "." then "0"; a missing minor is treated as a parse failure
    // because every real mkvmerge banner has one.
    int parsedMajor = 0;
    if (!scanNumber(parsedMajor)) {
        return false;
    }
    if (pos >= line.size() || line[pos] != L'.') {
        return false;
    }
    ++pos;
    int parsedMinor = 0;
    if (!scanNumber(parsedMinor)) {
        return false;
    }

    major = parsedMajor;
    minor = parsedMinor;
    return true;
}

// ---------------------------------------------------------------------------
// locateMkvmerge
// ---------------------------------------------------------------------------

MkvmergeInfo locateMkvmerge(const std::wstring& configuredPath, int minMajorVersion) {
    // Build the candidate list in the documented priority order.
    std::vector<std::wstring> candidates;
    auto addCandidate = [&](std::wstring path) {
        const std::wstring_view trimmed = platform::trim(path);
        if (trimmed.empty()) {
            return;
        }
        std::wstring clean(trimmed);
        // Skip duplicates (same path reached through two sources, e.g. PATH).
        const std::wstring key = path::normalizeKey(clean);
        for (const auto& existing : candidates) {
            if (path::normalizeKey(existing) == key) {
                return;
            }
        }
        candidates.push_back(std::move(clean));
    };

    // 1. The explicitly configured path (may point at a folder or the exe).
    if (!platform::trim(configuredPath).empty()) {
        std::wstring configured(platform::trim(configuredPath));
        if (platform::isDirectory(configured)) {
            configured = path::join(configured, L"mkvmerge.exe");
        }
        addCandidate(configured);
    }

    // 2./3. The standard MKVToolNix install locations.
    const std::wstring pf64 = platform::programFilesX64Folder();
    if (!pf64.empty()) {
        addCandidate(path::join(path::join(pf64, L"MKVToolNix"), L"mkvmerge.exe"));
    }
    const std::wstring pf86 = platform::programFilesX86Folder();
    if (!pf86.empty()) {
        addCandidate(path::join(path::join(pf86, L"MKVToolNix"), L"mkvmerge.exe"));
    }

    // 4. Anything on PATH.
    addCandidate(searchPathFor(L"mkvmerge", L".exe"));

    // 5. A copy shipped next to our own executable.
    const std::wstring exeDir = platform::exeDirectory();
    if (!exeDir.empty()) {
        addCandidate(path::join(path::join(exeDir, L"mkvtoolnix"), L"mkvmerge.exe"));
    }

    // Probe each candidate in order; the first acceptable one wins.
    MkvmergeInfo fallback;     // first candidate that ran but is too old
    for (const auto& candidate : candidates) {
        if (!platform::isFile(candidate)) {
            HH_LOG_DEBUG(kLog, L"candidate not present: {}", candidate);
            continue;
        }

        MkvmergeInfo probe;
        if (!probeVersion(candidate, probe)) {
            continue;
        }
        HH_LOG_INFO(kLog, L"found {} ({})", probe.versionLine, candidate);

        // Too old: remember it and keep looking for something newer.
        if (probe.major < minMajorVersion) {
            probe.ok = false;
            probe.error = std::format(L"mkvmerge v{}.{} is too old, need v{}+", probe.major, probe.minor, minMajorVersion);
            if (!fallback.found) {
                fallback = probe;
            }
            continue;
        }

        // Acceptable: finish with the ui-language capability probe.
        probe.ok = true;
        probe.supportsUiLanguage = probeUiLanguage(candidate);
        HH_LOG_INFO(kLog, L"using {} (ui-language en {})", candidate, probe.supportsUiLanguage ? L"supported" : L"not supported");
        return probe;
    }

    // Nothing new enough: report the old one if we saw one, else "not found".
    if (fallback.found) {
        HH_LOG_WARN(kLog, L"{}", fallback.error);
        return fallback;
    }

    MkvmergeInfo none;
    none.found = false;
    none.ok = false;
    none.error = L"mkvmerge not found";
    HH_LOG_WARN(kLog, L"mkvmerge not found in any of {} candidate locations", candidates.size());
    return none;
}

} // namespace hh
