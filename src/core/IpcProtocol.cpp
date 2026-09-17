// ---------------------------------------------------------------------------
// IpcProtocol.cpp - newline-delimited JSON between the CEP panel and HdrHint.
//
// Every message is one JSON object per line with a "kind" discriminator. The
// parser never throws: malformed input becomes std::nullopt and every field
// accessor tolerates a missing or mistyped value.
//
// Outbound shapes (all UTF-8, one line each):
//   {"kind":"welcome","appVersion":"1.0.0","protocol":1,"docked":true}
//   {"kind":"pong"}
//   {"kind":"ack","id":<request id>,"ok":true,"error":"..."}
//   {"kind":"requestBounds"}
//   {"kind":"status","docked":..,"counts":{...},"lastJob":{...},"mkvmerge":{...}}
//   {"kind":"jobs","jobs":[ {...}, ... ]}
// ---------------------------------------------------------------------------
#include "core/IpcProtocol.h"

#include "core/Logger.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hh::ipc {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"IpcProtocol";

/// Largest single line we are willing to parse (the server already caps its
/// inbound buffer at 1 MiB; this is a second fence for other callers).
constexpr size_t kMaxLineBytes = 4u * 1024u * 1024u;

/**
 * @brief Looks up @p key on an object message; nullptr when absent.
 */
const json* fieldOf(const json& msg, const char* key) {
    if (!key || !*key || !msg.is_object()) { return nullptr; }
    const auto it = msg.find(key);
    if (it == msg.end()) { return nullptr; }
    return &*it;
}

/**
 * @brief Stores a wide string as UTF-8 under @p key.
 */
void put(json& obj, const char* key, std::wstring_view value) {
    if (!key || !*key) { return; }
    obj[key] = platform::toUtf8(value);
}

/**
 * @brief Stores an enum by its toString() name.
 */
template <class E>
void putEnum(json& obj, const char* key, E value) {
    if (!key || !*key) { return; }
    const wchar_t* name = toString(value);
    obj[key] = platform::toUtf8(name ? name : L"");
}

/**
 * @brief Serialises with the replace handler so odd bytes never throw.
 */
