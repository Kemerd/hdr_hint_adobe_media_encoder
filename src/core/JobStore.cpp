// ---------------------------------------------------------------------------
// JobStore.cpp - in-memory job table with JSON persistence.
//
// The table is a plain std::map keyed by JobId. Everything in here runs on
// the UI thread, so there is no locking; the engine debounces save() through
// dirty()/touch().
//
// On-disk layout (jobs.json, UTF-8):
//
//   {
//     "version": 1,
//     "nextId":  42,
//     "jobs":    [ { ...one Job... }, ... ]
//   }
//
// Every Job field round-trips. Enums are stored by their toString() name so
// the file stays readable, 64-bit values are plain JSON numbers, wide strings
// are UTF-8 and the optional overrides are simply absent when not set.
// ---------------------------------------------------------------------------
#include "core/JobStore.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hh {

namespace {

using json = nlohmann::json;

/// Component tag for the log.
constexpr const wchar_t* kLog = L"JobStore";

/// Version written into the file; older/newer files are read best-effort.
constexpr int kStoreVersion = 1;

/// Hard cap on array sizes read back from disk (a corrupted or hostile file
/// must not be able to balloon memory).
constexpr size_t kMaxArrayItems = 2000;

/// Largest jobs file we are willing to parse (a few thousand jobs is < 10 MB).
constexpr uint64_t kMaxFileBytes = 64ull * 1024 * 1024;

// ---------------------------------------------------------------------------
// JSON write helpers
// ---------------------------------------------------------------------------

/**
 * @brief Stores a wide string under @p key as UTF-8.
 */
void putWide(json& obj, const char* key, std::wstring_view value) {
    if (!key || !*key) { return; }
    obj[key] = platform::toUtf8(value);
}

/**
 * @brief Stores a vector of wide strings as a JSON array of UTF-8 strings.
 */
void putWideArray(json& obj, const char* key, const std::vector<std::wstring>& values) {
    if (!key || !*key) { return; }
    json arr = json::array();
    for (const auto& v : values) {
        arr.push_back(platform::toUtf8(v));
    }
    obj[key] = std::move(arr);
}

/**
 * @brief Stores an enum by its toString() name (UTF-8).
 */
template <class E>
void putEnum(json& obj, const char* key, E value) {
    if (!key || !*key) { return; }
    const wchar_t* name = toString(value);
    obj[key] = platform::toUtf8(name ? name : L"");
}

/**
 * @brief Stores a float, replacing NaN/inf with zero so the file stays valid.
 */
void putFloat(json& obj, const char* key, float value) {
    if (!key || !*key) { return; }
    obj[key] = std::isfinite(value) ? static_cast<double>(value) : 0.0;
}

// ---------------------------------------------------------------------------
// JSON read helpers - every one of them tolerates a missing or mistyped field
// by leaving the output untouched and returning false.
// ---------------------------------------------------------------------------

/**
 * @brief Looks up @p key on an object; nullptr when absent or not an object.
 */
const json* field(const json& obj, const char* key) {
    if (!key || !*key || !obj.is_object()) { return nullptr; }
    const auto it = obj.find(key);
    if (it == obj.end()) { return nullptr; }
    return &*it;
}

/**
 * @brief Reads a UTF-8 string field into a wide string.
 */
bool getWide(const json& obj, const char* key, std::wstring& out) {
    const json* f = field(obj, key);
    if (!f || !f->is_string()) { return false; }
    out = platform::toWide(f->get_ref<const std::string&>());
    return true;
}

/**
 * @brief Reads an optional string field; the optional is engaged only when present.
 */
bool getOptionalWide(const json& obj, const char* key, std::optional<std::wstring>& out) {
    std::wstring value;
    if (!getWide(obj, key, value)) { return false; }
    out = std::move(value);
    return true;
}

/**
 * @brief Reads any JSON number as uint64 (negative and fractional values are clamped).
 */
bool getU64(const json& obj, const char* key, uint64_t& out) {
    const json* f = field(obj, key);
    if (!f || !f->is_number()) { return false; }

    // Fast path: the writer always produces unsigned integers for these.
    if (f->is_number_unsigned()) {
        out = f->get<uint64_t>();
        return true;
    }
    // A signed integer (hand-edited file): negatives clamp to zero.
    if (f->is_number_integer()) {
        const int64_t v = f->get<int64_t>();
        out = (v < 0) ? 0ull : static_cast<uint64_t>(v);
        return true;
    }
    // A float: clamp into range, NaN becomes zero.
    const double d = f->get<double>();
    if (!(d > 0.0)) {
        out = 0;
    } else if (d >= 18446744073709551615.0) {
        out = std::numeric_limits<uint64_t>::max();
    } else {
        out = static_cast<uint64_t>(d);
    }
    return true;
}

/**
 * @brief Reads a number as uint32 (clamped).
 */
bool getU32(const json& obj, const char* key, uint32_t& out) {
    uint64_t wide = 0;
    if (!getU64(obj, key, wide)) { return false; }
    constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
    out = static_cast<uint32_t>(wide > kMax ? kMax : wide);
    return true;
}

/**
 * @brief Reads a number as int (clamped to the int range).
 */
bool getInt(const json& obj, const char* key, int& out) {
    const json* f = field(obj, key);
    if (!f || !f->is_number()) { return false; }

    constexpr int64_t kMin = std::numeric_limits<int>::min();
    constexpr int64_t kMax = std::numeric_limits<int>::max();

    // Unsigned values only need an upper clamp.
    if (f->is_number_unsigned()) {
        const uint64_t v = f->get<uint64_t>();
        out = (v > static_cast<uint64_t>(kMax)) ? static_cast<int>(kMax) : static_cast<int>(v);
        return true;
    }
    // Signed integers clamp both ways.
    if (f->is_number_integer()) {
        const int64_t v = f->get<int64_t>();
        out = static_cast<int>(std::clamp(v, kMin, kMax));
        return true;
    }
    // Floats: NaN -> 0, otherwise clamp and truncate.
    const double d = f->get<double>();
    if (std::isnan(d)) {
        out = 0;
    } else {
        const double c = std::clamp(d, static_cast<double>(kMin), static_cast<double>(kMax));
        out = static_cast<int>(c);
    }
    return true;
}

/**
 * @brief Reads a number as double (NaN is rejected).
 */
bool getDouble(const json& obj, const char* key, double& out) {
    const json* f = field(obj, key);
    if (!f || !f->is_number()) { return false; }
    const double d = f->get<double>();
    if (std::isnan(d)) { return false; }
    out = d;
    return true;
}

/**
 * @brief Reads a number as float (clamped to the float range).
 */
bool getFloat(const json& obj, const char* key, float& out) {
    double d = 0.0;
    if (!getDouble(obj, key, d)) { return false; }
    constexpr double kMax = static_cast<double>(std::numeric_limits<float>::max());
    out = static_cast<float>(std::clamp(d, -kMax, kMax));
    return true;
}

/**
 * @brief Reads a boolean; numbers are accepted as non-zero = true.
 */
bool getBool(const json& obj, const char* key, bool& out) {
    const json* f = field(obj, key);
    if (!f) { return false; }
    if (f->is_boolean()) {
        out = f->get<bool>();
        return true;
    }
    if (f->is_number()) {
        out = f->get<double>() != 0.0;
        return true;
    }
    return false;
}

/**
 * @brief Reads an optional boolean; the optional is engaged only when present.
 */
bool getOptionalBool(const json& obj, const char* key, std::optional<bool>& out) {
    bool value = false;
    if (!getBool(obj, key, value)) { return false; }
    out = value;
    return true;
}

/**
 * @brief Reads an array of strings (non-string items are skipped, size capped).
 */
bool getWideArray(const json& obj, const char* key, std::vector<std::wstring>& out) {
    const json* f = field(obj, key);
    if (!f || !f->is_array()) { return false; }
    out.clear();
    for (const auto& item : *f) {
        if (out.size() >= kMaxArrayItems) { break; }
        if (item.is_string()) {
            out.push_back(platform::toWide(item.get_ref<const std::string&>()));
        }
    }
    return true;
}

/**
 * @brief Reads an enum stored by name through one of the *FromString parsers.
 *        Unknown names are logged and leave @p out untouched.
 */
template <class E, class Parser>
void getEnum(const json& obj, const char* key, E& out, Parser parse) {
    std::wstring name;
    if (!getWide(obj, key, name)) { return; }
    const std::optional<E> parsed = parse(name);
    if (!parsed) {
        HH_LOG_WARN(kLog, L"unknown {} value '{}' in jobs file, keeping default",
                    platform::toWide(key ? key : ""), name);
        return;
    }
    out = *parsed;
}

// ---------------------------------------------------------------------------
// Job <-> JSON object
// ---------------------------------------------------------------------------

/**
 * @brief Serialises every field of a Job into a JSON object.
 */
json jobToObject(const Job& job) {
    json o = json::object();

    // Identity and lifecycle.
    o["id"] = job.id;
    putWide(o, "key", job.key);
    o["generation"] = job.generation;
    putEnum(o, "state", job.state);
    putWide(o, "stateReason", job.stateReason);
    putWide(o, "phase", job.phase);
    putFloat(o, "progress", job.progress);

    // Paths and names.
    putWide(o, "outputPath", job.outputPath);
    putWide(o, "sourcePath", job.sourcePath);
    putWide(o, "hintPath", job.hintPath);
    putWide(o, "presetName", job.presetName);

    // Colour transfer knowledge.
    putEnum(o, "transfer", job.transfer);
    putWide(o, "transferSource", job.transferSource);
    {
        json v = json::object();
        v["width"] = job.video.width;
        v["height"] = job.video.height;
        v["fps"] = std::isfinite(job.video.fps) ? job.video.fps : 0.0;
        putEnum(v, "transfer", job.video.transfer);
        v["hardwareEncoding"] = job.video.hardwareEncoding;
        putWide(v, "vendor", job.video.vendor);
        putWide(v, "codecHint", job.video.codecHint);
        putWide(v, "raw", job.video.raw);
        o["video"] = std::move(v);
    }
    o["inbandHdr10"] = job.inbandHdr10;

    // Origin and user choices. Optionals are only written when engaged;
    // an engaged-but-empty lutPath is meaningful ("no LUT"), so it stays.
    putEnum(o, "source", job.source);
    {
        json ov = json::object();
        if (job.overrides.lutPath) { putWide(ov, "lutPath", *job.overrides.lutPath); }
        if (job.overrides.presetId) { putWide(ov, "presetId", *job.overrides.presetId); }
        if (job.overrides.attachLut) { ov["attachLut"] = *job.overrides.attachLut; }
        if (job.overrides.suffix) { putWide(ov, "suffix", *job.overrides.suffix); }
        ov["hold"] = job.overrides.hold;
        o["overrides"] = std::move(ov);
    }

    // The resolved plan captured at dispatch time.
    {
        json p = json::object();
        putWide(p, "presetId", job.plan.presetId);
        putWide(p, "lutPath", job.plan.lutPath);
        p["attachLut"] = job.plan.attachLut;
        putWide(p, "suffix", job.plan.suffix);
        putWide(p, "hintPath", job.plan.hintPath);
        putWide(p, "error", job.plan.error);
        o["plan"] = std::move(p);
    }

    // Timestamps (FILETIME ticks).
    o["createdUtc"] = job.createdUtc;
    o["updatedUtc"] = job.updatedUtc;
    o["encodeStartUtc"] = job.encodeStartUtc;
    o["readyUtc"] = job.readyUtc;
    o["muxStartUtc"] = job.muxStartUtc;
    o["doneUtc"] = job.doneUtc;

    // Identity of the source file when it became Ready.
    {
        json s = json::object();
        s["size"] = job.sourceStamp.size;
        s["lastWriteUtc"] = job.sourceStamp.lastWriteUtc;
        s["creationUtc"] = job.sourceStamp.creationUtc;
        s["fileIdLow"] = job.sourceStamp.fileIdLow;
        s["fileIdHigh"] = job.sourceStamp.fileIdHigh;
        s["volumeSerial"] = job.sourceStamp.volumeSerial;
        s["hasFileId"] = job.sourceStamp.hasFileId;
        s["valid"] = job.sourceStamp.valid;
        o["sourceStamp"] = std::move(s);
    }

    // What mkvmerge did.
    {
        json m = json::object();
        putWide(m, "commandLine", job.mux.commandLine);
        m["exitCode"] = static_cast<uint32_t>(job.mux.exitCode);
        putWideArray(m, "warnings", job.mux.warnings);
        putWideArray(m, "lastLines", job.mux.lastLines);
        putWide(m, "errorText", job.mux.errorText);
        m["outputSize"] = job.mux.outputSize;
        m["durationMs"] = job.mux.durationMs;
        o["mux"] = std::move(m);
    }

    // Recycle outcome, notes and confirmation flags.
    putEnum(o, "recycle", job.recycle);
    putWide(o, "recycleMessage", job.recycleMessage);
    putWideArray(o, "notes", job.notes);
    o["logConfirmed"] = job.logConfirmed;
    o["cepConfirmed"] = job.cepConfirmed;
    o["sidecarBytes"] = job.sidecarBytes;

    return o;
}

/**
 * @brief Fills a Job from a JSON object. Missing fields keep whatever @p job
 *        already holds, so callers pass a default-constructed Job.
 * @return false when @p o is not a JSON object at all.
 */
bool jobFromObject(const json& o, Job& job) {
    if (!o.is_object()) { return false; }

    // Identity and lifecycle.
    getU64(o, "id", job.id);
    getWide(o, "key", job.key);
    getU32(o, "generation", job.generation);
    if (job.generation == 0) { job.generation = 1; }
    getEnum(o, "state", job.state, jobStateFromString);
    getWide(o, "stateReason", job.stateReason);
    getWide(o, "phase", job.phase);
    getFloat(o, "progress", job.progress);
    job.progress = std::clamp(job.progress, 0.0f, 1.0f);

    // Paths and names.
    getWide(o, "outputPath", job.outputPath);
    getWide(o, "sourcePath", job.sourcePath);
    getWide(o, "hintPath", job.hintPath);
    getWide(o, "presetName", job.presetName);

    // Colour transfer knowledge.
    getEnum(o, "transfer", job.transfer, transferKindFromString);
    getWide(o, "transferSource", job.transferSource);
    if (const json* v = field(o, "video"); v && v->is_object()) {
        getInt(*v, "width", job.video.width);
        getInt(*v, "height", job.video.height);
        getDouble(*v, "fps", job.video.fps);
        getEnum(*v, "transfer", job.video.transfer, transferKindFromString);
        getBool(*v, "hardwareEncoding", job.video.hardwareEncoding);
        getWide(*v, "vendor", job.video.vendor);
        getWide(*v, "codecHint", job.video.codecHint);
        getWide(*v, "raw", job.video.raw);
    }
    getBool(o, "inbandHdr10", job.inbandHdr10);

    // Origin and user choices.
    getEnum(o, "source", job.source, jobSourceFromString);
    if (const json* ov = field(o, "overrides"); ov && ov->is_object()) {
        getOptionalWide(*ov, "lutPath", job.overrides.lutPath);
        getOptionalWide(*ov, "presetId", job.overrides.presetId);
        getOptionalBool(*ov, "attachLut", job.overrides.attachLut);
        getOptionalWide(*ov, "suffix", job.overrides.suffix);
        getBool(*ov, "hold", job.overrides.hold);
    }

    // The resolved plan.
    if (const json* p = field(o, "plan"); p && p->is_object()) {
        getWide(*p, "presetId", job.plan.presetId);
        getWide(*p, "lutPath", job.plan.lutPath);
        getBool(*p, "attachLut", job.plan.attachLut);
        getWide(*p, "suffix", job.plan.suffix);
        getWide(*p, "hintPath", job.plan.hintPath);
        getWide(*p, "error", job.plan.error);
    }

    // Timestamps.
    getU64(o, "createdUtc", job.createdUtc);
    getU64(o, "updatedUtc", job.updatedUtc);
    getU64(o, "encodeStartUtc", job.encodeStartUtc);
    getU64(o, "readyUtc", job.readyUtc);
    getU64(o, "muxStartUtc", job.muxStartUtc);
    getU64(o, "doneUtc", job.doneUtc);

    // Source identity.
    if (const json* s = field(o, "sourceStamp"); s && s->is_object()) {
        getU64(*s, "size", job.sourceStamp.size);
        getU64(*s, "lastWriteUtc", job.sourceStamp.lastWriteUtc);
        getU64(*s, "creationUtc", job.sourceStamp.creationUtc);
        getU64(*s, "fileIdLow", job.sourceStamp.fileIdLow);
        getU64(*s, "fileIdHigh", job.sourceStamp.fileIdHigh);
        getU32(*s, "volumeSerial", job.sourceStamp.volumeSerial);
        getBool(*s, "hasFileId", job.sourceStamp.hasFileId);
        getBool(*s, "valid", job.sourceStamp.valid);
    }

    // Mux record.
    if (const json* m = field(o, "mux"); m && m->is_object()) {
        getWide(*m, "commandLine", job.mux.commandLine);
        uint32_t exitCode = static_cast<uint32_t>(job.mux.exitCode);
        if (getU32(*m, "exitCode", exitCode)) { job.mux.exitCode = static_cast<DWORD>(exitCode); }
        getWideArray(*m, "warnings", job.mux.warnings);
        getWideArray(*m, "lastLines", job.mux.lastLines);
        getWide(*m, "errorText", job.mux.errorText);
        getU64(*m, "outputSize", job.mux.outputSize);
        getU64(*m, "durationMs", job.mux.durationMs);
    }

    // Recycle outcome, notes and confirmation flags.
    getEnum(o, "recycle", job.recycle, recycleStatusFromString);
    getWide(o, "recycleMessage", job.recycleMessage);
    getWideArray(o, "notes", job.notes);
    getBool(o, "logConfirmed", job.logConfirmed);
    getBool(o, "cepConfirmed", job.cepConfirmed);
    getU64(o, "sidecarBytes", job.sidecarBytes);

    return true;
}

/**
 * @brief Normalises a folder for comparison: backslashes only, no trailing
 *        separator (a bare drive root such as "C:\" is kept intact).
 */
std::wstring normaliseFolder(std::wstring_view folder) {
    std::wstring f = path::normalizeSeparators(folder);
    while (f.size() > 3 && f.back() == L'\\') {
        f.pop_back();
    }
    return f;
}

/**
 * @brief Ordering used by all(): newest first, ties broken by the higher id.
 */
bool newerFirst(const Job* a, const Job* b) noexcept {
    if (!a || !b) { return a != nullptr; }
    if (a->createdUtc != b->createdUtc) { return a->createdUtc > b->createdUtc; }
    return a->id > b->id;
}

/**
 * @brief Picks the "better" of two candidates for a key: higher generation,
 *        then the higher id.
 */
bool outranks(const Job& candidate, const Job* best) noexcept {
    if (!best) { return true; }
    if (candidate.generation != best->generation) { return candidate.generation > best->generation; }
    return candidate.id > best->id;
}

} // namespace

