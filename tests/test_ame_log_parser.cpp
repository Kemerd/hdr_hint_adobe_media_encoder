// ---------------------------------------------------------------------------
// test_ame_log_parser.cpp - AmeLogParser fed the real AMEEncodingLog.txt
// block shapes: success, "Encoding Failed" with the 37-hyphen reason rules,
// queue status storms, ProRes/HLG video lines, an empty preset and blocks
// that are still open when the feed stops.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "core/AmeLogParser.h"
#include "core/JobModel.h"
#include "platform/Time.h"

#include <cstdint>
#include <string>
#include <vector>

using hh::AmeItemRecord;
using hh::AmeLogParser;
using hh::AmeQueueEvent;
using hh::TransferKind;
using hh::VideoSummary;

namespace {

/// A believable "file clock" for lines whose timestamp cannot be parsed.
constexpr uint64_t kFileClock = 133700000000000000ull;

/// The PQ "Video:" value exactly as AME prints it.
const wchar_t* const kPqVideo =
    L"3840x2160 (1.0), 59.94 fps, Rec. 2100 PQ, 203 (75% HLG, 58% PQ), Hardware Encoding, Nvidia Codec, 00:11:34:20";
/// The HLG variant.
const wchar_t* const kHlgVideo =
    L"3840x2160 (1.0), 59.94 fps, Rec. 2100 HLG, 300 (81% HLG, 62% PQ), Hardware Encoding, Nvidia Codec, 00:05:00:00";
/// A software ProRes export (SDR).
const wchar_t* const kProResVideo =
    L"1920x1080 (1.0), 23.976 fps, Progressive, Rec. 709, Apple ProRes 422 HQ, 00:00:10:00";

/// A complete, successful block.
std::vector<std::wstring> successBlock() {
    return {
        L" - Source File: C:\\Users\\Editor\\Videos\\ep.prproj",
        L" - Output File: B:\\YouTube Renders\\clip.mp4",
        L" - Preset Used: HEVC 4k 59.94 HDR",
        std::wstring(L" - Video: ") + kPqVideo,
        L" - Audio: AAC, 320 kbps, 48 kHz, Stereo",
        L" - Bitrate: VBR, 1 pass, Target 100.00 Mbps",
        L" - Encoding Time: 00:00:19",
        L"09/16/2026 01:51:48 PM : File Successfully Encoded",
        L"",
    };
}

/// A failed block: status line, then the reason between two 37-hyphen rules.
std::vector<std::wstring> failedBlock() {
    const std::wstring rule(37, L'-');
    return {
        L" - Source File: C:\\Users\\Editor\\Videos\\ep.prproj",
        L" - Output File: B:\\YouTube Renders\\failed.mp4",
        L" - Preset Used: HEVC 4k 59.94 HDR",
        std::wstring(L" - Video: ") + kPqVideo,
        L" - Audio: AAC, 320 kbps, 48 kHz, Stereo",
        L" - Bitrate: VBR, 1 pass, Target 100.00 Mbps",
        L" - Encoding Time: 00:00:05",
        L"09/16/2026 01:55:02 PM : Encoding Failed",
        rule,
        L"The Operation was interrupted by user",
        rule,
        L"",
    };
}

/// Feeds every line with the same file clock.
void feedAll(AmeLogParser& parser, const std::vector<std::wstring>& lines) {
    for (const std::wstring& line : lines) { parser.feedLine(line, kFileClock); }
}

/// UTC for a US-style AME timestamp (0 when it does not parse; the test then fails loudly).
uint64_t utcOf(const wchar_t* text) {
    const auto v = hh::platform::parseAmeTimestamp(text, false);
    return v.has_value() ? *v : 0;
}

} // namespace

// ---- whole blocks -------------------------------------------------------------------

