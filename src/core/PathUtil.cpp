// ---------------------------------------------------------------------------
// PathUtil.cpp - path rules specific to HdrHint (keys, suffixes, AME temp names).
//
// Pure string work; the only file-system calls are in normalizeKey
// (GetFullPathName / its POSIX twin) and firstFreePath (existence probes).
//
// Windows paths accept both separators and know drive letters and UNC
// roots. POSIX paths have exactly one separator ('/') and one root ("/");
// a backslash there is an ordinary file-name character.
// ---------------------------------------------------------------------------
#include "core/PathUtil.h"

#include "platform/FileIo.h"
#include "platform/Utf.h"

#include <cwctype>
#include <format>
#include <string>

namespace hh::path {

namespace {

/// Windows: both separators count everywhere in this file. POSIX: only '/'.
bool isSep(wchar_t c) noexcept {
#if defined(_WIN32)
    return c == L'\\' || c == L'/';
#else
    return c == L'/';
#endif
}

#if defined(_WIN32)
/**
 * @brief Length of a UNC root ("\\server\share\") starting the scan at @p start
 *        (just past the leading "\\" or "\\?\UNC\"). The trailing separator
 *        belongs to the root when present.
 */
size_t uncRootLength(std::wstring_view p, size_t start) noexcept {
    const size_t n = p.size();
    size_t i = start;
    // Server name.
    while (i < n && !isSep(p[i])) {
        ++i;
    }
    if (i >= n) {
        return n;
    }
    ++i;  // separator between server and share
    // Share name.
    while (i < n && !isSep(p[i])) {
        ++i;
    }
    if (i < n && isSep(p[i])) {
        ++i;  // the root owns its trailing separator
    }
    return i;
}
#endif

/**
 * @brief Length of the root part of a path including its trailing separator:
 *        "C:\" -> 3, "C:" -> 2, "\\server\share\" -> full, "\\?\C:\" -> 7,
 *        "\x" -> 1, "clip.mp4" -> 0.
 */
size_t rootLength(std::wstring_view p) noexcept {
    const size_t n = p.size();
#if !defined(_WIN32)
    // POSIX has a single root.
    return (n >= 1 && p[0] == L'/') ? 1 : 0;
#else

    // UNC / device prefixes.
    if (n >= 2 && isSep(p[0]) && isSep(p[1])) {
        size_t start = 2;
        if (n >= 4 && (p[2] == L'?' || p[2] == L'.') && isSep(p[3])) {
            start = 4;
            // "\\?\UNC\server\share\"
            if (n >= 8 && (p[4] == L'U' || p[4] == L'u') && (p[5] == L'N' || p[5] == L'n') &&
                (p[6] == L'C' || p[6] == L'c') && isSep(p[7])) {
                return uncRootLength(p, 8);
            }
            // "\\?\C:\"
            if (n >= start + 2 && p[start + 1] == L':') {
                return (n >= start + 3 && isSep(p[start + 2])) ? start + 3 : start + 2;
            }
            return start;
        }
        return uncRootLength(p, start);
    }

    // Drive letter.
    if (n >= 2 && p[1] == L':' && std::iswalpha(static_cast<wint_t>(p[0]))) {
        return (n >= 3 && isSep(p[2])) ? 3 : 2;
    }

    // Rooted on the current drive.
    if (n >= 1 && isSep(p[0])) {
        return 1;
    }
    return 0;
#endif
}

/// Index of the last '.' that starts an extension in a *file name*, or npos.
size_t extensionDot(std::wstring_view name) noexcept {
    const size_t dot = name.find_last_of(L'.');
    // A leading dot (".gitignore") or "." / ".." is not an extension.
    if (dot == std::wstring_view::npos || dot == 0) {
        return std::wstring_view::npos;
    }
    if (name == L".." ) {
        return std::wstring_view::npos;
    }
    return dot;
}

/// Parses 1..10 decimal digits into a DWORD; nullopt for anything else.
std::optional<DWORD> parseDigits(std::wstring_view s) noexcept {
    if (s.empty() || s.size() > 10) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<uint64_t>(c - L'0');
        if (value > 0xFFFFFFFFull) {
            return std::nullopt;
        }
    }
    return static_cast<DWORD>(value);
}

bool isHighSurrogate(wchar_t c) noexcept { return c >= 0xD800 && c <= 0xDBFF; }
bool isLowSurrogate(wchar_t c) noexcept { return c >= 0xDC00 && c <= 0xDFFF; }

} // namespace

// ---------------------------------------------------------------------------
// Keys and components
// ---------------------------------------------------------------------------

