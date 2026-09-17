// ---------------------------------------------------------------------------
// test_ipc_protocol.cpp - the newline-delimited JSON protocol between the
// CEP panel and HdrHint: line parsing, tolerant field access and the shape
// of every outbound message builder.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "core/IpcProtocol.h"
#include "core/JobModel.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ipc = hh::ipc;
namespace platform = hh::platform;
using hh::Job;
using hh::JobState;
using hh::TransferKind;

namespace {

/// Looks up a nested object member; nullptr when absent or not an object.
const ipc::json* objectAt(const ipc::json& msg, const char* key) {
    if (!key || !msg.is_object()) { return nullptr; }
    const auto it = msg.find(key);
    if (it == msg.end() || !it->is_object()) { return nullptr; }
    return &*it;
}

/// Looks up a nested array member; nullptr when absent or not an array.
const ipc::json* arrayAt(const ipc::json& msg, const char* key) {
    if (!key || !msg.is_object()) { return nullptr; }
    const auto it = msg.find(key);
    if (it == msg.end() || !it->is_array()) { return nullptr; }
    return &*it;
}

/// A job with the fields the panel displays filled in.
Job makeJob(hh::JobId id, JobState state, TransferKind transfer, const std::wstring& outputPath) {
    Job job;
    job.id = id;
    job.state = state;
    job.transfer = transfer;
    job.outputPath = outputPath;
    job.key = platform::toUpperInvariant(outputPath);
    return job;
}

} // namespace

// ---- parsing ---------------------------------------------------------------------

/**
 * @brief A JSON object with a string "kind" parses, with or without the
 *        trailing newline and surrounding whitespace.
 */
HH_TEST(Ipc_parseLineAcceptsObjectWithKind) {
    const std::optional<ipc::json> plain = ipc::parseLine("{\"kind\":\"ping\"}");
    if (CHECK(plain.has_value())) {
        CHECK_EQ(ipc::kindOf(*plain), "ping");
    }

    const std::optional<ipc::json> newline = ipc::parseLine("{\"kind\":\"ping\"}\n");
    if (CHECK(newline.has_value())) {
        CHECK_EQ(ipc::kindOf(*newline), "ping");
    }

    const std::optional<ipc::json> padded = ipc::parseLine("  \t{\"kind\":\"ping\"}  \r\n");
    if (CHECK(padded.has_value())) {
        CHECK_EQ(ipc::kindOf(*padded), "ping");
    }

    // Extra fields ride along and are reachable through the accessors.
    const std::optional<ipc::json> hold = ipc::parseLine("{\"kind\":\"setHold\",\"id\":3,\"jobId\":9,\"hold\":true}\n");
    if (CHECK(hold.has_value())) {
        CHECK_EQ(ipc::kindOf(*hold), "setHold");
        CHECK_EQ(ipc::num(*hold, "id"), 3.0);
        CHECK_EQ(ipc::num(*hold, "jobId"), 9.0);
        CHECK(ipc::boolean(*hold, "hold"));
    }
}

/**
 * @brief Anything that is not a JSON object carrying a non-empty string
 *        "kind" is rejected: plain text, arrays, objects without kind,
 *        empty lines, truncated documents.
 */
HH_TEST(Ipc_parseLineRejectsJunk) {
    CHECK_FALSE(ipc::parseLine("not json").has_value());
    CHECK_FALSE(ipc::parseLine("[1,2]").has_value());
    CHECK_FALSE(ipc::parseLine("{\"nokind\":1}").has_value());
    CHECK_FALSE(ipc::parseLine("").has_value());
    CHECK_FALSE(ipc::parseLine("   \r\n").has_value());
    CHECK_FALSE(ipc::parseLine("{\"kind\":\"\"}").has_value());       // empty kind
    CHECK_FALSE(ipc::parseLine("{\"kind\":5}").has_value());          // kind is not a string
    CHECK_FALSE(ipc::parseLine("{\"kind\":null}").has_value());
    CHECK_FALSE(ipc::parseLine("{\"kind\":\"ping\"").has_value());     // truncated
    CHECK_FALSE(ipc::parseLine("{\"kind\":\"ping\"} trailing").has_value());
    CHECK_FALSE(ipc::parseLine("42").has_value());
    CHECK_FALSE(ipc::parseLine("\"ping\"").has_value());
}