std::string dumpCompact(const json& msg) {
    return msg.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

/**
 * @brief Parses one line into a message.
 *
 * The line must (after trimming) start with '{', parse as a JSON object and
 * carry a non-empty string "kind". Anything else yields std::nullopt.
 */
std::optional<json> parseLine(std::string_view line) {
    const std::string_view text = platform::trim(line);

    // Cheap rejections before touching the parser.
    if (text.empty() || text.front() != '{') { return std::nullopt; }
    if (text.size() > kMaxLineBytes) {
        HH_LOG_WARN(kLog, L"dropping oversized line ({} bytes)", text.size());
        return std::nullopt;
    }

    // Exceptions are disabled: a bad document comes back as "discarded".
    json msg = json::parse(text.begin(), text.end(), nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) { return std::nullopt; }

    // The discriminator is mandatory.
    const json* kind = fieldOf(msg, "kind");
    if (!kind || !kind->is_string() || kind->get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    return msg;
}

/**
 * @brief "kind" of a message, or empty.
 */
std::string kindOf(const json& msg) {
    return str(msg, "kind");
}

/**
 * @brief String field, or empty when missing / not a string.
 */
std::string str(const json& msg, const char* key) {
    const json* f = fieldOf(msg, key);
    if (!f || !f->is_string()) { return {}; }
    return f->get<std::string>();
}

/**
 * @brief String field as UTF-16, or empty.
 */
std::wstring wstr(const json& msg, const char* key) {
    return platform::toWide(str(msg, key));
}

/**
 * @brief Numeric field. Numeric strings are accepted too; anything else,
 *        including NaN, yields @p fallback.
 */
double num(const json& msg, const char* key, double fallback) {
    const json* f = fieldOf(msg, key);
    if (!f) { return fallback; }

    // Plain numbers (int, unsigned, float) all convert.
    if (f->is_number()) {
        const double v = f->get<double>();
        return std::isnan(v) ? fallback : v;
    }
    // Booleans are occasionally sent for flags that later became counters.
    if (f->is_boolean()) { return f->get<bool>() ? 1.0 : 0.0; }
    // "42" from a form field.
    if (f->is_string()) {
        const auto parsed = platform::parseDouble(platform::toWide(f->get_ref<const std::string&>()));
        if (parsed && !std::isnan(*parsed)) { return *parsed; }
    }
    return fallback;
}

/**
 * @brief Boolean field. Numbers (non-zero) and "true"/"false"-style strings
 *        are accepted; anything else yields @p fallback.
 */
bool boolean(const json& msg, const char* key, bool fallback) {
    const json* f = fieldOf(msg, key);
    if (!f) { return fallback; }
    if (f->is_boolean()) { return f->get<bool>(); }
    if (f->is_number()) { return f->get<double>() != 0.0; }
    if (f->is_string()) {
        const auto parsed = platform::parseBool(platform::toWide(f->get_ref<const std::string&>()));
        if (parsed) { return *parsed; }
    }
    return fallback;
}

/**
 * @brief Serialises a message as one line with a trailing newline.
 */
std::string line(const json& msg) {
    std::string out = dumpCompact(msg);
    out.push_back('\n');
    return out;
}

// ---------------------------------------------------------------------------
// Outbound builders
// ---------------------------------------------------------------------------

/**
 * @brief First message after a client connects.
 */
json makeWelcome(const std::wstring& appVersion, bool docked, int protocol) {
    json msg = json::object();
    msg["kind"] = "welcome";
    put(msg, "appVersion", appVersion);
    msg["protocol"] = protocol;
    msg["docked"] = docked;
    return msg;
}

/**
 * @brief Reply to a "ping".
 */
json makePong() {
    json msg = json::object();
    msg["kind"] = "pong";
    return msg;
}

/**
 * @brief Acknowledges a request, echoing its "id" when it carried one.
 *        "error" is only present when there is text to report.
 */
json makeAck(const json& request, bool ok, const std::string& error) {
    json msg = json::object();
    msg["kind"] = "ack";

    // Echo a string or numeric id verbatim so the panel can pair the reply.
    if (const json* id = fieldOf(request, "id"); id && (id->is_string() || id->is_number())) {
        msg["id"] = *id;
    }
    msg["ok"] = ok;
    if (!error.empty()) { msg["error"] = error; }
    return msg;
}

/**
 * @brief Asks the panel to report its window bounds (for docking).
 */
json makeRequestBounds() {
    json msg = json::object();
    msg["kind"] = "requestBounds";
    return msg;
}

/**
 * @brief Summary of the engine for the panel's header.
 */
json makeStatus(bool docked, int encoding, int ready, int muxing, int done, int failed,
                const std::wstring& lastJobName, const std::wstring& lastJobState,
                const std::wstring& mkvmergeVersion, bool mkvmergeOk) {
    json msg = json::object();
    msg["kind"] = "status";
    msg["docked"] = docked;

    // Counters never go negative on the wire.
    json counts = json::object();
    counts["encoding"] = encoding < 0 ? 0 : encoding;
    counts["ready"] = ready < 0 ? 0 : ready;
    counts["muxing"] = muxing < 0 ? 0 : muxing;
    counts["done"] = done < 0 ? 0 : done;
    counts["failed"] = failed < 0 ? 0 : failed;
    msg["counts"] = std::move(counts);

    // The most recent job, for the one-line summary.
    json lastJob = json::object();
    put(lastJob, "name", lastJobName);
    put(lastJob, "state", lastJobState);
    msg["lastJob"] = std::move(lastJob);

    // Tool availability.
    json mkv = json::object();
    put(mkv, "version", mkvmergeVersion);
    mkv["ok"] = mkvmergeOk;
    msg["mkvmerge"] = std::move(mkv);

    return msg;
}

/**
 * @brief Full job list for the panel.
 */
json makeJobsEvent(const std::vector<Job>& jobs) {
    json msg = json::object();
    msg["kind"] = "jobs";
    json arr = json::array();
    for (const Job& job : jobs) {
        arr.push_back(jobToJson(job));
    }
    msg["jobs"] = std::move(arr);
    return msg;
}

/**
 * @brief The panel-facing view of one job.
 *
 * Plan-related fields show the effective value: a per-job override wins,
 * otherwise the plan captured at dispatch (empty before the first dispatch).
 * Timestamps are Unix milliseconds because that is what JavaScript wants.
 */
json jobToJson(const Job& job) {
    json o = json::object();

    // Identity and state.
    o["id"] = job.id;
    put(o, "key", job.key);
    o["generation"] = job.generation;
    putEnum(o, "state", job.state);
    put(o, "stateReason", job.stateReason);
    put(o, "phase", job.phase);
    o["progress"] = std::isfinite(job.progress) ? static_cast<double>(job.progress) : 0.0;

    // Paths and display.
    put(o, "outputPath", job.outputPath);
    put(o, "hintPath", job.hintPath);
    put(o, "displayName", job.displayName());
    putEnum(o, "transfer", job.transfer);

    // Effective plan: override first, then the resolved plan.
    put(o, "presetId", job.overrides.presetId ? *job.overrides.presetId : job.plan.presetId);
    put(o, "lutPath", job.overrides.lutPath ? *job.overrides.lutPath : job.plan.lutPath);
    o["attachLut"] = job.overrides.attachLut ? *job.overrides.attachLut : job.plan.attachLut;
    put(o, "suffix", job.overrides.suffix ? *job.overrides.suffix : job.plan.suffix);

    // Timestamps as Unix milliseconds.
    o["createdUtc"] = platform::utcToUnixMs(job.createdUtc);
    o["updatedUtc"] = platform::utcToUnixMs(job.updatedUtc);

    return o;
}

} // namespace hh::ipc