HH_TEST(AmeLogParser_successBlockProducesOneRecord) {
    AmeLogParser parser;
    feedAll(parser, successBlock());

    std::vector<AmeItemRecord> items = parser.takeItems();
    if (!CHECK_EQ(items.size(), 1)) { return; }
    const AmeItemRecord& r = items[0];

    // Every field line lands in its slot.
    CHECK_WEQ(r.sourcePath, L"C:\\Users\\Editor\\Videos\\ep.prproj");
    CHECK_WEQ(r.outputPath, L"B:\\YouTube Renders\\clip.mp4");
    CHECK_WEQ(r.presetName, L"HEVC 4k 59.94 HDR");
    CHECK_WEQ(r.videoLine, kPqVideo);
    CHECK_WEQ(r.audioLine, L"AAC, 320 kbps, 48 kHz, Stereo");
    CHECK_WEQ(r.bitrateLine, L"VBR, 1 pass, Target 100.00 Mbps");
    CHECK_WEQ(r.encodingTime, L"00:00:19");

    // Status and its timestamp.
    CHECK_EQ(r.result, AmeItemRecord::Result::Success);
    CHECK_WEQ(r.statusText, L"File Successfully Encoded");
    const uint64_t expectedUtc = utcOf(L"09/16/2026 01:51:48 PM");
    CHECK(expectedUtc != 0);
    CHECK_EQ(r.statusUtc, expectedUtc);
    CHECK(r.failureReason.empty());

    // The video summary is parsed as part of the block.
    CHECK_EQ(r.video.width, 3840);
    CHECK_EQ(r.video.height, 2160);
    CHECK_NEAR(r.video.fps, 59.94, 0.001);
    CHECK_EQ(r.video.transfer, TransferKind::PQ);
    CHECK(r.video.hardwareEncoding);
    CHECK_WEQ(r.video.vendor, L"Nvidia");

    // takeItems() moves the records out: a second call is empty.
    CHECK(parser.takeItems().empty());
    CHECK(parser.takeQueueEvents().empty());
}

HH_TEST(AmeLogParser_failedBlockCapturesReasonAndRecovers) {
    AmeLogParser parser;
    feedAll(parser, failedBlock());
    // A success block right after the closing rule must parse normally.
    feedAll(parser, successBlock());

    std::vector<AmeItemRecord> items = parser.takeItems();
    if (!CHECK_EQ(items.size(), 2)) { return; }

    const AmeItemRecord& failed = items[0];
    CHECK_EQ(failed.result, AmeItemRecord::Result::Failed);
    CHECK_WEQ(failed.statusText, L"Encoding Failed");
    CHECK_WEQ(failed.outputPath, L"B:\\YouTube Renders\\failed.mp4");
    CHECK_WEQ(failed.failureReason, L"The Operation was interrupted by user");
    CHECK_WEQ(failed.encodingTime, L"00:00:05");
    CHECK_EQ(failed.statusUtc, utcOf(L"09/16/2026 01:55:02 PM"));

    const AmeItemRecord& ok = items[1];
    CHECK_EQ(ok.result, AmeItemRecord::Result::Success);
    CHECK_WEQ(ok.outputPath, L"B:\\YouTube Renders\\clip.mp4");
    CHECK(ok.failureReason.empty());
}

HH_TEST(AmeLogParser_failureReasonDoesNotLeakIntoItems) {
    // The reason lines and rule lines must never be mistaken for field lines
    // or status lines: only one record, nothing else.
    AmeLogParser parser;
    feedAll(parser, failedBlock());
    parser.finish();
    std::vector<AmeItemRecord> items = parser.takeItems();
    CHECK_EQ(items.size(), 1);
    CHECK(parser.takeQueueEvents().empty());
}