/**
 * @brief The field accessors return the value when it is there and the
 *        fallback (empty / given default) when it is missing or mistyped.
 */
HH_TEST(Ipc_accessorsFallBackOnMissingKeys) {
    ipc::json msg = ipc::json::object();
    msg["kind"] = "ping";
    msg["name"] = "clip \u00e9.mp4";
    msg["n"] = 3;
    msg["f"] = 2.5;
    msg["flag"] = true;
    msg["numText"] = "42";
    msg["boolText"] = "false";

    // Present.
    CHECK_EQ(ipc::kindOf(msg), "ping");
    CHECK_EQ(ipc::str(msg, "name"), "clip \u00e9.mp4");
    CHECK_WEQ(ipc::wstr(msg, "name"), L"clip \u00e9.mp4");
    CHECK_EQ(ipc::num(msg, "n"), 3.0);
    CHECK_EQ(ipc::num(msg, "f"), 2.5);
    CHECK(ipc::boolean(msg, "flag"));
    // Lenient conversions the panel relies on.
    CHECK_EQ(ipc::num(msg, "numText"), 42.0);
    CHECK_EQ(ipc::num(msg, "flag"), 1.0);
    CHECK_FALSE(ipc::boolean(msg, "boolText", true));
    CHECK(ipc::boolean(msg, "n"));

    // Missing keys.
    CHECK(ipc::str(msg, "missing").empty());
    CHECK(ipc::wstr(msg, "missing").empty());
    CHECK_EQ(ipc::num(msg, "missing"), 0.0);
    CHECK_EQ(ipc::num(msg, "missing", 42.5), 42.5);
    CHECK_FALSE(ipc::boolean(msg, "missing"));
    CHECK(ipc::boolean(msg, "missing", true));

    // Wrong types: a number is not a string; a word is not a number.
    CHECK(ipc::str(msg, "n").empty());
    CHECK(ipc::wstr(msg, "flag").empty());
    CHECK_EQ(ipc::num(msg, "name", -1.0), -1.0);
    CHECK(ipc::boolean(msg, "name", true));

    // Null or empty keys are tolerated.
    CHECK(ipc::str(msg, nullptr).empty());
    CHECK(ipc::str(msg, "").empty());
    CHECK_EQ(ipc::num(msg, nullptr, 9.0), 9.0);

    // Non-object messages never crash and report nothing.
    const ipc::json arr = ipc::json::array();
    CHECK(ipc::kindOf(arr).empty());
    CHECK(ipc::str(arr, "kind").empty());
    CHECK_EQ(ipc::num(arr, "x", 7.0), 7.0);
    CHECK(ipc::boolean(arr, "x", true));
    const ipc::json nothing;
    CHECK(ipc::kindOf(nothing).empty());
}

/**
 * @brief line() serialises compactly with exactly one trailing newline, and
 *        what it writes parseLine() reads back.
 */
HH_TEST(Ipc_lineEndsWithNewline) {
    const std::string pong = ipc::line(ipc::makePong());
    CHECK_EQ(pong, "{\"kind\":\"pong\"}\n");
    CHECK(!pong.empty() && pong.back() == '\n');
    CHECK_EQ(pong.find('\n'), pong.size() - 1);

    const std::optional<ipc::json> back = ipc::parseLine(pong);
    if (CHECK(back.has_value())) {
        CHECK_EQ(ipc::kindOf(*back), "pong");
    }

    // A message with text keeps everything on one line.
    const std::string welcome = ipc::line(ipc::makeWelcome(L"1.0.0", false));
    CHECK(!welcome.empty() && welcome.back() == '\n');
    CHECK_EQ(welcome.find('\n'), welcome.size() - 1);
}

// ---- outbound builders -----------------------------------------------------------

/**
 * @brief makeWelcome carries the app version, docked flag and protocol
 *        number (1 by default).
 */
