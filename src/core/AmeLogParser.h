// ---------------------------------------------------------------------------
// AmeLogParser.h - incremental parser for AMEEncodingLog.txt blocks.
//
// Pure: bytes/lines in, records out. No I/O, no regex. Feed complete lines
// (CR/LF already stripped) in file order; call takeItems() afterwards.
//
// A block looks like:
//    - Source File: C:\...\ep.prproj
//    - Output File: B:\...\clip.mp4
//    - Preset Used: HEVC 4k 59.94 HDR
//    - Video: 3840x2160 (1.0), 59.94 fps, Rec. 2100 PQ, 203 (75% HLG, 58% PQ), Hardware Encoding, Nvidia Codec, 00:11:34:20
//    - Audio: AAC, 320 kbps, 48 kHz, Stereo
//    - Bitrate: VBR, 1 pass, Target 100.00 Mbps
//    - Encoding Time: 00:00:19
//   09/16/2026 01:51:48 PM : File Successfully Encoded
// ---------------------------------------------------------------------------
#pragma once

#include "core/JobModel.h"
#include "platform/Win.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hh {

/**
 * @brief One completed (or failed) export block.
 */
struct AmeItemRecord {
    enum class Result { Success, Failed, Incomplete, Unknown };

    std::wstring sourcePath;
    std::wstring outputPath;
    std::wstring presetName;
    std::wstring videoLine;
    std::wstring audioLine;
    std::wstring bitrateLine;
    std::wstring encodingTime;       ///< "00:12:34"
    VideoSummary video;
    Result result = Result::Unknown;
    std::wstring statusText;         ///< "File Successfully Encoded" / "Encoding Failed" / ...
    uint64_t statusUtc = 0;          ///< parsed from the status line (or the file clock)
    std::wstring failureReason;      ///< text between the ---- rules after "Encoding Failed"
};

/**
 * @brief Queue-level status lines.
 */
struct AmeQueueEvent {
    enum class Kind { Started, Stopped, Paused };
    Kind kind = Kind::Started;
    uint64_t utc = 0;
};

class AmeLogParser {
public:
    explicit AmeLogParser(bool preferDayFirstDates = false);

    /// Feeds one complete line. @p fileClockUtc is used when the timestamp cannot be parsed.
    void feedLine(std::wstring_view line, uint64_t fileClockUtc);
    /// Drops any partial block (used when the file is recreated).
    void reset();
    /// Flushes an unterminated block as Incomplete (call at end of a full-file parse if desired).
    void finish();

    /// Records emitted since the last call (moved out).
    std::vector<AmeItemRecord> takeItems();
    std::vector<AmeQueueEvent> takeQueueEvents();

    // ---- static helpers (unit-tested) ----------------------------------------

    /// Parses the "Video:" value into width/height/fps/transfer/hardware/vendor.
    static VideoSummary parseVideoLine(std::wstring_view value);
    /// Splits "MM/DD/YYYY hh:mm:ss AM : Status" -> (timestamp text, status text).
    static bool splitStatusLine(std::wstring_view line, std::wstring& timestamp, std::wstring& status);
    /// Splits " - Key: value" -> (key, value). False when not a field line.
    static bool splitFieldLine(std::wstring_view line, std::wstring& key, std::wstring& value);
    /// True for "----------" rule lines (10+ hyphens, nothing else).
    static bool isRuleLine(std::wstring_view line);
    /// Classifies a status text (suffix match, case-insensitive).
    static AmeItemRecord::Result classifyStatus(std::wstring_view status);
    /// Recognises "Queue Started/Stopped/Paused".
    static bool classifyQueueStatus(std::wstring_view status, AmeQueueEvent::Kind& kind);

private:
    enum class State { Idle, InBlock, FailureReason };

    void emitCurrent(AmeItemRecord::Result result, std::wstring_view statusText, uint64_t utc);
    void applyField(std::wstring_view key, std::wstring_view value);

    State state_ = State::Idle;
    AmeItemRecord current_;
    int fieldIndex_ = 0;
    int reasonLines_ = 0;
    bool reasonOpen_ = false;
    bool preferDayFirst_ = false;
    std::vector<AmeItemRecord> items_;
    std::vector<AmeQueueEvent> queueEvents_;
    AmeQueueEvent::Kind lastQueueKind_ = AmeQueueEvent::Kind::Stopped;
    bool haveQueueKind_ = false;
};

} // namespace hh