HH_TEST(AmeLogParser_queuePausedStormIsDeduplicated) {
    AmeLogParser parser;
    const std::vector<std::wstring> lines = {
        L"09/16/2026 01:51:20 PM : Queue Started",
        L"09/16/2026 01:51:21 PM : Queue Paused",
        L"09/16/2026 01:51:21 PM : Queue Paused",
        L"09/16/2026 01:51:22 PM : Queue Paused",
        L"09/16/2026 01:51:22 PM : Queue Paused",
        L"09/16/2026 01:51:23 PM : Queue Paused",
        L"09/16/2026 01:51:30 PM : Queue Started",
        L"09/16/2026 01:51:48 PM : Queue Stopped",
    };
    feedAll(parser, lines);

    // Queue lines never produce item records.
    CHECK(parser.takeItems().empty());

    std::vector<AmeQueueEvent> events = parser.takeQueueEvents();
    if (!CHECK_EQ(events.size(), 4)) { return; }
    CHECK_EQ(events[0].kind, AmeQueueEvent::Kind::Started);
    CHECK_EQ(events[1].kind, AmeQueueEvent::Kind::Paused);
    CHECK_EQ(events[2].kind, AmeQueueEvent::Kind::Started);
    CHECK_EQ(events[3].kind, AmeQueueEvent::Kind::Stopped);
    CHECK_EQ(events[0].utc, utcOf(L"09/16/2026 01:51:20 PM"));
    CHECK_EQ(events[3].utc, utcOf(L"09/16/2026 01:51:48 PM"));

    // Moved out: nothing left.
    CHECK(parser.takeQueueEvents().empty());
}

HH_TEST(AmeLogParser_proResBlockIsSdr) {
    AmeLogParser parser;
    std::vector<std::wstring> block = successBlock();
    block[1] = L" - Output File: B:\\YouTube Renders\\master.mov";
    block[2] = L" - Preset Used: Apple ProRes 422 HQ";
    block[3] = std::wstring(L" - Video: ") + kProResVideo;
    feedAll(parser, block);

    std::vector<AmeItemRecord> items = parser.takeItems();
    if (!CHECK_EQ(items.size(), 1)) { return; }
    const AmeItemRecord& r = items[0];
    CHECK_EQ(r.result, AmeItemRecord::Result::Success);
    CHECK_WEQ(r.outputPath, L"B:\\YouTube Renders\\master.mov");
    CHECK_EQ(r.video.width, 1920);
    CHECK_EQ(r.video.height, 1080);
    CHECK_NEAR(r.video.fps, 23.976, 0.001);
    CHECK_EQ(r.video.transfer, TransferKind::SDR);
    CHECK_FALSE(r.video.hardwareEncoding);
    CHECK(r.video.vendor.empty());
    CHECK(r.video.codecHint.find(L"ProRes") != std::wstring::npos);
}

HH_TEST(AmeLogParser_hlgBlockIsHlg) {
    AmeLogParser parser;
    std::vector<std::wstring> block = successBlock();
    block[3] = std::wstring(L" - Video: ") + kHlgVideo;
    feedAll(parser, block);

    std::vector<AmeItemRecord> items = parser.takeItems();
    if (!CHECK_EQ(items.size(), 1)) { return; }
    CHECK_EQ(items[0].video.transfer, TransferKind::HLG);
    CHECK(items[0].video.hardwareEncoding);
    CHECK_WEQ(items[0].video.vendor, L"Nvidia");
    CHECK_WEQ(items[0].videoLine, kHlgVideo);
}

HH_TEST(AmeLogParser_emptyPresetUsedIsTolerated) {
    // AME writes "Preset Used:" with nothing after it for ad-hoc exports;
    // both the bare and the trailing-space spelling must parse.
    for (const wchar_t* presetLine : {L" - Preset Used:", L" - Preset Used: "}) {
        AmeLogParser parser;
        std::vector<std::wstring> block = successBlock();
        block[2] = presetLine;
        feedAll(parser, block);

        std::vector<AmeItemRecord> items = parser.takeItems();
        if (!CHECK_EQ(items.size(), 1)) { continue; }
        CHECK(items[0].presetName.empty());
        CHECK_WEQ(items[0].outputPath, L"B:\\YouTube Renders\\clip.mp4");
        CHECK_WEQ(items[0].encodingTime, L"00:00:19");
        CHECK_EQ(items[0].result, AmeItemRecord::Result::Success);
    }
}

// ---- partial blocks ------------------------------------------------------------------