HH_TEST(Ipc_makeWelcome) {
    const ipc::json w = ipc::makeWelcome(L"1.0.0", true);
    CHECK_EQ(ipc::kindOf(w), "welcome");
    CHECK_EQ(ipc::str(w, "appVersion"), "1.0.0");
    CHECK(ipc::boolean(w, "docked"));
    CHECK_EQ(ipc::num(w, "protocol"), 1.0);

    // Undocked with an explicit protocol number.
    const ipc::json w2 = ipc::makeWelcome(L"2.3.4", false, 5);
    CHECK_EQ(ipc::kindOf(w2), "welcome");
    CHECK_EQ(ipc::str(w2, "appVersion"), "2.3.4");
    CHECK_FALSE(ipc::boolean(w2, "docked", true));
    CHECK_EQ(ipc::num(w2, "protocol"), 5.0);
}

/**
 * @brief makePong and makeRequestBounds are bare "kind" messages.
 */
HH_TEST(Ipc_makePongAndRequestBounds) {
    const ipc::json pong = ipc::makePong();
    CHECK_EQ(ipc::kindOf(pong), "pong");
    CHECK_EQ(pong.size(), 1);

    const ipc::json bounds = ipc::makeRequestBounds();
    CHECK_EQ(ipc::kindOf(bounds), "requestBounds");
    CHECK_EQ(bounds.size(), 1);
}

/**
 * @brief makeAck echoes the request id (number or string), reports ok and
 *        only carries "error" when there is text.
 */
HH_TEST(Ipc_makeAckEchoesRequestId) {
    ipc::json request = ipc::json::object();
    request["kind"] = "setHold";
    request["id"] = 7;

    // Success: id 7, ok true, no error member.
    const ipc::json ack = ipc::makeAck(request, true);
    CHECK_EQ(ipc::kindOf(ack), "ack");
    CHECK_EQ(ipc::num(ack, "id"), 7.0);
    CHECK(ipc::boolean(ack, "ok"));
    CHECK_FALSE(ack.contains("error"));

    // Failure with text.
    const ipc::json nack = ipc::makeAck(request, false, "job not found");
    CHECK_EQ(ipc::kindOf(nack), "ack");
    CHECK_EQ(ipc::num(nack, "id"), 7.0);
    CHECK_FALSE(ipc::boolean(nack, "ok", true));
    CHECK_EQ(ipc::str(nack, "error"), "job not found");

    // String ids are echoed verbatim.
    request["id"] = "req-42";
    const ipc::json strAck = ipc::makeAck(request, true);
    CHECK_EQ(ipc::str(strAck, "id"), "req-42");

    // No id on the request: no id on the ack.
    ipc::json noId = ipc::json::object();
    noId["kind"] = "ping";
    const ipc::json anon = ipc::makeAck(noId, true);
    CHECK_FALSE(anon.contains("id"));
    CHECK(ipc::boolean(anon, "ok"));

    // An id of the wrong type (object) is dropped rather than echoed.
    request["id"] = ipc::json::object();
    CHECK_FALSE(ipc::makeAck(request, true).contains("id"));
}

/**
 * @brief makeStatus nests the counters, the last job and the mkvmerge
 *        availability; negative counters are clamped to zero.
 */