/**
 * @brief Absolute path, no \\?\ prefix, no trailing separator, upper-cased.
 */
std::wstring normalizeKey(std::wstring_view path) {
    const std::wstring_view t = platform::trim(path);
    if (t.empty()) {
        return {};
    }

    // Absolute form first (resolves ".." and relative pieces).
    std::wstring full = platform::fullPath(t);
    if (full.empty()) {
        full = normalizeSeparators(t);
    }

#if defined(_WIN32)
    // Drop any extended-length prefix so keys compare regardless of how the
    // path was handed to us.
    if (platform::istartsWith(full, L"\\\\?\\UNC\\")) {
        full = L"\\\\" + full.substr(8);
    } else if (platform::istartsWith(full, L"\\\\?\\")) {
        full = full.substr(4);
    }
#else
    // The file system may hand back decomposed names; AME's log uses composed ones.
    full = platform::normalizeNfc(full);
#endif

    // Trailing separators beyond the root are noise.
    const size_t root = rootLength(full);
    while (full.size() > root && isSep(full.back())) {
        full.pop_back();
    }

    return platform::toUpperInvariant(full);
}

/**
 * @brief Last path component.
 */
std::wstring fileName(std::wstring_view path) {
#if defined(_WIN32)
    const size_t pos = path.find_last_of(L"\\/");
#else
    const size_t pos = path.find_last_of(L'/');
#endif
    if (pos == std::wstring_view::npos) {
        return std::wstring(path);
    }
    return std::wstring(path.substr(pos + 1));
}

/**
 * @brief File name without its last extension.
 */
std::wstring stem(std::wstring_view path) {
    const std::wstring name = fileName(path);
    const size_t dot = extensionDot(name);
    if (dot == std::wstring_view::npos) {
        return name;
    }
    return name.substr(0, dot);
}

/**
 * @brief Lower-cased extension with the dot, or empty.
 */
std::wstring extension(std::wstring_view path) {
    const std::wstring name = fileName(path);
    const size_t dot = extensionDot(name);
    if (dot == std::wstring_view::npos) {
        return {};
    }
    return platform::toLowerInvariant(std::wstring_view(name).substr(dot));
}

/**
 * @brief Parent directory. Roots are returned unchanged ("C:\" stays "C:\"),
 *        a bare file name has an empty parent.
 */
std::wstring parent(std::wstring_view path) {
    if (path.empty()) {
        return {};
    }
    const size_t root = rootLength(path);

    // Ignore trailing separators ("C:\a\b\" is the directory b).
    size_t end = path.size();
    while (end > root && isSep(path[end - 1])) {
        --end;
    }
    if (end <= root) {
        return std::wstring(path.substr(0, root));
    }

    // Find the separator before the last component.
    size_t pos = end;
    while (pos > root && !isSep(path[pos - 1])) {
        --pos;
    }
    if (pos <= root) {
        return std::wstring(path.substr(0, root));
    }

    // Strip that separator (and any duplicates) but never eat into the root.
    size_t cut = pos;
    while (cut > root && isSep(path[cut - 1])) {
        --cut;
    }
    if (cut <= root) {
        return std::wstring(path.substr(0, root));
    }
    return std::wstring(path.substr(0, cut));
}

/**
 * @brief dir + '\' + name with exactly one separator between them.
 */
std::wstring join(std::wstring_view dir, std::wstring_view name) {
    // Leading separators on the name would otherwise double up.
    while (!name.empty() && isSep(name.front())) {
        name.remove_prefix(1);
    }
    if (dir.empty()) {
        return std::wstring(name);
    }
    if (name.empty()) {
        return std::wstring(dir);
    }

    std::wstring out(dir);
    const size_t root = rootLength(out);
    while (out.size() > root && isSep(out.back())) {
        out.pop_back();
    }
    if (out.empty() || !isSep(out.back())) {
        out += kSeparator;
    }
    out.append(name);
    return out;
}

/**
 * @brief '/' -> '\' and duplicate separators collapsed; a leading "\\"
 *        (UNC or \\?\ prefix) is kept intact.
 */
std::wstring normalizeSeparators(std::wstring_view path) {
    std::wstring out;
    out.reserve(path.size());
#if !defined(_WIN32)
    // POSIX: collapse runs of '/' and leave every other character alone.
    for (const wchar_t c : path) {
        if (c == L'/' && !out.empty() && out.back() == L'/') {
            continue;
        }
        out += c;
    }
    return out;
#else
    size_t i = 0;
    if (path.size() >= 2 && isSep(path[0]) && isSep(path[1])) {
        out += L"\\\\";
        i = 2;
    }
    for (; i < path.size(); ++i) {
        const wchar_t c = path[i];
        if (isSep(c)) {
            if (out.empty() || out.back() != L'\\') {
                out += L'\\';
            }
        } else {
            out += c;
        }
    }
    return out;
#endif
}

