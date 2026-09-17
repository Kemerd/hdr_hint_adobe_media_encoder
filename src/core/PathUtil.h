// ---------------------------------------------------------------------------
// PathUtil.h - path rules specific to HdrHint (keys, suffixes, AME temp names).
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <optional>
#include <string>
#include <vector>

namespace hh::path {

/// Absolute path, no \\?\ prefix, upper-cased: the dedup key for jobs.
std::wstring normalizeKey(std::wstring_view path);

/// "C:\a\b\clip.mp4" -> "clip.mp4"
std::wstring fileName(std::wstring_view path);
/// "clip.mp4" -> "clip" ; "clip.tar.gz" -> "clip.tar"
std::wstring stem(std::wstring_view path);
/// ".mp4" (lower-cased, with dot) or empty.
std::wstring extension(std::wstring_view path);
/// Parent directory without trailing separator ("C:\" stays "C:\").
std::wstring parent(std::wstring_view path);
/// Joins with a single backslash.
std::wstring join(std::wstring_view dir, std::wstring_view name);
/// Replaces '/' with '\' and collapses duplicate separators (keeps UNC prefix).
std::wstring normalizeSeparators(std::wstring_view path);

/// True when @p ext (with dot) is in @p list (case-insensitive).
bool hasExtension(std::wstring_view path, const std::vector<std::wstring>& list);

/**
 * @brief Where the hint file goes: "<folder>\<stem><suffix>.mkv".
 *        A stem that already ends with the suffix is not doubled.
 * @param outputFolder empty = same folder as the source
 */
std::wstring hintPathFor(std::wstring_view sourcePath, std::wstring_view suffix, std::wstring_view outputFolder);

/// "clip_REC709_HINT.mkv" -> "clip_REC709_HINT (2).mkv", "(3)", ... first free name.
std::wstring firstFreePath(std::wstring_view path);

/// True for our own outputs: ".mkv" and stem ends with the suffix (case-insensitive).
bool isOurOutput(std::wstring_view fileName, std::wstring_view suffix);
/// True for our in-progress files: "*.hdrhint-partial.mkv".
bool isPartialOutput(std::wstring_view fileName);
/// The partial name for a hint path: "<hint without .mkv>.hdrhint-partial.mkv".
std::wstring partialPathFor(std::wstring_view hintPath);

/**
 * @brief AME sidecar "<stem>.<pid>.<tid>.<ext>" written during an encode.
 */
struct Sidecar {
    std::wstring stem;
    DWORD pid = 0;
    DWORD tid = 0;
    std::wstring ext;   ///< "m4v", "aac", ...
};
/// Parses an AME sidecar name (ext must be a known media ES extension).
std::optional<Sidecar> parseSidecar(std::wstring_view fileName);
/// True for AME's "<8hex>-<4hex>-<4hex>-<4hex>.tmp" scratch files and other temp names.
bool isTemporaryName(std::wstring_view fileName);

/// Characters not allowed in a file name component: \ / : * ? " < > |
bool hasInvalidFileNameChars(std::wstring_view name);

/// Middle-ellipsis for display: "very_long_file_name.mp4" -> "very_lo…ame.mp4" (max chars).
std::wstring ellipsizeMiddle(std::wstring_view text, size_t maxChars);

} // namespace hh::path