HH_TEST(Ipc_makeStatus) {
    const ipc::json s = ipc::makeStatus(true, 2, 1, 1, 5, 0, L"clip.mp4", L"Muxing",
                                        L"mkvmerge v80.0 ('Roundabout') 64-bit", true);
    CHECK_EQ(ipc::kindOf(s), "status");
    CHECK(ipc::boolean(s, "docked"));

    if (const ipc::json* counts = objectAt(s, "counts"); CHECK(counts != nullptr)) {
        CHECK_EQ(ipc::num(*counts, "encoding"), 2.0);
        CHECK_EQ(ipc::num(*counts, "ready"), 1.0);
        CHECK_EQ(ipc::num(*counts, "muxing"), 1.0);
        CHECK_EQ(ipc::num(*counts, "done"), 5.0);
        CHECK_EQ(ipc::num(*counts, "failed"), 0.0);
    }
    if (const ipc::json* last = objectAt(s, "lastJob"); CHECK(last != nullptr)) {
        CHECK_EQ(ipc::str(*last, "name"), "clip.mp4");
        CHECK_EQ(ipc::str(*last, "state"), "Muxing");
    }
    if (const ipc::json* mkv = objectAt(s, "mkvmerge"); CHECK(mkv != nullptr)) {
        CHECK_EQ(ipc::str(*mkv, "version"), "mkvmerge v80.0 ('Roundabout') 64-bit");
        CHECK(ipc::boolean(*mkv, "ok"));
    }

    // Negative counters never reach the wire; empty strings are fine.
    const ipc::json s2 = ipc::makeStatus(false, -3, -1, 0, 0, 0, L"", L"", L"", false);
    CHECK_FALSE(ipc::boolean(s2, "docked", true));
    if (const ipc::json* counts = objectAt(s2, "counts"); CHECK(counts != nullptr)) {
        CHECK_EQ(ipc::num(*counts, "encoding", -9.0), 0.0);
        CHECK_EQ(ipc::num(*counts, "ready", -9.0), 0.0);
    }
    if (const ipc::json* mkv = objectAt(s2, "mkvmerge"); CHECK(mkv != nullptr)) {
        CHECK(ipc::str(*mkv, "version").empty());
        CHECK_FALSE(ipc::boolean(*mkv, "ok", true));
    }
}

/**
 * @brief jobToJson exposes id, state and transfer by name, the display name
 *        and the paths as UTF-8, and Unix-millisecond timestamps.
 */
HH_TEST(Ipc_jobToJson) {
    Job job = makeJob(5, JobState::Muxing, TransferKind::PQ, L"C:\\x\\clip.mp4");
    job.generation = 2;
    job.phase = L"Muxing";
    job.progress = 0.5f;
    job.hintPath = L"C:\\x\\clip_REC709_HINT.mkv";
    job.stateReason = L"";
    job.plan.presetId = L"generic_pq_1000";
    job.plan.suffix = L"_REC709_HINT";
    job.plan.lutPath = L"C:\\luts\\pq.cube";
    job.plan.attachLut = true;
    job.createdUtc = platform::unixMsToUtc(1789432106000);
    job.updatedUtc = platform::unixMsToUtc(1789432107000);

    const ipc::json j = ipc::jobToJson(job);
    CHECK_EQ(ipc::num(j, "id"), 5.0);
    CHECK_EQ(ipc::str(j, "state"), "Muxing");
    CHECK_EQ(ipc::str(j, "transfer"), "PQ");
    CHECK_EQ(ipc::str(j, "displayName"), "clip.mp4");
    CHECK_EQ(ipc::num(j, "generation"), 2.0);
    CHECK_EQ(ipc::str(j, "phase"), "Muxing");
    CHECK_NEAR(ipc::num(j, "progress"), 0.5, 1e-9);

    // Paths: UTF-8 on the wire, back to the same wide string.
    CHECK_EQ(ipc::str(j, "outputPath"), platform::toUtf8(L"C:\\x\\clip.mp4"));
    CHECK_WEQ(ipc::wstr(j, "outputPath"), L"C:\\x\\clip.mp4");
    CHECK_WEQ(ipc::wstr(j, "hintPath"), L"C:\\x\\clip_REC709_HINT.mkv");
    CHECK_WEQ(ipc::wstr(j, "key"), job.key);

    // Effective plan, no overrides yet.
    CHECK_EQ(ipc::str(j, "presetId"), "generic_pq_1000");
    CHECK_EQ(ipc::str(j, "suffix"), "_REC709_HINT");
    CHECK_WEQ(ipc::wstr(j, "lutPath"), L"C:\\luts\\pq.cube");
    CHECK(ipc::boolean(j, "attachLut"));

    // Timestamps as Unix milliseconds.
    CHECK_EQ(ipc::num(j, "createdUtc"), 1789432106000.0);
    CHECK_EQ(ipc::num(j, "updatedUtc"), 1789432107000.0);

    // Per-job overrides win over the captured plan.
    job.overrides.presetId = L"generic_pq_4000";
    job.overrides.attachLut = false;
    job.overrides.suffix = L"_X";
    job.overrides.lutPath = L"";
    const ipc::json j2 = ipc::jobToJson(job);
    CHECK_EQ(ipc::str(j2, "presetId"), "generic_pq_4000");
    CHECK_EQ(ipc::str(j2, "suffix"), "_X");
    CHECK(ipc::str(j2, "lutPath").empty());
    CHECK_FALSE(ipc::boolean(j2, "attachLut", true));

    // Other states and transfers use their canonical names.
    const ipc::json held = ipc::jobToJson(makeJob(6, JobState::Held, TransferKind::Unknown, L"C:\\x\\b.mp4"));
    CHECK_EQ(ipc::str(held, "state"), "Held");
    CHECK_EQ(ipc::str(held, "transfer"), "Unknown");
    const ipc::json skipped = ipc::jobToJson(makeJob(7, JobState::SkippedSdr, TransferKind::SDR, L"C:\\x\\c.mp4"));
    CHECK_EQ(ipc::str(skipped, "state"), "SkippedSdr");
    CHECK_EQ(ipc::str(skipped, "transfer"), "SDR");
}

