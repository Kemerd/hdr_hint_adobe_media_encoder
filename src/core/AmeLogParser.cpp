// ---------------------------------------------------------------------------
// AmeLogParser.cpp - incremental, regex-free parser for AMEEncodingLog.txt.
//
// The parser is a small state machine fed one line at a time:
//
//   Idle ----(field line)-------------> InBlock
//   InBlock --(field line)------------> InBlock          (fields accumulate)
//   InBlock --("Source File" again)---> emit Incomplete, start a new block
//   InBlock --(status line)-----------> emit record; Failed -> FailureReason
//   InBlock --(queue status line)-----> emit Incomplete, back to Idle
//   FailureReason --(rule line)-------> toggle "inside the ---- rules"
//   FailureReason --(text)------------> append to the last record's reason
//   FailureReason --(blank after the closing rule / field / status / 50 lines)
//                                     -> Idle (field and status lines are
//                                        re-fed so nothing is lost)
//
// Everything is hand scanned: the log is written by AME with whatever the
// user's locale produces, and a regex engine on a multi-kilobyte failure
// reason is both slow and a stack-overflow hazard on MSVC. Lines longer than
// 8 KiB are truncated before they reach any of the classifiers.
// ---------------------------------------------------------------------------
#include "core/AmeLogParser.h"

#include "core/Logger.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hh {