HH_TEST(AmeLogParser_unterminatedBlockIsOnlyEmittedByFinish) {
    AmeLogParser parser;
    const std::vector<std::wstring> block = successBlock();

    // First half of the block: nothing may be emitted yet.
    for (size_t i = 0; i < 4 && i < block.size(); ++i) { parser.feedLine(block[i], kFileClock); }
    CHECK(parser.takeItems().empty());

    // The remaining field lines but not the status line: still nothing.
    for (size_t i = 4; i < 7 && i < block.size(); ++i) { parser.feedLine(block[i], kFileClock); }
    CHECK(parser.takeItems().empty());

    // finish() flushes the open block as Incomplete with what it has.
    parser.finish();
    std::vector<AmeItemRecord> items = parser.takeItems();
    if (!CHECK_EQ(items.size(), 1)) { return; }
    CHECK_EQ(items[0].result, AmeItemRecord::Result::Incomplete);
    CHECK_WEQ(items[0].outputPath, L"B:\\YouTube Renders\\clip.mp4");
    CHECK_WEQ(items[0].presetName, L"HEVC 4k 59.94 HDR");
    CHECK_WEQ(items[0].encodingTime, L"00:00:19");

    // A second finish() has nothing to flush.
    parser.finish();
    CHECK(parser.takeItems().empty());
}

HH_TEST(AmeLogParser_blockSplitAcrossFeedSequencesStillCompletes) {
    AmeLogParser parser;
    const std::vector<std::wstring> block = successBlock();

    // Feed the first three lines, drain, then feed the rest: the block state
    // must survive the takeItems() call in between.
    for (size_t i = 0; i < 3 && i < block.size(); ++i) { parser.feedLine(block[i], kFileClock); }
    CHECK(parser.takeItems().empty());
    for (size_t i = 3; i < block.size(); ++i) { parser.feedLine(block[i], kFileClock); }

    std::vector<AmeItemRecord> items = parser.takeItems();
    if (!CHECK_EQ(items.size(), 1)) { return; }
    CHECK_EQ(items[0].result, AmeItemRecord::Result::Success);
    CHECK_WEQ(items[0].sourcePath, L"C:\\Users\\Editor\\Videos\\ep.prproj");
    CHECK_WEQ(items[0].audioLine, L"AAC, 320 kbps, 48 kHz, Stereo");
}

HH_TEST(AmeLogParser_resetDropsPartialBlock) {
    AmeLogParser parser;
    const std::vector<std::wstring> block = successBlock();
    for (size_t i = 0; i < 3 && i < block.size(); ++i) { parser.feedLine(block[i], kFileClock); }

    // reset() throws the partial block away: finish() then has nothing.
    parser.reset();
    parser.finish();
    CHECK(parser.takeItems().empty());

    // And the parser is usable again afterwards.
    feedAll(parser, block);
    CHECK_EQ(parser.takeItems().size(), 1);
}

HH_TEST(AmeLogParser_dayFirstStatusTimestamp) {
    // Non-US locales write "16/09/2026 13:51:48" without AM/PM.
    AmeLogParser parser(true);
    std::vector<std::wstring> block = successBlock();
    block[7] = L"16/09/2026 13:51:48 : File Successfully Encoded";
    feedAll(parser, block);

    std::vector<AmeItemRecord> items = parser.takeItems();
    if (!CHECK_EQ(items.size(), 1)) { return; }
    CHECK_EQ(items[0].result, AmeItemRecord::Result::Success);
    const auto expected = hh::platform::parseAmeTimestamp(L"2026-09-16 13:51:48", false);
    if (CHECK(expected.has_value())) { CHECK_EQ(items[0].statusUtc, *expected); }
}

// ---- static helpers ---------------------------------------------------------------------

HH_TEST(AmeLogParser_parseVideoLinePq) {
    const VideoSummary v = AmeLogParser::parseVideoLine(kPqVideo);
    CHECK_EQ(v.width, 3840);
    CHECK_EQ(v.height, 2160);
    CHECK_NEAR(v.fps, 59.94, 0.001);
    CHECK_EQ(v.transfer, TransferKind::PQ);
    CHECK(v.hardwareEncoding);
    CHECK_WEQ(v.vendor, L"Nvidia");
    CHECK_WEQ(v.raw, kPqVideo);
}