/**
 * @brief Non-ASCII paths survive the UTF-8 encoding, both field-wise and
 *        through a full line() -> parseLine() trip.
 */
HH_TEST(Ipc_jobToJsonRoundTripsNonAsciiPaths) {
    const std::wstring path = L"D:\\Renders\\\u00c9pisode 04 \u65e5\u672c\u8a9e.mp4";
    const Job job = makeJob(9, JobState::Ready, TransferKind::HLG, path);

    const ipc::json j = ipc::jobToJson(job);
    CHECK_WEQ(ipc::wstr(j, "outputPath"), path);
    CHECK_WEQ(ipc::wstr(j, "displayName"), L"\u00c9pisode 04 \u65e5\u672c\u8a9e.mp4");

    // Serialise the whole jobs event and read it back.
    const std::optional<ipc::json> back = ipc::parseLine(ipc::line(ipc::makeJobsEvent({job})));
    if (!CHECK(back.has_value())) { return; }
    CHECK_EQ(ipc::kindOf(*back), "jobs");
    if (const ipc::json* jobs = arrayAt(*back, "jobs"); CHECK(jobs != nullptr)) {
        if (CHECK_EQ(jobs->size(), 1)) {
            const ipc::json& first = jobs->front();
            CHECK_EQ(ipc::num(first, "id"), 9.0);
            CHECK_WEQ(ipc::wstr(first, "outputPath"), path);
            CHECK_EQ(ipc::str(first, "transfer"), "HLG");
        }
    }
}

/**
 * @brief makeJobsEvent lists every job in order under "jobs"; an empty
 *        list gives an empty array, not a missing member.
 */
HH_TEST(Ipc_makeJobsEventListsEveryJob) {
    std::vector<Job> jobs;
    jobs.push_back(makeJob(1, JobState::Encoding, TransferKind::HLG, L"C:\\a\\one.mp4"));
    jobs.push_back(makeJob(2, JobState::Done, TransferKind::PQ, L"C:\\a\\two.mp4"));

    const ipc::json ev = ipc::makeJobsEvent(jobs);
    CHECK_EQ(ipc::kindOf(ev), "jobs");
    if (const ipc::json* arr = arrayAt(ev, "jobs"); CHECK(arr != nullptr)) {
        if (CHECK_EQ(arr->size(), 2)) {
            const ipc::json& first = arr->front();
            const ipc::json& second = arr->back();
            CHECK_EQ(ipc::num(first, "id"), 1.0);
            CHECK_EQ(ipc::str(first, "state"), "Encoding");
            CHECK_EQ(ipc::str(first, "displayName"), "one.mp4");
            CHECK_EQ(ipc::num(second, "id"), 2.0);
            CHECK_EQ(ipc::str(second, "state"), "Done");
            CHECK_EQ(ipc::str(second, "displayName"), "two.mp4");
        }
    }

    // Empty list.
    const ipc::json none = ipc::makeJobsEvent({});
    CHECK_EQ(ipc::kindOf(none), "jobs");
    if (const ipc::json* arr = arrayAt(none, "jobs"); CHECK(arr != nullptr)) {
        CHECK(arr->empty());
    }
}