/**
 * @brief True when the path's extension is in @p list (case-insensitive;
 *        entries without a leading dot are tolerated).
 */
bool hasExtension(std::wstring_view path, const std::vector<std::wstring>& list) {
    const std::wstring ext = extension(path);
    if (ext.empty() || list.empty()) {
        return false;
    }
    for (const std::wstring& item : list) {
        const std::wstring_view t = platform::trim(item);
        if (t.empty()) {
            continue;
        }
        if (t.front() == L'.') {
            if (platform::iequals(ext, t)) {
                return true;
            }
        } else if (platform::iequals(std::wstring_view(ext).substr(1), t)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Hint file naming
// ---------------------------------------------------------------------------

/**
 * @brief "<folder>\<stem><suffix>.mkv"; a stem already ending in the suffix
 *        is not doubled.
 */
std::wstring hintPathFor(std::wstring_view sourcePath, std::wstring_view suffix, std::wstring_view outputFolder) {
    if (platform::trim(sourcePath).empty()) {
        return {};
    }

    const std::wstring folder = platform::trim(outputFolder).empty() ? parent(sourcePath)
                                                                     : std::wstring(platform::trim(outputFolder));
    std::wstring s = stem(sourcePath);
    if (s.empty()) {
        s = L"output";
    }
    if (suffix.empty() || !platform::iendsWith(s, suffix)) {
        s.append(suffix);
    }
    return join(folder, s + L".mkv");
}

/**
 * @brief The path itself when free, otherwise " (2)", " (3)", ... before the
 *        extension. Gives up (returns the last candidate) after 999.
 */
std::wstring firstFreePath(std::wstring_view path) {
    if (path.empty()) {
        return {};
    }
    if (!platform::exists(path)) {
        return std::wstring(path);
    }

    // Split around the extension of the last component (original casing kept).
    const std::wstring name = fileName(path);
    const size_t dot = extensionDot(name);
    const size_t extLen = (dot == std::wstring_view::npos) ? 0 : name.size() - dot;
    if (extLen > path.size()) {
        return std::wstring(path);
    }
    const std::wstring_view head = path.substr(0, path.size() - extLen);
    const std::wstring_view tail = path.substr(path.size() - extLen);

    std::wstring candidate;
    for (int n = 2; n <= 999; ++n) {
        candidate = std::format(L"{} ({}){}", head, n, tail);
        if (!platform::exists(candidate)) {
            return candidate;
        }
    }
    return candidate;
}

/**
 * @brief ".mkv" whose stem ends with the suffix (and is not a partial).
 */
bool isOurOutput(std::wstring_view fileName_, std::wstring_view suffix) {
    if (fileName_.empty()) {
        return false;
    }
    if (isPartialOutput(fileName_)) {
        return false;
    }
    if (extension(fileName_) != L".mkv") {
        return false;
    }
    return platform::iendsWith(stem(fileName_), suffix);
}

/**
 * @brief "*.hdrhint-partial.mkv"
 */
bool isPartialOutput(std::wstring_view fileName_) {
    return platform::iendsWith(fileName_, L".hdrhint-partial.mkv");
}

/**
 * @brief "<hint without .mkv>.hdrhint-partial.mkv" - the final ".mkv" is kept
 *        so mkvmerge still picks the Matroska writer for the partial file.
 */
std::wstring partialPathFor(std::wstring_view hintPath) {
    if (hintPath.empty()) {
        return {};
    }
    std::wstring base(hintPath);
    if (platform::iendsWith(base, L".mkv")) {
        base.resize(base.size() - 4);
    }
    return base + L".hdrhint-partial.mkv";
}

// ---------------------------------------------------------------------------
// AME temp names
// ---------------------------------------------------------------------------

/**
 * @brief "<stem>.<pid>.<tid>.<ext>" where ext is a known elementary-stream
 *        extension and both numbers are 1-10 digits.
 */
std::optional<Sidecar> parseSidecar(std::wstring_view fileName_) {
    if (fileName_.empty() || fileName_.size() > 1024) {
        return std::nullopt;
    }
    static constexpr const wchar_t* kKnownExt[] = {
        L"m4v", L"aac", L"m4a", L"h264", L"hevc", L"264", L"265", L"mov", L"wav", L"pcm", L"ac3", L"mp4v",
    };

    // Work backwards: extension, tid, pid; whatever is left is the stem.
    const size_t dotExt = fileName_.find_last_of(L'.');
    if (dotExt == std::wstring_view::npos || dotExt == 0) {
        return std::nullopt;
    }
    const std::wstring_view ext = fileName_.substr(dotExt + 1);
    bool known = false;
    for (const wchar_t* k : kKnownExt) {
        if (platform::iequals(ext, k)) {
            known = true;
            break;
        }
    }
    if (!known) {
        return std::nullopt;
    }

    std::wstring_view rest = fileName_.substr(0, dotExt);
    const size_t dotTid = rest.find_last_of(L'.');
    if (dotTid == std::wstring_view::npos || dotTid == 0) {
        return std::nullopt;
    }
    const std::wstring_view tidText = rest.substr(dotTid + 1);

    rest = rest.substr(0, dotTid);
    const size_t dotPid = rest.find_last_of(L'.');
    if (dotPid == std::wstring_view::npos || dotPid == 0) {
        return std::nullopt;
    }
    const std::wstring_view pidText = rest.substr(dotPid + 1);
    const std::wstring_view stemText = rest.substr(0, dotPid);
    if (platform::trim(stemText).empty()) {
        return std::nullopt;
    }

    // Both groups must be plain decimal numbers that fit a DWORD.
    const std::optional<DWORD> pid = parseDigits(pidText);
    const std::optional<DWORD> tid = parseDigits(tidText);
    if (!pid || !tid) {
        return std::nullopt;
    }

    Sidecar sc;
    sc.stem = std::wstring(stemText);
    sc.pid = *pid;
    sc.tid = *tid;
    sc.ext = platform::toLowerInvariant(ext);
    return sc;
}

/**
 * @brief Scratch / partial download names that must never become jobs.
 *
 * AME's "<8hex>-<4hex>-<4hex>-<4hex>.tmp" scratch files are covered by the
 * ".tmp" rule; Office lock files start with "~$".
 */
bool isTemporaryName(std::wstring_view fileName_) {
    const std::wstring_view name = platform::trim(fileName_);
    if (name.empty()) {
        return false;
    }
    if (platform::istartsWith(name, L"~$")) {
        return true;
    }
    static constexpr const wchar_t* kTempSuffixes[] = {
        L".tmp", L".part", L".partial", L".crdownload", L".download", L".lock",
    };
    for (const wchar_t* s : kTempSuffixes) {
        if (platform::iendsWith(name, s)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Any of \ / : * ? " < > | or a control character.
 */
bool hasInvalidFileNameChars(std::wstring_view name) {
    for (wchar_t c : name) {
        if (c < 32) {
            return true;
        }
        switch (c) {
        case L'\\': case L'/': case L':': case L'*': case L'?': case L'"': case L'<': case L'>': case L'|':
            return true;
        default:
            break;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

/**
 * @brief Middle ellipsis that keeps the extension and roughly two thirds of
 *        the budget at the front: "very_long_file_name.mp4" (15) -> "very_lo…ame.mp4".
 */
std::wstring ellipsizeMiddle(std::wstring_view text, size_t maxChars) {
    if (text.size() <= maxChars) {
        return std::wstring(text);
    }
    if (maxChars == 0) {
        return {};
    }
    constexpr wchar_t kEllipsis = L'\u2026';
    if (maxChars == 1) {
        return std::wstring(1, kEllipsis);
    }

    // Keep a short extension intact when it leaves room for leading chars.
    std::wstring_view ext;
    const size_t dot = text.find_last_of(L'.');
    if (dot != std::wstring_view::npos && dot > 0) {
        const size_t extLen = text.size() - dot;
        if (extLen <= 8 && extLen + 2 < maxChars && text.substr(dot).find_first_of(L"\\/") == std::wstring_view::npos) {
            ext = text.substr(dot);
        }
    }

    // Split the remaining budget ~2:1 between head and tail.
    const size_t body = maxChars - 1 - ext.size();
    size_t head = (body * 2 + 2) / 3;
    size_t tail = body - head;

    // Never cut a surrogate pair in half.
    if (head > 0 && isHighSurrogate(text[head - 1])) {
        --head;
    }
    size_t tailStart = text.size() - ext.size() - tail;
    if (tail > 0 && tailStart < text.size() && isLowSurrogate(text[tailStart])) {
        ++tailStart;
        --tail;
    }

    std::wstring out;
    out.reserve(maxChars);
    out.append(text.substr(0, head));
    out += kEllipsis;
    out.append(text.substr(tailStart, tail));
    out.append(ext);
    return out;
}

} // namespace hh::path