HH_TEST(AmeLogParser_parseVideoLineHlg) {
    const VideoSummary v = AmeLogParser::parseVideoLine(kHlgVideo);
    CHECK_EQ(v.width, 3840);
    CHECK_EQ(v.height, 2160);
    CHECK_EQ(v.transfer, TransferKind::HLG);
    CHECK(v.hardwareEncoding);
    CHECK_WEQ(v.vendor, L"Nvidia");
}

HH_TEST(AmeLogParser_parseVideoLineProResSdr) {
    const VideoSummary v = AmeLogParser::parseVideoLine(kProResVideo);
    CHECK_EQ(v.width, 1920);
    CHECK_EQ(v.height, 1080);
    CHECK_NEAR(v.fps, 23.976, 0.001);
    CHECK_EQ(v.transfer, TransferKind::SDR);
    CHECK_FALSE(v.hardwareEncoding);
    CHECK(v.vendor.empty());
    CHECK(v.codecHint.find(L"ProRes") != std::wstring::npos);
}

HH_TEST(AmeLogParser_parseVideoLineGarbage) {
    // Empty and nonsensical input must produce a zeroed, Unknown summary.
    for (const wchar_t* text : {L"", L"   ", L"no dimensions here", L"x", L"1920x", L"x1080, fps"}) {
        const VideoSummary v = AmeLogParser::parseVideoLine(text);
        CHECK_EQ(v.width, 0);
        CHECK_EQ(v.height, 0);
        CHECK_NEAR(v.fps, 0.0, 1e-9);
        CHECK_EQ(v.transfer, TransferKind::Unknown);
        CHECK_FALSE(v.hardwareEncoding);
    }
}

HH_TEST(AmeLogParser_splitStatusLine) {
    std::wstring ts;
    std::wstring status;

    CHECK(AmeLogParser::splitStatusLine(L"09/16/2026 01:51:48 PM : File Successfully Encoded", ts, status));
    CHECK_WEQ(ts, L"09/16/2026 01:51:48 PM");
    CHECK_WEQ(status, L"File Successfully Encoded");

    CHECK(AmeLogParser::splitStatusLine(L"09/16/2026 01:55:02 PM : Encoding Failed", ts, status));
    CHECK_WEQ(status, L"Encoding Failed");

    CHECK(AmeLogParser::splitStatusLine(L"16/09/2026 13:51:48 : Queue Started", ts, status));
    CHECK_WEQ(ts, L"16/09/2026 13:51:48");
    CHECK_WEQ(status, L"Queue Started");

    // Not status lines: field lines, rule lines, reason text, empty.
    CHECK_FALSE(AmeLogParser::splitStatusLine(L" - Source File: C:\\Users\\Editor\\ep.prproj", ts, status));
    CHECK_FALSE(AmeLogParser::splitStatusLine(std::wstring(37, L'-'), ts, status));
    CHECK_FALSE(AmeLogParser::splitStatusLine(L"The Operation was interrupted by user", ts, status));
    CHECK_FALSE(AmeLogParser::splitStatusLine(L"", ts, status));
}

HH_TEST(AmeLogParser_splitFieldLine) {
    std::wstring key;
    std::wstring value;

    CHECK(AmeLogParser::splitFieldLine(L" - Preset Used: HEVC 4k 59.94 HDR", key, value));
    CHECK_WEQ(key, L"Preset Used");
    CHECK_WEQ(value, L"HEVC 4k 59.94 HDR");

    // Values may themselves contain colons (drive letters, time codes).
    CHECK(AmeLogParser::splitFieldLine(L" - Source File: C:\\Users\\Editor\\ep.prproj", key, value));
    CHECK_WEQ(key, L"Source File");
    CHECK_WEQ(value, L"C:\\Users\\Editor\\ep.prproj");

    CHECK(AmeLogParser::splitFieldLine(L" - Encoding Time: 00:00:19", key, value));
    CHECK_WEQ(key, L"Encoding Time");
    CHECK_WEQ(value, L"00:00:19");

    // Empty values are still field lines.
    CHECK(AmeLogParser::splitFieldLine(L" - Preset Used:", key, value));
    CHECK_WEQ(key, L"Preset Used");
    CHECK(value.empty());
    CHECK(AmeLogParser::splitFieldLine(L" - Preset Used: ", key, value));
    CHECK(value.empty());

    // Not field lines.
    CHECK_FALSE(AmeLogParser::splitFieldLine(L"09/16/2026 01:51:48 PM : File Successfully Encoded", key, value));
    CHECK_FALSE(AmeLogParser::splitFieldLine(L"The Operation was interrupted by user", key, value));
    CHECK_FALSE(AmeLogParser::splitFieldLine(L"", key, value));
    CHECK_FALSE(AmeLogParser::splitFieldLine(std::wstring(37, L'-'), key, value));
}