namespace {

constexpr const wchar_t* kComponent = L"LogParser";

/// Longest line we are willing to look at; anything past this is dropped.
constexpr size_t kMaxLineChars = 8 * 1024;
/// Field keys are short English (or localized) labels; longer "keys" are noise.
constexpr size_t kMaxKeyChars = 40;
/// A rule line needs at least this many hyphens to count as one.
constexpr size_t kMinRuleChars = 10;
/// Timestamps are well under this; longer left-hand sides are not status lines.
constexpr size_t kMaxTimestampChars = 64;
/// Hard stop for the FailureReason state so a malformed log cannot swallow the file.
constexpr int kMaxReasonLines = 50;
/// Cap on the collected failure reason text (characters).
constexpr size_t kMaxReasonChars = 4096;

/// Classification of one incoming line, computed once per feedLine call.
enum class LineKind { Blank, Rule, Field, Status, Text };

/**
 * @brief ASCII digit test that does not depend on the C locale.
 */
[[nodiscard]] constexpr bool isDigit(wchar_t c) noexcept {
    return c >= L'0' && c <= L'9';
}

/**
 * @brief ASCII letter test (drive letters).
 */
[[nodiscard]] constexpr bool isAsciiAlpha(wchar_t c) noexcept {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

/**
 * @brief True when a field value looks like an absolute Windows path
 *        ("X:\", "X:/" or a UNC "\\server\share").
 *
 * Used for the positional fallback on localized AME builds whose field keys
 * are not the English "Source File" / "Output File".
 */
[[nodiscard]] bool looksLikePath(std::wstring_view v) noexcept {
    // Drive-letter form: letter, colon, separator.
    if (v.size() >= 3 && isAsciiAlpha(v[0]) && v[1] == L':' && (v[2] == L'\\' || v[2] == L'/')) {
        return true;
    }
    // UNC form: two leading backslashes and at least something after them.
    if (v.size() >= 3 && v[0] == L'\\' && v[1] == L'\\' && v[2] != L'\\') {
        return true;
    }
    return false;
}

/**
 * @brief Parses a leading "<digits>x<digits>" token ("3840x2160 (1.0)").
 * @return true when both numbers were present and sane.
 */
[[nodiscard]] bool parseDimensions(std::wstring_view token, int& width, int& height) noexcept {
    width = 0;
    height = 0;
    size_t i = 0;
    long long w = 0;
    // Width digits.
    while (i < token.size() && isDigit(token[i])) {
        w = w * 10 + (token[i] - L'0');
        if (w > 100000) { return false; }
        ++i;
    }
    if (i == 0 || i >= token.size() || (token[i] != L'x' && token[i] != L'X')) { return false; }
    ++i;
    // Height digits.
    long long h = 0;
    const size_t heightStart = i;
    while (i < token.size() && isDigit(token[i])) {
        h = h * 10 + (token[i] - L'0');
        if (h > 100000) { return false; }
        ++i;
    }
    if (i == heightStart) { return false; }
    // Whatever follows must not be another digit run glued on (e.g. "1920x1080p" is fine, "1920x10x8" is not).
    if (i < token.size() && token[i] == L'x') { return false; }
    if (w <= 0 || h <= 0) { return false; }
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

/**
 * @brief True for "HH:MM:SS" style values (AME's "Encoding Time").
 */
[[nodiscard]] bool looksLikeHms(std::wstring_view v) noexcept {
    if (v.size() < 7 || v.size() > 12) { return false; }
    int colons = 0;
    for (const wchar_t c : v) {
        if (c == L':') { ++colons; continue; }
        if (!isDigit(c)) { return false; }
    }
    return colons == 2;
}

/**
 * @brief True when an unknown-key value is clearly AME's "Video:" summary
 *        (starts with WxH and mentions a frame rate).
 */
[[nodiscard]] bool looksLikeVideoValue(std::wstring_view v) noexcept {
    int w = 0;
    int h = 0;
    if (!parseDimensions(v, w, h)) { return false; }
    return platform::icontains(v, L" fps");
}

/**
 * @brief Splits on the literal ", " separator AME uses between video tokens.
 */
[[nodiscard]] std::vector<std::wstring_view> splitTokens(std::wstring_view v) {
    std::vector<std::wstring_view> tokens;
    size_t pos = 0;
    // Walk separator to separator; the last piece runs to the end.
    while (pos <= v.size()) {
        const size_t sep = v.find(L", ", pos);
        if (sep == std::wstring_view::npos) {
            const std::wstring_view tail = platform::trim(v.substr(pos));
            if (!tail.empty()) { tokens.push_back(tail); }
            break;
        }
        const std::wstring_view piece = platform::trim(v.substr(pos, sep - pos));
        if (!piece.empty()) { tokens.push_back(piece); }
        pos = sep + 2;
    }
    return tokens;
}

/**
 * @brief Human label for a result enum (log lines only).
 */
[[nodiscard]] const wchar_t* resultName(AmeItemRecord::Result r) noexcept {
    switch (r) {
    case AmeItemRecord::Result::Success:    return L"Success";
    case AmeItemRecord::Result::Failed:     return L"Failed";
    case AmeItemRecord::Result::Incomplete: return L"Incomplete";
    case AmeItemRecord::Result::Unknown:    break;
    }
    return L"Unknown";
}

} // namespace

// ===========================================================================
// Construction / lifecycle
// ===========================================================================

/**
 * @brief Creates a parser in the Idle state.
 * @param preferDayFirstDates resolves ambiguous "a/b/yyyy" stamps as D/M/Y
 */
AmeLogParser::AmeLogParser(bool preferDayFirstDates)
    : preferDayFirst_(preferDayFirstDates) {
}

/**
 * @brief Drops the partial block and the failure-reason bookkeeping.
 *
 * Emitted-but-not-yet-taken records survive: they came from real lines. The
 * queue de-duplication memory is cleared as well because a reset means the
 * file is being re-read from the top.
 */
void AmeLogParser::reset() {
    state_ = State::Idle;
    current_ = AmeItemRecord{};
    fieldIndex_ = 0;
    reasonLines_ = 0;
    reasonOpen_ = false;
    haveQueueKind_ = false;
    lastQueueKind_ = AmeQueueEvent::Kind::Stopped;
}

/**
 * @brief Flushes an unterminated block as Incomplete and returns to Idle.
 */
void AmeLogParser::finish() {
    // A block without its status line is reported so the caller at least
    // learns the paths involved.
    if (state_ == State::InBlock) {
        HH_LOG_DEBUG(kComponent, L"finish(): flushing unterminated block for '{}'", current_.outputPath);
        emitCurrent(AmeItemRecord::Result::Incomplete, L"", 0);
    }
    state_ = State::Idle;
    reasonOpen_ = false;
    reasonLines_ = 0;
    fieldIndex_ = 0;
}

/**
 * @brief Moves out every record emitted since the previous call.
 */
std::vector<AmeItemRecord> AmeLogParser::takeItems() {
    std::vector<AmeItemRecord> out;
    out.swap(items_);
    return out;
}

/**
 * @brief Moves out every queue event emitted since the previous call.
 */
std::vector<AmeQueueEvent> AmeLogParser::takeQueueEvents() {
    std::vector<AmeQueueEvent> out;
    out.swap(queueEvents_);
    return out;
}

// ===========================================================================
// Line feed
// ===========================================================================

/**
 * @brief Feeds one complete line through the state machine.
 * @param rawLine       the line without its terminator (a stray CR/LF is tolerated)
 * @param fileClockUtc  used as the record time when the stamp cannot be parsed
 */
void AmeLogParser::feedLine(std::wstring_view rawLine, uint64_t fileClockUtc) {
    // ---- Normalise the line -------------------------------------------------
    std::wstring_view line = rawLine;
    if (line.size() > kMaxLineChars) {
        HH_LOG_DEBUG(kComponent, L"truncating a {}-char line to {} chars", line.size(), kMaxLineChars);
        line = line.substr(0, kMaxLineChars);
    }
    // Callers strip terminators, but be forgiving about a stray one.
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) {
        line.remove_suffix(1);
    }
    // A UTF-8 BOM decoded to U+FEFF must not poison the first line.
    if (!line.empty() && line.front() == static_cast<wchar_t>(0xFEFF)) {
        line.remove_prefix(1);
    }
    const std::wstring_view trimmed = platform::trim(line);

    // ---- Classify once ------------------------------------------------------
    std::wstring key;
    std::wstring value;
    std::wstring stampText;
    std::wstring statusText;
    LineKind kind = LineKind::Text;
    if (trimmed.empty()) {
        kind = LineKind::Blank;
    } else if (isRuleLine(trimmed)) {
        kind = LineKind::Rule;
    } else if (splitFieldLine(trimmed, key, value)) {
        kind = LineKind::Field;
    } else if (splitStatusLine(trimmed, stampText, statusText)) {
        kind = LineKind::Status;
    }

    // ---- FailureReason: collect the text between the ---- rules -------------
    if (state_ == State::FailureReason) {
        bool consumed = false;
        if (kind == LineKind::Field || kind == LineKind::Status) {
            // The next block (or a queue line) has started: leave and re-feed
            // this very line through the normal path below.
            state_ = State::Idle;
            reasonOpen_ = false;
        } else if (reasonLines_ >= kMaxReasonLines) {
            // Safety valve for a log that never closes its reason block.
            HH_LOG_DEBUG(kComponent, L"failure reason exceeded {} lines; leaving FailureReason", kMaxReasonLines);
            state_ = State::Idle;
            reasonOpen_ = false;
        } else {
            ++reasonLines_;
            consumed = true;
            // The reason belongs to the record emitted by the status line.
            AmeItemRecord* target = nullptr;
            if (!items_.empty() && items_.back().result == AmeItemRecord::Result::Failed) {
                target = &items_.back();
            }
            const bool haveText = (target != nullptr) && !target->failureReason.empty();
            if (kind == LineKind::Rule) {
                // Rules bracket the reason: first one opens, second one closes.
                reasonOpen_ = !reasonOpen_;
            } else if (kind == LineKind::Blank) {
                // A blank line after the closing rule ends the block.
                if (!reasonOpen_ && haveText) {
                    state_ = State::Idle;
                    reasonOpen_ = false;
                }
            } else {
                // Text: inside the rules, or a rule-less single-line reason.
                if (reasonOpen_ || !haveText) {
                    if (target == nullptr) {
                        HH_LOG_DEBUG(kComponent, L"failure reason text arrived after the record was taken; dropping '{}'", trimmed);
                    } else if (target->failureReason.size() < kMaxReasonChars) {
                        if (!target->failureReason.empty()) { target->failureReason += L'\n'; }
                        target->failureReason.append(trimmed.data(), trimmed.size());
                        if (target->failureReason.size() > kMaxReasonChars) {
                            target->failureReason.resize(kMaxReasonChars);
                        }
                    }
                }
            }
        }
        if (consumed) { return; }
    }

    // ---- Normal processing --------------------------------------------------
    switch (kind) {
    case LineKind::Blank:
    case LineKind::Rule:
        // Blank lines separate blocks; the header rule is decoration.
        return;

    case LineKind::Field: {
        if (state_ == State::Idle) {
            // Any field opens a block (the first one is "Source File" on
            // English builds, but localized builds may differ).
            current_ = AmeItemRecord{};
            fieldIndex_ = 0;
            state_ = State::InBlock;
        } else if (state_ == State::InBlock && platform::iequals(key, L"Source File")) {
            // A block that never got its status line: report what we have.
            HH_LOG_WARN(kComponent, L"new 'Source File' inside an open block; emitting '{}' as Incomplete", current_.outputPath);
            emitCurrent(AmeItemRecord::Result::Incomplete, L"", fileClockUtc);
            state_ = State::InBlock;
        }
        applyField(key, value);
        return;
    }

    case LineKind::Status: {
        // Resolve the stamp; the file clock is the fallback for odd locales.
        uint64_t utc = fileClockUtc;
        if (const auto parsed = platform::parseAmeTimestamp(stampText, preferDayFirst_)) {
            utc = *parsed;
        } else {
            HH_LOG_DEBUG(kComponent, L"could not parse timestamp '{}'; using file clock", stampText);
        }
        AmeQueueEvent::Kind queueKind = AmeQueueEvent::Kind::Started;
        const bool isQueue = classifyQueueStatus(statusText, queueKind);

        if (state_ == State::InBlock) {
            if (isQueue) {
                // Queue lines never terminate a block in practice; treat the
                // block as cut short rather than losing it.
                emitCurrent(AmeItemRecord::Result::Incomplete, statusText, utc);
                state_ = State::Idle;
            } else {
                const AmeItemRecord::Result result = classifyStatus(statusText);
                emitCurrent(result, statusText, utc);
                if (result == AmeItemRecord::Result::Failed) {
                    state_ = State::FailureReason;
                    reasonLines_ = 0;
                    reasonOpen_ = false;
                } else {
                    state_ = State::Idle;
                }
            }
        } else if (!isQueue) {
            // A completion line with no block in front of it: nothing to attach.
            HH_LOG_DEBUG(kComponent, L"status '{}' outside a block; ignored", statusText);
        }

        if (isQueue) {
            // "Queue Paused" fires hundreds of times; only state changes matter.
            if (!haveQueueKind_ || lastQueueKind_ != queueKind) {
                AmeQueueEvent ev;
                ev.kind = queueKind;
                ev.utc = utc;
                queueEvents_.push_back(ev);
                lastQueueKind_ = queueKind;
                haveQueueKind_ = true;
            }
        }
        return;
    }

    case LineKind::Text:
        // Header ("Log File Created: ...") and anything we do not understand.
        if (state_ == State::InBlock) {
            HH_LOG_DEBUG(kComponent, L"unrecognised line inside a block: '{}'", trimmed);
        }
        return;
    }
}

// ===========================================================================
// Private helpers
// ===========================================================================

/**
 * @brief Finalises the current block and appends it to the output list.
 */
void AmeLogParser::emitCurrent(AmeItemRecord::Result result, std::wstring_view statusText, uint64_t utc) {
    current_.result = result;
    current_.statusText.assign(platform::trim(statusText));
    current_.statusUtc = utc;
    HH_LOG_DEBUG(kComponent, L"block {}: '{}' <- '{}' ({})", resultName(result), current_.outputPath, current_.sourcePath, current_.statusText);
    items_.push_back(std::move(current_));
    // Start clean for the next block.
    current_ = AmeItemRecord{};
    fieldIndex_ = 0;
}

/**
 * @brief Stores one "Key: value" pair on the current block.
 *
 * English keys are matched case-insensitively. Unknown keys fall back to
 * position: the first path-like value is the source, the second the output;
 * a value that looks like the video summary or an HH:MM:SS duration is also
 * recognised so localized logs still yield something useful.
 */
void AmeLogParser::applyField(std::wstring_view key, std::wstring_view value) {
    const std::wstring_view k = platform::trim(key);
    const std::wstring_view v = platform::trim(value);
    const bool pathLike = looksLikePath(v);

    // ---- Known English keys -------------------------------------------------
    bool handled = true;
    if (platform::iequals(k, L"Source File")) {
        current_.sourcePath.assign(v);
    } else if (platform::iequals(k, L"Output File")) {
        current_.outputPath.assign(v);
    } else if (platform::iequals(k, L"Preset Used")) {
        current_.presetName.assign(v);
    } else if (platform::iequals(k, L"Video")) {
        current_.videoLine.assign(v);
        current_.video = parseVideoLine(v);
    } else if (platform::iequals(k, L"Audio")) {
        current_.audioLine.assign(v);
    } else if (platform::iequals(k, L"Bitrate")) {
        current_.bitrateLine.assign(v);
    } else if (platform::iequals(k, L"Encoding Time")) {
        current_.encodingTime.assign(v);
    } else {
        handled = false;
    }

    // ---- Positional / shape based fallback for unknown keys -----------------
    if (!handled) {
        if (pathLike) {
            if (fieldIndex_ == 0 && current_.sourcePath.empty()) {
                current_.sourcePath.assign(v);
            } else if (fieldIndex_ == 1 && current_.outputPath.empty()) {
                current_.outputPath.assign(v);
            } else {
                HH_LOG_DEBUG(kComponent, L"extra path field '{}' ignored: '{}'", k, v);
            }
        } else if (current_.videoLine.empty() && looksLikeVideoValue(v)) {
            current_.videoLine.assign(v);
            current_.video = parseVideoLine(v);
        } else if (current_.encodingTime.empty() && looksLikeHms(v)) {
            current_.encodingTime.assign(v);
        } else {
            HH_LOG_DEBUG(kComponent, L"unknown field '{}' ignored", k);
        }
    }

    // Every path-like value advances the positional counter, known key or not.
    if (pathLike) {
        ++fieldIndex_;
    }
}

// ===========================================================================
// Static helpers
// ===========================================================================

/**
 * @brief Parses AME's "Video:" value.
 *
 * Example: "3840x2160 (1.0), 59.94 fps, Rec. 2100 PQ, 203 (75% HLG, 58% PQ),
 * Hardware Encoding, Nvidia Codec, 00:11:34:20". The ProRes variant reads
 * "..., Progressive, Rec. 709, ..., Quality 100, Apple ProRes 422 HQ, ...".
 */
VideoSummary AmeLogParser::parseVideoLine(std::wstring_view value) {
    VideoSummary vs;
    const std::wstring_view v = platform::trim(value);
    vs.raw.assign(v);
    if (v.empty()) { return vs; }

    // ---- Token by token -----------------------------------------------------
    const std::vector<std::wstring_view> tokens = splitTokens(v);
    for (const std::wstring_view tok : tokens) {
        if (tok.empty()) { continue; }

        // "3840x2160 (1.0)" - normally the first token, but scan them all.
        if (vs.width == 0) {
            int w = 0;
            int h = 0;
            if (parseDimensions(tok, w, h)) {
                vs.width = w;
                vs.height = h;
            }
        }

        // "59.94 fps"
        if (vs.fps <= 0.0 && platform::iendsWith(tok, L" fps")) {
            const std::wstring_view num = platform::trim(tok.substr(0, tok.size() - 4));
            if (const auto parsed = platform::parseDouble(num)) {
                if (*parsed > 0.0 && *parsed < 10000.0) { vs.fps = *parsed; }
            }
        }

        // "Hardware Encoding" / "Software Encoding"
        if (platform::icontains(tok, L"Hardware Encoding")) {
            vs.hardwareEncoding = true;
        } else if (platform::icontains(tok, L"Software Encoding")) {
            vs.hardwareEncoding = false;
        }

        // "Nvidia Codec" -> vendor "Nvidia" (the word "Codec" is dropped wherever it sits).
        if (vs.vendor.empty()) {
            const size_t at = platform::ifind(tok, L"Codec");
            if (at != std::wstring::npos) {
                std::wstring vendor;
                vendor.append(tok.substr(0, at));
                vendor.append(tok.substr(at + 5));
                const std::wstring_view cleaned = platform::trim(vendor);
                if (!cleaned.empty()) { vs.vendor.assign(cleaned); }
            }
        }

        // "Apple ProRes 422 HQ" / "DNxHR HQX" - only present for those codecs.
        if (vs.codecHint.empty() && (platform::icontains(tok, L"ProRes") || platform::icontains(tok, L"DNx"))) {
            vs.codecHint.assign(tok);
        }
    }

    // ---- Transfer characteristic from the whole line ------------------------
    if (platform::icontains(v, L"Rec. 2100 PQ") || platform::icontains(v, L"Rec.2100 PQ")) {
        vs.transfer = TransferKind::PQ;
    } else if (platform::icontains(v, L"Rec. 2100 HLG") || platform::icontains(v, L"Rec.2100 HLG")) {
        vs.transfer = TransferKind::HLG;
    } else if (platform::icontains(v, L"Rec. 709") || platform::icontains(v, L"Rec.709") || platform::icontains(v, L"BT.709")) {
        vs.transfer = TransferKind::SDR;
    } else {
        vs.transfer = TransferKind::Unknown;
    }
    return vs;
}

/**
 * @brief Splits "MM/DD/YYYY hh:mm:ss AM : Status" into its two halves.
 *
 * The separator is the literal " : " (space, colon, space). The left side
 * must contain a digit and the line must not be a field line ("- Key: ...").
 */
bool AmeLogParser::splitStatusLine(std::wstring_view line, std::wstring& timestamp, std::wstring& status) {
    const std::wstring_view t = platform::trim(line);
    if (t.empty() || t.front() == L'-') { return false; }
    if (t.size() > kMaxLineChars) { return false; }

    // Find the separator; the left side is the stamp, the right the status.
    const size_t sep = t.find(L" : ");
    if (sep == std::wstring_view::npos || sep == 0) { return false; }
    const std::wstring_view left = platform::trim(t.substr(0, sep));
    const std::wstring_view right = platform::trim(t.substr(sep + 3));
    if (left.empty() || right.empty() || left.size() > kMaxTimestampChars) { return false; }

    // A timestamp always carries digits; "Error : text" does not qualify.
    const bool hasDigit = std::any_of(left.begin(), left.end(), [](wchar_t c) { return isDigit(c); });
    if (!hasDigit) { return false; }

    timestamp.assign(left);
    status.assign(right);
    return true;
}

/**
 * @brief Splits " - Key: value" into key and value.
 *
 * The trimmed line must start with "- " and the key (up to the first colon)
 * must be non-empty, short and free of path separators.
 */
bool AmeLogParser::splitFieldLine(std::wstring_view line, std::wstring& key, std::wstring& value) {
    const std::wstring_view t = platform::trim(line);
    if (t.size() < 3 || t[0] != L'-' || t[1] != L' ') { return false; }

    // Everything after "- " is "Key: value".
    const std::wstring_view rest = t.substr(2);
    const size_t colon = rest.find(L':');
    if (colon == std::wstring_view::npos) { return false; }
    const std::wstring_view k = platform::trim(rest.substr(0, colon));
    if (k.empty() || k.size() > kMaxKeyChars) { return false; }
    if (k.find(L'\\') != std::wstring_view::npos || k.find(L'/') != std::wstring_view::npos) { return false; }

    // "- C:\path" would otherwise read as key "C"; a drive letter is not a key.
    const std::wstring_view v = platform::trim(rest.substr(colon + 1));
    if (k.size() == 1 && !v.empty() && (v.front() == L'\\' || v.front() == L'/')) { return false; }

    key.assign(k);
    value.assign(v);
    return true;
}

/**
 * @brief True for a line made of ten or more hyphens and nothing else.
 */
bool AmeLogParser::isRuleLine(std::wstring_view line) {
    const std::wstring_view t = platform::trim(line);
    if (t.size() < kMinRuleChars) { return false; }
    return std::all_of(t.begin(), t.end(), [](wchar_t c) { return c == L'-'; });
}

/**
 * @brief Maps a status text to a block result (case-insensitive suffix match).
 */
AmeItemRecord::Result AmeLogParser::classifyStatus(std::wstring_view status) {
    const std::wstring_view t = platform::trim(status);
    if (t.empty()) { return AmeItemRecord::Result::Unknown; }
    if (platform::iendsWith(t, L"File Successfully Encoded")) { return AmeItemRecord::Result::Success; }
    if (platform::iendsWith(t, L"Encoding Failed")) { return AmeItemRecord::Result::Failed; }
    return AmeItemRecord::Result::Unknown;
}

/**
 * @brief Recognises "Queue Started" / "Queue Stopped" / "Queue Paused".
 */
bool AmeLogParser::classifyQueueStatus(std::wstring_view status, AmeQueueEvent::Kind& kind) {
    const std::wstring_view t = platform::trim(status);
    if (t.empty()) { return false; }
    if (platform::iendsWith(t, L"Queue Started")) { kind = AmeQueueEvent::Kind::Started; return true; }
    if (platform::iendsWith(t, L"Queue Stopped")) { kind = AmeQueueEvent::Kind::Stopped; return true; }
    if (platform::iendsWith(t, L"Queue Paused"))  { kind = AmeQueueEvent::Kind::Paused;  return true; }
    return false;
}

} // namespace hh