// ---------------------------------------------------------------------------
// Creation and lookup
// ---------------------------------------------------------------------------

/**
 * @brief Creates a job with a fresh id.
 *
 * The generation continues from any existing job with the same key: a
 * terminal predecessor bumps it by one (the file was exported again), an
 * active one shares its generation, and a brand-new key starts at 1.
 */
Job& JobStore::create(const std::wstring& key, const std::wstring& outputPath, JobSource source) {
    // Empty inputs are almost certainly a caller bug; the row is still created
    // (the API cannot fail) but the log makes the mistake visible.
    if (key.empty()) { HH_LOG_WARN(kLog, L"create: empty key for '{}'", outputPath); }
    if (outputPath.empty()) { HH_LOG_WARN(kLog, L"create: empty output path for key '{}'", key); }

    // Work out the generation from the previous job with the same key.
    uint32_t generation = 1;
    if (const Job* previous = findByKey(key)) {
        generation = previous->isTerminal()
            ? (previous->generation < std::numeric_limits<uint32_t>::max() ? previous->generation + 1 : previous->generation)
            : previous->generation;
    }

    // Allocate an id that is guaranteed unused (nextId_ normally is, but a
    // hand-edited jobs file could have left a collision behind).
    if (nextId_ == 0) { nextId_ = 1; }
    while (jobs_.contains(nextId_)) { ++nextId_; }
    const JobId id = nextId_++;

    // Fill the row.
    Job job;
    job.id = id;
    job.key = key;
    job.generation = generation;
    job.outputPath = outputPath;
    job.source = source;
    job.createdUtc = platform::nowUtc();
    job.updatedUtc = job.createdUtc;

    auto [it, inserted] = jobs_.emplace(id, std::move(job));
    if (!inserted) {
        // Cannot happen after the loop above, but never hand back a stale row silently.
        HH_LOG_ERROR(kLog, L"create: id {} already present, replacing", id);
        it->second = Job{};
        it->second.id = id;
        it->second.key = key;
        it->second.generation = generation;
        it->second.outputPath = outputPath;
        it->second.source = source;
        it->second.createdUtc = platform::nowUtc();
        it->second.updatedUtc = it->second.createdUtc;
    }

    dirty_ = true;
    HH_LOG_DEBUG(kLog, L"created job {} gen {} for '{}'", id, generation, outputPath);
    return it->second;
}