HH_TEST(AmeLogParser_isRuleLine) {
    CHECK(AmeLogParser::isRuleLine(std::wstring(37, L'-')));
    CHECK(AmeLogParser::isRuleLine(std::wstring(10, L'-')));
    CHECK(AmeLogParser::isRuleLine(std::wstring(200, L'-')));
    CHECK_FALSE(AmeLogParser::isRuleLine(std::wstring(9, L'-')));
    CHECK_FALSE(AmeLogParser::isRuleLine(L""));
    CHECK_FALSE(AmeLogParser::isRuleLine(L"----------x"));
    // Trailing whitespace is tolerated: AME lines are trimmed before classification.
    CHECK(AmeLogParser::isRuleLine(L"---------- "));
    CHECK_FALSE(AmeLogParser::isRuleLine(L" - Source File: x"));
}

HH_TEST(AmeLogParser_classifyStatus) {
    CHECK_EQ(AmeLogParser::classifyStatus(L"File Successfully Encoded"), AmeItemRecord::Result::Success);
    CHECK_EQ(AmeLogParser::classifyStatus(L"file successfully encoded"), AmeItemRecord::Result::Success);
    CHECK_EQ(AmeLogParser::classifyStatus(L"Encoding Failed"), AmeItemRecord::Result::Failed);
    CHECK_EQ(AmeLogParser::classifyStatus(L"ENCODING FAILED"), AmeItemRecord::Result::Failed);
    // Suffix match: a prefix must not hide the verdict.
    CHECK_EQ(AmeLogParser::classifyStatus(L"Item 3: File Successfully Encoded"), AmeItemRecord::Result::Success);
    // Anything else is Unknown.
    CHECK_EQ(AmeLogParser::classifyStatus(L"Queue Started"), AmeItemRecord::Result::Unknown);
    CHECK_EQ(AmeLogParser::classifyStatus(L""), AmeItemRecord::Result::Unknown);
    CHECK_EQ(AmeLogParser::classifyStatus(L"Encoded"), AmeItemRecord::Result::Unknown);
}

HH_TEST(AmeLogParser_classifyQueueStatus) {
    AmeQueueEvent::Kind kind = AmeQueueEvent::Kind::Stopped;
    CHECK(AmeLogParser::classifyQueueStatus(L"Queue Started", kind));
    CHECK_EQ(kind, AmeQueueEvent::Kind::Started);
    CHECK(AmeLogParser::classifyQueueStatus(L"Queue Stopped", kind));
    CHECK_EQ(kind, AmeQueueEvent::Kind::Stopped);
    CHECK(AmeLogParser::classifyQueueStatus(L"Queue Paused", kind));
    CHECK_EQ(kind, AmeQueueEvent::Kind::Paused);
    CHECK(AmeLogParser::classifyQueueStatus(L"queue paused", kind));
    CHECK_EQ(kind, AmeQueueEvent::Kind::Paused);

    CHECK_FALSE(AmeLogParser::classifyQueueStatus(L"Encoding Failed", kind));
    CHECK_FALSE(AmeLogParser::classifyQueueStatus(L"File Successfully Encoded", kind));
    CHECK_FALSE(AmeLogParser::classifyQueueStatus(L"", kind));
}