/**
 * @brief Looks a job up by id.
 */
Job* JobStore::find(JobId id) noexcept {
    if (id == 0) { return nullptr; }
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : &it->second;
}

/**
 * @brief Looks a job up by id (const).
 */
const Job* JobStore::find(JobId id) const noexcept {
    if (id == 0) { return nullptr; }
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : &it->second;
}

/**
 * @brief Newest job (highest generation, then id) with the key, any state.
 */
Job* JobStore::findByKey(std::wstring_view key) noexcept {
    if (key.empty()) { return nullptr; }
    Job* best = nullptr;
    for (auto& entry : jobs_) {
        Job& job = entry.second;
        if (job.key != key) { continue; }
        if (outranks(job, best)) { best = &job; }
    }
    return best;
}

/**
 * @brief Newest non-terminal job with the key.
 */
Job* JobStore::findActiveByKey(std::wstring_view key) noexcept {
    if (key.empty()) { return nullptr; }
    Job* best = nullptr;
    for (auto& entry : jobs_) {
        Job& job = entry.second;
        if (job.key != key || job.isTerminal()) { continue; }
        if (outranks(job, best)) { best = &job; }
    }
    return best;
}

/**
 * @brief Active job whose output lives in @p folder with file stem @p stem.
 *        Both comparisons are case-insensitive (Windows file systems are).
 */
Job* JobStore::findActiveByStem(std::wstring_view folder, std::wstring_view stem) noexcept {
    if (folder.empty() || stem.empty()) { return nullptr; }
    const std::wstring wantFolder = normaliseFolder(folder);

    Job* best = nullptr;
    for (auto& entry : jobs_) {
        Job& job = entry.second;
        if (job.isTerminal() || job.outputPath.empty()) { continue; }

        // Cheap check first: the stem, then the (normalised) parent folder.
        if (!platform::iequals(path::stem(job.outputPath), stem)) { continue; }
        if (!platform::iequals(normaliseFolder(path::parent(job.outputPath)), wantFolder)) { continue; }
        if (outranks(job, best)) { best = &job; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Enumeration and removal
// ---------------------------------------------------------------------------

/**
 * @brief All jobs, newest first.
 */
std::vector<Job*> JobStore::all() {
    std::vector<Job*> out;
    out.reserve(jobs_.size());
    for (auto& entry : jobs_) {
        out.push_back(&entry.second);
    }
    std::sort(out.begin(), out.end(), newerFirst);
    return out;
}

/**
 * @brief All jobs, newest first (const).
 */
std::vector<const Job*> JobStore::all() const {
    std::vector<const Job*> out;
    out.reserve(jobs_.size());
    for (const auto& entry : jobs_) {
        out.push_back(&entry.second);
    }
    std::sort(out.begin(), out.end(), newerFirst);
    return out;
}

/**
 * @brief Removes a job; false when the id is unknown.
 */
bool JobStore::remove(JobId id) {
    if (id == 0) { return false; }
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) { return false; }
    jobs_.erase(it);
    dirty_ = true;
    HH_LOG_DEBUG(kLog, L"removed job {}", id);
    return true;
}

/**
 * @brief Keeps at most @p historyMax terminal jobs, dropping the oldest first.
 *        Active jobs are never touched.
 */
void JobStore::trim(size_t historyMax) {
    // Collect the terminal rows; nothing to do while they fit.
    std::vector<const Job*> terminal;
    for (const auto& entry : jobs_) {
        if (entry.second.isTerminal()) { terminal.push_back(&entry.second); }
    }
    if (terminal.size() <= historyMax) { return; }

    // Oldest first: sort newest-first and walk from the back.
    std::sort(terminal.begin(), terminal.end(), newerFirst);
    const size_t excess = terminal.size() - historyMax;

    // Snapshot the ids before erasing (pointers die with the rows).
    std::vector<JobId> victims;
    victims.reserve(excess);
    for (size_t i = 0; i < excess && i < terminal.size(); ++i) {
        const Job* job = terminal[terminal.size() - 1 - i];
        if (job) { victims.push_back(job->id); }
    }
    for (const JobId id : victims) {
        jobs_.erase(id);
    }

    if (!victims.empty()) {
        dirty_ = true;
        HH_LOG_INFO(kLog, L"trimmed {} old job(s) (history max {})", victims.size(), historyMax);
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

/**
 * @brief Replaces the table with the contents of @p path.
 *
 * A missing file is an empty store (first run). Jobs that were mid-mux when
 * the app last exited cannot be resumed and are marked Failed; jobs that were
 * still encoding or waiting keep their state so the engine can re-probe them.
 */
Result<void> JobStore::load(const std::wstring& path) {
    if (path.empty()) { return Error::text(L"JobStore::load: empty path"); }

    // First run: nothing on disk yet.
    if (!platform::isFile(path)) {
        HH_LOG_INFO(kLog, L"no jobs file at '{}' (starting empty)", path);
        jobs_.clear();
        dirty_ = false;
        return Result<void>::success();
    }

    // Slurp the file.
    auto bytes = platform::readAll(path, kMaxFileBytes);
    if (!bytes) {
        HH_LOG_ERROR(kLog, L"cannot read '{}': {}", path, bytes.error().toString());
        return bytes.error();
    }
    const std::vector<uint8_t>& buf = bytes.value();
    std::string_view text(reinterpret_cast<const char*>(buf.data()), buf.size());

    // Tolerate a UTF-8 BOM left behind by an editor.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    // An empty file is treated like a missing one rather than as corruption.
    if (platform::trim(text).empty()) {
        HH_LOG_WARN(kLog, L"jobs file '{}' is empty (starting empty)", path);
        jobs_.clear();
        dirty_ = false;
        return Result<void>::success();
    }

    // Parse without exceptions; anything but an object is corruption.
    const json root = json::parse(text.begin(), text.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        HH_LOG_ERROR(kLog, L"jobs file '{}' is not valid JSON", path);
        return Error::text(L"jobs file is not valid JSON: " + path);
    }

    // Header fields. A newer version is read best-effort (unknown fields ignored).
    int version = 0;
    getInt(root, "version", version);
    if (version > kStoreVersion) {
        HH_LOG_WARN(kLog, L"jobs file version {} is newer than {}, reading best-effort", version, kStoreVersion);
    }
    uint64_t storedNext = 0;
    getU64(root, "nextId", storedNext);

    // Rebuild the table.
    jobs_.clear();
    uint64_t maxId = 0;
    size_t skipped = 0;
    size_t interrupted = 0;
    const uint64_t now = platform::nowUtc();

    if (const json* arr = field(root, "jobs"); arr && arr->is_array()) {
        for (const json& item : *arr) {
            Job job;
            if (!jobFromObject(item, job) || job.id == 0) {
                ++skipped;
                continue;
            }
            if (jobs_.contains(job.id)) {
                HH_LOG_WARN(kLog, L"duplicate job id {} in jobs file, keeping the first", job.id);
                ++skipped;
                continue;
            }

            // A mux cannot survive a restart: mkvmerge is gone and its partial
            // output has been abandoned, so the row becomes Failed.
            if (job.state == JobState::Muxing || job.state == JobState::Verifying) {
                job.state = JobState::Failed;
                job.stateReason = L"Interrupted by app exit";
                job.phase.clear();
                job.progress = 0.0f;
                job.updatedUtc = now;
                ++interrupted;
            }

            maxId = std::max(maxId, job.id);
            jobs_.emplace(job.id, std::move(job));
        }
    } else {
        HH_LOG_WARN(kLog, L"jobs file '{}' has no \"jobs\" array", path);
    }

    // Continue numbering above everything we have seen.
    const uint64_t afterMax = (maxId < std::numeric_limits<uint64_t>::max()) ? maxId + 1 : maxId;
    nextId_ = std::max({storedNext, afterMax, uint64_t{1}});

    // Only the fix-ups need writing back.
    dirty_ = interrupted > 0;
    HH_LOG_INFO(kLog, L"loaded {} job(s) from '{}' (skipped {}, interrupted {}, nextId {})",
                jobs_.size(), path, skipped, interrupted, nextId_);
    return Result<void>::success();
}

/**
 * @brief Writes the whole table atomically to @p path.
 */
Result<void> JobStore::save(const std::wstring& path) {
    if (path.empty()) { return Error::text(L"JobStore::save: empty path"); }

    // Build the document.
    json root = json::object();
    root["version"] = kStoreVersion;
    root["nextId"] = nextId_;
    json arr = json::array();
    for (const auto& entry : jobs_) {
        arr.push_back(jobToObject(entry.second));
    }
    root["jobs"] = std::move(arr);

    // Indented output keeps the file readable in bug reports; the replace
    // handler guarantees dump() never throws on odd bytes.
    const std::string text = root.dump(2, ' ', false, json::error_handler_t::replace);

    // Make sure the folder exists (a fresh %APPDATA% profile has none).
    const std::wstring dir = path::parent(path);
    if (!dir.empty() && !platform::isDirectory(dir)) {
        const auto made = platform::createDirectories(dir);
        if (!made) {
            HH_LOG_WARN(kLog, L"cannot create '{}': {}", dir, made.error().toString());
        }
    }

    // Temp file + rename so a crash mid-write never leaves a torn file.
    const auto written = platform::writeAllAtomic(path, text);
    if (!written) {
        HH_LOG_ERROR(kLog, L"cannot write '{}': {}", path, written.error().toString());
        return written.error();
    }

    dirty_ = false;
    HH_LOG_DEBUG(kLog, L"saved {} job(s) to '{}' ({} bytes)", jobs_.size(), path, text.size());
    return Result<void>::success();
}

// ---------------------------------------------------------------------------
// Single-job JSON (used by the IPC layer)
// ---------------------------------------------------------------------------

/**
 * @brief One job as compact UTF-8 JSON.
 */
std::string JobStore::toJson(const Job& job) {
    return jobToObject(job).dump(-1, ' ', false, json::error_handler_t::replace);
}

/**
 * @brief Parses one job; false when the text is not a JSON object.
 */
bool JobStore::fromJson(std::string_view text, Job& job) {
    const std::string_view trimmed = platform::trim(text);
    if (trimmed.empty() || trimmed.front() != '{') { return false; }
    const json o = json::parse(trimmed.begin(), trimmed.end(), nullptr, false);
    if (o.is_discarded()) { return false; }
    return jobFromObject(o, job);
}

} // namespace hh
