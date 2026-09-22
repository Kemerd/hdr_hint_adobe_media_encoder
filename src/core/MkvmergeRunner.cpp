// ---------------------------------------------------------------------------
// MkvmergeRunner.cpp - identification (-J), argv building, muxing, verification.
//
// Everything here runs on the mux worker thread. The runner never touches UI
// state; progress is reported through the callback the caller hands in and
// every failure comes back as a value (MuxRunResult / Result<T>).
// ---------------------------------------------------------------------------
#include "core/MkvmergeRunner.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Process.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace hh {

namespace {

using json = nlohmann::json;

/// Component tag used for every log line in this file.
constexpr const wchar_t* kLog = L"MkvmergeRunner";

/// How many trailing output lines we keep for diagnostics.
constexpr size_t kLastLinesCap = 200;

/// Pipe read wait per loop iteration (also the watchdog tick).
constexpr DWORD kReadWaitMs = 250;

/// How long we give mkvmerge to die after terminate() before moving on.
constexpr DWORD kTerminateWaitMs = 5000;

/// Upper bound for a progress percentage scanner (guards against junk digits).
constexpr size_t kMaxPercentDigits = 4;

/// Mime type used when the plan carries an empty one.
constexpr const wchar_t* kDefaultAttachmentMime = L"application/x-cube";

// ---- JSON access helpers ----------------------------------------------------
// mkvmerge's JSON is well-formed in practice, but every accessor below still
// type-checks so a future format change degrades to "field missing" instead
// of a crash. Numbers may arrive as integers or floats depending on the field.

/**
 * @brief Reads a string member as UTF-16, or @p fallback when absent/not a string.
 */
std::wstring jsonString(const json& obj, const char* key, const std::wstring& fallback = {}) {
    if (!obj.is_object() || key == nullptr) {
        return fallback;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return fallback;
    }
    return platform::toWide(it->get_ref<const std::string&>());
}

/**
 * @brief Reads a boolean member, or @p fallback when absent/not a boolean.
 */
bool jsonBool(const json& obj, const char* key, bool fallback = false) {
    if (!obj.is_object() || key == nullptr) {
        return fallback;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

/**
 * @brief Converts a JSON value that should be numeric into a double.
 *
 * Accepts integers, unsigned integers, floats and numeric strings.
 * @return true when a number was extracted
 */
bool jsonToDouble(const json& value, double& out) {
    if (value.is_number_float()) {
        out = value.get<double>();
        return std::isfinite(out);
    }
    if (value.is_number_unsigned()) {
        out = static_cast<double>(value.get<uint64_t>());
        return true;
    }
    if (value.is_number_integer()) {
        out = static_cast<double>(value.get<int64_t>());
        return true;
    }
    if (value.is_string()) {
        const auto parsed = platform::parseDouble(platform::toWide(value.get_ref<const std::string&>()));
        if (parsed.has_value() && std::isfinite(*parsed)) {
            out = *parsed;
            return true;
        }
    }
    return false;
}

/**
 * @brief Reads a numeric member as double, or @p fallback.
 */
double jsonDouble(const json& obj, const char* key, double fallback) {
    if (!obj.is_object() || key == nullptr) {
        return fallback;
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    double value = fallback;
    return jsonToDouble(*it, value) ? value : fallback;
}

/**
 * @brief Reads a numeric member as int (range-checked), or @p fallback.
 */
int jsonInt(const json& obj, const char* key, int fallback) {
    if (!obj.is_object() || key == nullptr) {
        return fallback;
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    double value = 0.0;
    if (!jsonToDouble(*it, value)) {
        return fallback;
    }
    // Reject values that would not survive the narrowing conversion.
    constexpr double kMax = static_cast<double>(INT32_MAX);
    constexpr double kMin = static_cast<double>(INT32_MIN);
    if (value > kMax || value < kMin) {
        return fallback;
    }
    return static_cast<int>(value);
}

/**
 * @brief Reads a numeric member as uint64 (negative -> fallback).
 */
uint64_t jsonU64(const json& obj, const char* key, uint64_t fallback) {
    if (!obj.is_object() || key == nullptr) {
        return fallback;
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    // Prefer the exact integer path; fall back to double for odd encodings.
    if (it->is_number_unsigned()) {
        return it->get<uint64_t>();
    }
    if (it->is_number_integer()) {
        const int64_t v = it->get<int64_t>();
        return v < 0 ? fallback : static_cast<uint64_t>(v);
    }
    double value = 0.0;
    if (!jsonToDouble(*it, value) || value < 0.0) {
        return fallback;
    }
    return static_cast<uint64_t>(value);
}

/**
 * @brief Collects an array of strings ("errors" / "warnings") as wide text.
 */
void jsonStringArray(const json& obj, const char* key, std::vector<std::wstring>& out) {
    if (!obj.is_object() || key == nullptr) {
        return;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return;
    }
    for (const auto& item : *it) {
        if (item.is_string()) {
            out.push_back(std::wstring(platform::trim(platform::toWide(item.get_ref<const std::string&>()))));
        }
    }
}

// ---- verification helpers ---------------------------------------------------

/**
 * @brief Tolerance for comparing floating point colour values: relative to the
 *        magnitude with a tiny absolute floor so 0.0001 vs 9.9999997e-05 passes.
 */
double toleranceFor(double expected) {
    return 1e-6 * std::max(1.0, std::fabs(expected)) + 1e-9;
}

/**
 * @brief True when @p actual is within tolerance of @p expected.
 */
bool nearlyEqual(double expected, double actual) {
    return std::fabs(expected - actual) <= toleranceFor(expected);
}

/**
 * @brief Splits "a,b,c" into doubles. Returns false on any non-numeric piece.
 */
bool parseDoubleList(std::wstring_view text, std::vector<double>& out) {
    out.clear();
    const auto pieces = platform::split(text, L',', false);
    for (const auto& piece : pieces) {
        const auto value = platform::parseDouble(platform::trim(piece));
        if (!value.has_value() || !std::isfinite(*value)) {
            return false;
        }
        out.push_back(*value);
    }
    return !out.empty();
}

/**
 * @brief Compares an integer colour field from the preset against the output.
 * @param label       human name for the error message ("colour matrix")
 * @param presetText  the preset's string value (empty = not set, skip)
 * @param actual      value read from the output (-1 = absent)
 */
Result<void> checkIntField(const wchar_t* label, const std::wstring& presetText, int actual) {
    const std::wstring_view trimmed = platform::trim(presetText);
    if (trimmed.empty()) {
        return Result<void>::success();
    }
    const auto expected = platform::parseInt(trimmed);
    if (!expected.has_value()) {
        return Error::text(std::format(L"Preset {} value '{}' is not a number", label, trimmed));
    }
    if (actual < 0) {
        return Error::text(std::format(L"Output is missing the {} (expected {})", label, *expected));
    }
    if (*expected != static_cast<long long>(actual)) {
        return Error::text(std::format(L"{} mismatch: expected {}, output has {}", label, *expected, actual));
    }
    return Result<void>::success();
}

/**
 * @brief Compares a floating point colour field with tolerance.
 */
Result<void> checkDoubleField(const wchar_t* label, const std::wstring& presetText, double actual, bool present) {
    const std::wstring_view trimmed = platform::trim(presetText);
    if (trimmed.empty()) {
        return Result<void>::success();
    }
    const auto expected = platform::parseDouble(trimmed);
    if (!expected.has_value() || !std::isfinite(*expected)) {
        return Error::text(std::format(L"Preset {} value '{}' is not a number", label, trimmed));
    }
    if (!present) {
        return Error::text(std::format(L"Output is missing the {} (expected {})", label, trimmed));
    }
    if (!nearlyEqual(*expected, actual)) {
        return Error::text(std::format(L"{} mismatch: expected {}, output has {}", label, *expected, actual));
    }
    return Result<void>::success();
}

/**
 * @brief Compares a comma-separated coordinate list with tolerance per element.
 */
Result<void> checkCoordinateField(const wchar_t* label, const std::wstring& presetText, const std::wstring& actualText) {
    const std::wstring_view trimmed = platform::trim(presetText);
    if (trimmed.empty()) {
        return Result<void>::success();
    }
    std::vector<double> expected;
    if (!parseDoubleList(trimmed, expected)) {
        return Error::text(std::format(L"Preset {} value '{}' is not a coordinate list", label, trimmed));
    }
    if (platform::trim(actualText).empty()) {
        return Error::text(std::format(L"Output is missing the {} (expected {})", label, trimmed));
    }
    std::vector<double> actual;
    if (!parseDoubleList(actualText, actual)) {
        return Error::text(std::format(L"Output {} '{}' is not a coordinate list", label, actualText));
    }
    if (expected.size() != actual.size()) {
        return Error::text(std::format(L"{} mismatch: expected {} values, output has {}", label, expected.size(), actual.size()));
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!nearlyEqual(expected[i], actual[i])) {
            return Error::text(std::format(L"{} mismatch at index {}: expected {}, output has {}", label, i, expected[i], actual[i]));
        }
    }
    return Result<void>::success();
}

// ---- output line handling ---------------------------------------------------

/**
 * @brief Mutable state shared by the reader loop while a mux runs.
 */
struct RunState {
    std::string pending;              ///< bytes of the current, unterminated line
    float lastProgress = -1.0f;       ///< last value handed to onProgress
    MuxRecord* record = nullptr;      ///< where warnings / lines / errors go
    const std::function<void(float)>* onProgress = nullptr;
};

/**
 * @brief Appends to the tail ring, dropping the oldest line beyond the cap.
 */
void pushLastLine(MuxRecord& record, std::wstring line) {
    record.lastLines.push_back(std::move(line));
    while (record.lastLines.size() > kLastLinesCap) {
        record.lastLines.erase(record.lastLines.begin());
    }
}

/**
 * @brief Case-insensitive ASCII prefix test on raw UTF-8 bytes.
 */
bool startsWithNoCase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(text[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        const unsigned char la = (a >= 'A' && a <= 'Z') ? static_cast<unsigned char>(a + 32u) : a;
        const unsigned char lb = (b >= 'A' && b <= 'Z') ? static_cast<unsigned char>(b + 32u) : b;
        if (la != lb) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Classifies one complete output line and routes it.
 *
 * Progress lines feed the callback (throttled to whole-percent changes),
 * #GUI# warning/error markers land in the record, other #GUI# chatter is
 * dropped and everything else goes to the tail ring for diagnostics.
 */
void handleLine(RunState& state, std::string_view rawLine) {
    if (state.record == nullptr) {
        return;
    }
    const std::string_view line = platform::trim(rawLine);
    if (line.empty()) {
        return;
    }

    // Progress: only forward meaningful changes so the UI is not flooded.
    float progress = 0.0f;
    if (MkvmergeRunner::parseProgressLine(line, progress)) {
        const bool changed = state.lastProgress < 0.0f
                          || std::fabs(progress - state.lastProgress) >= 0.01f
                          || (progress >= 1.0f && state.lastProgress < 1.0f);
        if (changed) {
            state.lastProgress = progress;
            if (state.onProgress != nullptr && *state.onProgress) {
                (*state.onProgress)(progress);
            }
        }
        return;
    }

    // GUI-mode warning / error markers (and the plain-text forms, in case a
    // build ignores --gui-mode for some message class).
    constexpr std::string_view kGuiWarning = "#GUI#warning ";
    constexpr std::string_view kGuiError = "#GUI#error ";
    constexpr std::string_view kPlainWarning = "Warning: ";
    constexpr std::string_view kPlainError = "Error: ";
    if (startsWithNoCase(line, kGuiWarning)) {
        state.record->warnings.push_back(std::wstring(platform::trim(platform::toWide(line.substr(kGuiWarning.size())))));
        return;
    }
    if (startsWithNoCase(line, kGuiError)) {
        const std::wstring text(platform::trim(platform::toWide(line.substr(kGuiError.size()))));
        // Keep the first error as the headline; later ones stay in the tail.
        if (state.record->errorText.empty()) {
            state.record->errorText = text;
        } else {
            pushLastLine(*state.record, text);
        }
        return;
    }
    if (startsWithNoCase(line, "#GUI#")) {
        // Other GUI markers (begin_scanning_playlists, flush, ...) carry nothing we need.
        return;
    }
    if (startsWithNoCase(line, kPlainWarning)) {
        state.record->warnings.push_back(std::wstring(platform::trim(platform::toWide(line.substr(kPlainWarning.size())))));
        return;
    }
    if (startsWithNoCase(line, kPlainError)) {
        const std::wstring text(platform::trim(platform::toWide(line.substr(kPlainError.size()))));
        if (state.record->errorText.empty()) {
            state.record->errorText = text;
        }
        pushLastLine(*state.record, platform::toWide(line));
        return;
    }

    // Anything else is informational; keep the tail for the log pane.
    pushLastLine(*state.record, platform::toWide(line));
}

/**
 * @brief Splits the pending buffer on \n and \r and handles complete lines.
 *
 * mkvmerge separates non-GUI progress with bare \r, so both separators end a
 * line. A trailing fragment stays in the buffer until more bytes arrive
 * (or @p flush is set at the end of the run).
 */
void consumeLines(RunState& state, bool flush) {
    for (;;) {
        const size_t cut = state.pending.find_first_of("\r\n");
        if (cut == std::string::npos) {
            break;
        }
        handleLine(state, std::string_view(state.pending).substr(0, cut));
        state.pending.erase(0, cut + 1);
    }
    if (flush && !state.pending.empty()) {
        handleLine(state, state.pending);
        state.pending.clear();
    }
}

/**
 * @brief True when the manual-reset cancel event is signalled right now.
 */
bool cancelRequested(platform::WaitHandle cancelEvent) {
    if (cancelEvent == nullptr) {
        return false;
    }
    return platform::isSignalled(cancelEvent);
}

/**
 * @brief Removes the partial output unless the plan wants it kept.
 */
void discardPartial(const MuxPlan& plan) {
    if (plan.keepPartialOnFailure || plan.partialPath.empty()) {
        return;
    }
    if (!platform::exists(plan.partialPath)) {
        return;
    }
    auto del = platform::deleteFile(plan.partialPath);
    if (!del) {
        HH_LOG_WARN(kLog, L"could not delete partial '{}': {}", plan.partialPath, del.error().toString());
    } else {
        HH_LOG_INFO(kLog, L"deleted partial '{}'", plan.partialPath);
    }
}

/**
 * @brief Last non-empty tail line, for failure messages without #GUI#error.
 */
std::wstring lastNonEmptyLine(const MuxRecord& record) {
    for (auto it = record.lastLines.rbegin(); it != record.lastLines.rend(); ++it) {
        if (!platform::trim(*it).empty()) {
            return *it;
        }
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// identify / parseIdentification
// ---------------------------------------------------------------------------

Result<Identification> MkvmergeRunner::identify(const std::wstring& mkvmergePath, const std::wstring& file, DWORD timeoutMs) {
    // Validate inputs before spawning anything.
    if (platform::trim(mkvmergePath).empty()) {
        return Error::text(L"mkvmerge path is empty");
    }
    if (platform::trim(file).empty()) {
        return Error::text(L"identify: file path is empty");
    }
    if (!platform::isFile(mkvmergePath)) {
        return Error::text(std::format(L"mkvmerge not found at '{}'", mkvmergePath));
    }
    if (!platform::isFile(file)) {
        return Error::text(std::format(L"File not found: '{}'", file));
    }

    // "-J" prints JSON (UTF-8 regardless of console code page) and exits 2 on
    // an unreadable file while still printing a JSON object with errors[].
    auto capture = platform::runCapture(mkvmergePath, {L"-J", file}, timeoutMs);
    if (!capture) {
        return Error::text(std::format(L"Could not run mkvmerge -J: {}", capture.error().toString()));
    }
    if (capture.value().timedOut) {
        return Error::text(std::format(L"mkvmerge -J timed out after {} ms on '{}'", timeoutMs, file));
    }

    // Parse whatever came back; the JSON carries the error details itself.
    auto parsed = parseIdentification(capture.value().output);
    if (!parsed) {
        return Error::text(std::format(L"mkvmerge -J (exit {}): {}", capture.value().exitCode, parsed.error().message));
    }
    HH_LOG_DEBUG(kLog, L"identified '{}': {} tracks, video id {}, exit {}", file,
                 parsed.value().trackCount, parsed.value().videoTrackId, capture.value().exitCode);
    return parsed;
}

Result<Identification> MkvmergeRunner::parseIdentification(std::string_view text) {
    // Non-throwing parse: a discarded value means the text was not JSON.
    if (text.empty()) {
        return Error::text(L"mkvmerge -J printed nothing");
    }
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return Error::text(L"mkvmerge -J output is not valid JSON");
    }

    Identification ident;

    // container { recognized, supported, type, properties { duration, writing_application } }
    const auto containerIt = root.find("container");
    if (containerIt != root.end() && containerIt->is_object()) {
        const json& container = *containerIt;
        ident.recognized = jsonBool(container, "recognized", false);
        ident.supported = jsonBool(container, "supported", false);
        ident.containerType = jsonString(container, "type");
        const auto propsIt = container.find("properties");
        if (propsIt != container.end() && propsIt->is_object()) {
            ident.writingApplication = jsonString(*propsIt, "writing_application");
            // Matroska duration is in nanoseconds.
            const double durationNs = jsonDouble(*propsIt, "duration", 0.0);
            ident.durationSec = durationNs > 0.0 ? durationNs / 1e9 : 0.0;
        }
    }

    // tracks[]: first video track carries the colour properties we verify.
    const auto tracksIt = root.find("tracks");
    if (tracksIt != root.end() && tracksIt->is_array()) {
        for (const auto& track : *tracksIt) {
            if (!track.is_object()) {
                continue;
            }
            ++ident.trackCount;
            const std::wstring type = jsonString(track, "type");
            if (platform::iequals(type, L"audio")) {
                ++ident.audioTrackCount;
                continue;
            }
            if (!platform::iequals(type, L"video") || ident.videoTrackId >= 0) {
                continue;
            }

            // First video track: id, codec and the property block.
            ident.videoTrackId = jsonInt(track, "id", -1);
            ident.videoCodec = jsonString(track, "codec");
            const auto propsIt = track.find("properties");
            if (propsIt == track.end() || !propsIt->is_object()) {
                continue;
            }
            const json& props = *propsIt;
            ident.pixelDimensions = jsonString(props, "pixel_dimensions");

            // Basic colour description (present on Matroska outputs only).
            ident.matrix = jsonInt(props, "color_matrix_coefficients", -1);
            ident.range = jsonInt(props, "color_range", -1);
            ident.transfer = jsonInt(props, "color_transfer_characteristics", -1);
            ident.primaries = jsonInt(props, "color_primaries", -1);
            ident.hasColour = ident.matrix >= 0 || ident.range >= 0 || ident.transfer >= 0 || ident.primaries >= 0;

            // Mastering display / content light metadata.
            ident.maxCll = jsonInt(props, "max_content_light", -1);
            ident.maxFall = jsonInt(props, "max_frame_light", -1);
            ident.maxLuminance = jsonDouble(props, "max_luminance", -1.0);
            ident.minLuminance = jsonDouble(props, "min_luminance", -1.0);
            ident.chromaticity = jsonString(props, "chromaticity_coordinates");
            ident.whitePoint = jsonString(props, "white_color_coordinates");
            ident.hasMastering = ident.maxCll >= 0 || ident.maxFall >= 0
                              || ident.maxLuminance >= 0.0 || ident.minLuminance >= 0.0
                              || !ident.chromaticity.empty() || !ident.whitePoint.empty();
        }
    }

    // attachments[]: file name, mime type and size.
    const auto attachmentsIt = root.find("attachments");
    if (attachmentsIt != root.end() && attachmentsIt->is_array()) {
        for (const auto& att : *attachmentsIt) {
            if (!att.is_object()) {
                continue;
            }
            Identification::Attachment entry;
            entry.fileName = jsonString(att, "file_name");
            entry.mimeType = jsonString(att, "content_type");
            entry.size = jsonU64(att, "size", 0);
            ident.attachments.push_back(std::move(entry));
        }
    }

    // errors[] / warnings[] are plain string arrays.
    jsonStringArray(root, "errors", ident.errors);
    jsonStringArray(root, "warnings", ident.warnings);

    return ident;
}

// ---------------------------------------------------------------------------
// buildArgs / commandLine
// ---------------------------------------------------------------------------

std::vector<std::wstring> MkvmergeRunner::buildArgs(const MuxPlan& plan) {
    std::vector<std::wstring> args;
    args.reserve(32);

    // Global options first: language pin (when supported), GUI-mode markers,
    // priority and the warning policy.
    if (plan.supportsUiLanguage) {
        args.emplace_back(L"--ui-language");
        args.emplace_back(L"en");
    }
    args.emplace_back(L"--gui-mode");
    if (plan.lowerPriority) {
        args.emplace_back(L"--priority");
        args.emplace_back(L"lower");
    }
    if (plan.failOnWarnings) {
        args.emplace_back(L"--abort-on-warnings");
    }

    // Destination: always the partial name so a crash never leaves a
    // half-written file under the final name.
    args.emplace_back(L"--output");
    args.emplace_back(plan.partialPath);

    // Optional segment title.
    if (plan.setTitle) {
        args.emplace_back(L"--title");
        args.emplace_back(plan.title);
    }

    // LUT attachment: the attachment-* options apply to the NEXT --attach-file.
    if (plan.attachLut && !plan.lutPath.empty()) {
        const std::wstring mime = platform::trim(plan.attachmentMime).empty() ? std::wstring(kDefaultAttachmentMime) : plan.attachmentMime;
        args.emplace_back(L"--attachment-mime-type");
        args.emplace_back(mime);
        args.emplace_back(L"--attachment-name");
        args.emplace_back(path::fileName(plan.lutPath));
        args.emplace_back(L"--attach-file");
        args.emplace_back(plan.lutPath);
    }

    // Track-scoped colour flags must precede the input file they apply to.
    const std::vector<std::wstring> colour = PresetRegistry::colourFlags(plan.preset, plan.trackId);
    args.insert(args.end(), colour.begin(), colour.end());

    // Finally the input file.
    args.emplace_back(plan.inputPath);
    return args;
}

std::wstring MkvmergeRunner::commandLine(const MuxPlan& plan) {
    return platform::buildCommandLine(plan.mkvmergePath, buildArgs(plan));
}

// ---------------------------------------------------------------------------
// parseProgressLine
// ---------------------------------------------------------------------------

bool MkvmergeRunner::parseProgressLine(std::string_view rawLine, float& progress) {
    progress = 0.0f;
    const std::string_view line = platform::trim(rawLine);
    if (line.empty()) {
        return false;
    }

    // Accept both the GUI-mode marker and the classic "Progress:" form.
    constexpr std::string_view kGui = "#GUI#progress";
    constexpr std::string_view kPlain = "Progress:";
    size_t pos = 0;
    if (startsWithNoCase(line, kGui)) {
        pos = kGui.size();
    } else if (startsWithNoCase(line, kPlain)) {
        pos = kPlain.size();
    } else {
        return false;
    }

    // Skip separating whitespace, then scan the percentage digits.
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }
    size_t digits = 0;
    unsigned value = 0;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        if (digits >= kMaxPercentDigits) {
            return false;
        }
        value = value * 10u + static_cast<unsigned>(line[pos] - '0');
        ++pos;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }

    // Optional spaces, then the percent sign is required to avoid matching
    // unrelated "Progress: ..." prose.
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }
    if (pos >= line.size() || line[pos] != '%') {
        return false;
    }

    progress = std::clamp(static_cast<float>(value) / 100.0f, 0.0f, 1.0f);
    return true;
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

MuxRunResult MkvmergeRunner::run(const MuxPlan& plan, platform::WaitHandle cancelEvent,
                                 const std::function<void(float)>& onProgress, MuxRecord& record) {
    MuxRunResult result;
    result.status = MuxRunResult::Status::Failed;

    // The command line is recorded even when the launch fails so the user can
    // copy it from the job card and try by hand.
    record.commandLine = commandLine(plan);
    record.exitCode = 0;
    record.errorText.clear();
    record.warnings.clear();
    record.lastLines.clear();
    record.durationMs = 0;

    // Plan sanity: everything the process spec needs must be present.
    if (platform::trim(plan.mkvmergePath).empty() || !platform::isFile(plan.mkvmergePath)) {
        result.message = L"mkvmerge executable not found";
        HH_LOG_ERROR(kLog, L"{} ('{}')", result.message, plan.mkvmergePath);
        return result;
    }
    if (platform::trim(plan.inputPath).empty()) {
        result.message = L"Input path is empty";
        HH_LOG_ERROR(kLog, L"{}", result.message);
        return result;
    }
    if (platform::trim(plan.partialPath).empty()) {
        result.message = L"Partial output path is empty";
        HH_LOG_ERROR(kLog, L"{}", result.message);
        return result;
    }
    if (cancelRequested(cancelEvent)) {
        result.status = MuxRunResult::Status::Cancelled;
        result.message = L"Cancelled";
        return result;
    }

    // Launch mkvmerge with the output pipe captured and a job object so it
    // dies with us.
    platform::ProcessSpec spec;
    spec.exe = plan.mkvmergePath;
    spec.args = buildArgs(plan);
    spec.workingDirectory = path::parent(plan.inputPath);
    spec.captureOutput = true;
    spec.lowerPriority = plan.lowerPriority;
    spec.killOnJobClose = true;
    spec.hideWindow = true;

    const uint64_t startedMs = platform::nowMonotonicMs();
    platform::ChildProcess child;
    auto started = child.start(spec);
    if (!started) {
        result.message = std::format(L"Could not start mkvmerge: {}", started.error().toString());
        HH_LOG_ERROR(kLog, L"{}", result.message);
        record.durationMs = platform::nowMonotonicMs() - startedMs;
        return result;
    }
    if (!child.commandLine().empty()) {
        record.commandLine = child.commandLine();
    }
    HH_LOG_INFO(kLog, L"started mkvmerge pid {}: {}", child.pid(), record.commandLine);

    // Reader loop with a "no output for timeoutMs" watchdog.
    RunState state;
    state.record = &record;
    state.onProgress = &onProgress;
    uint64_t lastOutputMs = platform::nowMonotonicMs();
    bool cancelled = false;
    bool timedOut = false;
    bool pipeFailed = false;

    for (;;) {
        std::string chunk;
        const auto status = child.readChunk(chunk, kReadWaitMs, cancelEvent);

        // Whatever the status, bytes that arrived are processed first.
        if (!chunk.empty()) {
            state.pending += chunk;
            consumeLines(state, false);
            lastOutputMs = platform::nowMonotonicMs();
        }

        // Cancellation is checked explicitly too, in case the read path only
        // observes the event while it is actually blocked waiting.
        if (status == platform::ChildProcess::ReadStatus::Cancelled || cancelRequested(cancelEvent)) {
            cancelled = true;
            break;
        }

        if (status == platform::ChildProcess::ReadStatus::Data) {
            continue;
        }
        if (status == platform::ChildProcess::ReadStatus::Timeout) {
            // Watchdog: a stalled child (network volume gone) never produces
            // output, so the silence itself is the failure signal.
            const uint64_t silentMs = platform::nowMonotonicMs() - lastOutputMs;
            if (plan.timeoutMs > 0 && silentMs >= static_cast<uint64_t>(plan.timeoutMs)) {
                timedOut = true;
                break;
            }
            if (child.exited()) {
                // Exited without the read path noticing: fall through to drain.
                break;
            }
            continue;
        }
        if (status == platform::ChildProcess::ReadStatus::Exited) {
            break;
        }
        // ReadStatus::Error: the pipe broke. Usually the child is gone; give
        // it a moment to exit before we treat it as a failure.
        pipeFailed = true;
        break;
    }

    // Cancel / watchdog: kill the child and wait briefly for the handle.
    if (cancelled || timedOut) {
        HH_LOG_WARN(kLog, L"{} mkvmerge pid {}", cancelled ? L"cancelling" : L"watchdog timeout, terminating", child.pid());
        child.terminate();
        child.waitForExit(kTerminateWaitMs);
    } else if (pipeFailed) {
        if (!child.waitForExit(kTerminateWaitMs)) {
            HH_LOG_WARN(kLog, L"output pipe failed and mkvmerge pid {} did not exit; terminating", child.pid());
            child.terminate();
            child.waitForExit(kTerminateWaitMs);
        }
    } else if (!child.exited()) {
        // Exited status but the handle is not signalled yet: short wait.
        child.waitForExit(kTerminateWaitMs);
    }

    // Drain whatever is left in the pipe (bounded so a misbehaving read path
    // can never spin forever) and flush the trailing fragment.
    for (int i = 0; i < 4096; ++i) {
        std::string chunk;
        const auto status = child.readChunk(chunk, 0, nullptr);
        if (!chunk.empty()) {
            state.pending += chunk;
            consumeLines(state, false);
        }
        if (status != platform::ChildProcess::ReadStatus::Data) {
            break;
        }
    }
    consumeLines(state, true);

    record.durationMs = platform::nowMonotonicMs() - startedMs;
    record.exitCode = child.exited() ? child.exitCode() : STILL_ACTIVE;

    // Map the outcome.
    if (cancelled) {
        discardPartial(plan);
        result.status = MuxRunResult::Status::Cancelled;
        result.message = L"Cancelled";
        HH_LOG_INFO(kLog, L"mkvmerge cancelled after {} ms", record.durationMs);
        return result;
    }
    if (timedOut) {
        result.status = MuxRunResult::Status::TimedOut;
        result.message = std::format(L"mkvmerge produced no output for {} ms and was terminated", plan.timeoutMs);
        HH_LOG_ERROR(kLog, L"{}", result.message);
        return result;
    }
    if (!child.exited()) {
        result.status = MuxRunResult::Status::Failed;
        result.message = L"mkvmerge output pipe failed and the process did not exit";
        HH_LOG_ERROR(kLog, L"{}", result.message);
        return result;
    }

    // Exit codes: 0 ok, 1 warnings, 2 error, anything else is a crash.
    const DWORD code = record.exitCode;
    switch (code) {
    case 0:
        result.status = MuxRunResult::Status::Done;
        result.message.clear();
        HH_LOG_INFO(kLog, L"mkvmerge finished ok in {} ms", record.durationMs);
        break;
    case 1:
        result.status = MuxRunResult::Status::DoneWithWarnings;
        result.message = std::format(L"Completed with {} mkvmerge warnings", record.warnings.size());
        HH_LOG_WARN(kLog, L"mkvmerge finished with {} warnings in {} ms", record.warnings.size(), record.durationMs);
        break;
    case 2: {
        result.status = MuxRunResult::Status::Failed;
        std::wstring message = record.errorText;
        if (message.empty()) {
            message = lastNonEmptyLine(record);
        }
        if (message.empty()) {
            message = L"mkvmerge failed (exit code 2)";
        }
        result.message = message;
        HH_LOG_ERROR(kLog, L"mkvmerge failed: {}", message);
        break;
    }
    default:
        result.status = MuxRunResult::Status::Crashed;
        result.message = std::format(L"mkvmerge crashed (0x{:08X})", static_cast<unsigned long>(code));
        HH_LOG_ERROR(kLog, L"{}", result.message);
        break;
    }
    return result;
}

// ---------------------------------------------------------------------------
// verify
// ---------------------------------------------------------------------------

Result<void> MkvmergeRunner::verify(const MuxPlan& plan, const Identification& source,
                                    const Mp4Layout* sourceLayout, Identification& outputIdent, MuxRecord& record) {
    // The partial must exist and be non-empty before we spend time on -J.
    if (platform::trim(plan.partialPath).empty()) {
        return Error::text(L"Partial output path is empty");
    }
    auto sizeResult = platform::fileSize(plan.partialPath);
    if (!sizeResult) {
        return Error::text(std::format(L"Output file missing: {}", sizeResult.error().toString()));
    }
    const uint64_t partialSize = sizeResult.value();
    record.outputSize = partialSize;
    if (partialSize == 0) {
        return Error::text(L"Output file is empty");
    }

    // Identify the output.
    auto identResult = identify(plan.mkvmergePath, plan.partialPath);
    if (!identResult) {
        return Error::text(std::format(L"Could not identify output: {}", identResult.error().message));
    }
    outputIdent = identResult.value();

    // Container-level checks.
    if (!platform::icontains(outputIdent.containerType, L"Matroska")) {
        return Error::text(std::format(L"Output is not a Matroska file (type '{}')", outputIdent.containerType));
    }
    if (!outputIdent.errors.empty()) {
        return Error::text(std::format(L"mkvmerge reported an error on the output: {}", outputIdent.errors.front()));
    }
    if (outputIdent.videoTrackId < 0) {
        return Error::text(L"Output has no video track");
    }

    // Colour metadata: every non-empty preset field must round-trip.
    if (!plan.preset.isSentinel()) {
        if (auto r = checkIntField(L"colour matrix", plan.preset.matrix, outputIdent.matrix); !r) { return r; }
        if (auto r = checkIntField(L"colour range", plan.preset.range, outputIdent.range); !r) { return r; }
        if (auto r = checkIntField(L"transfer characteristics", plan.preset.transfer, outputIdent.transfer); !r) { return r; }
        if (auto r = checkIntField(L"colour primaries", plan.preset.primaries, outputIdent.primaries); !r) { return r; }
        if (auto r = checkIntField(L"max content light level", plan.preset.maxCll, outputIdent.maxCll); !r) { return r; }
        if (auto r = checkIntField(L"max frame-average light level", plan.preset.maxFall, outputIdent.maxFall); !r) { return r; }
        if (auto r = checkDoubleField(L"max luminance", plan.preset.maxLuminance, outputIdent.maxLuminance, outputIdent.maxLuminance >= 0.0); !r) { return r; }
        if (auto r = checkDoubleField(L"min luminance", plan.preset.minLuminance, outputIdent.minLuminance, outputIdent.minLuminance >= 0.0); !r) { return r; }
        if (auto r = checkCoordinateField(L"chromaticity coordinates", plan.preset.chromaticity, outputIdent.chromaticity); !r) { return r; }
        if (auto r = checkCoordinateField(L"white point coordinates", plan.preset.whitePoint, outputIdent.whitePoint); !r) { return r; }
    }

    // Track structure must match the source when we know it.
    if (source.recognized) {
        if (outputIdent.audioTrackCount != source.audioTrackCount) {
            return Error::text(std::format(L"Audio track count mismatch: source has {}, output has {}",
                                           source.audioTrackCount, outputIdent.audioTrackCount));
        }
    }
    if (!source.pixelDimensions.empty() && !outputIdent.pixelDimensions.empty()
        && !platform::iequals(source.pixelDimensions, outputIdent.pixelDimensions)) {
        return Error::text(std::format(L"Video dimensions mismatch: source {}, output {}",
                                       source.pixelDimensions, outputIdent.pixelDimensions));
    }

    // LUT attachment: exactly one, with the right mime, name and size.
    if (plan.attachLut && !plan.lutPath.empty()) {
        if (outputIdent.attachments.size() != 1) {
            return Error::text(std::format(L"Expected exactly one attachment, output has {}", outputIdent.attachments.size()));
        }
        const Identification::Attachment& att = outputIdent.attachments.front();
        const std::wstring expectedMime = platform::trim(plan.attachmentMime).empty() ? std::wstring(kDefaultAttachmentMime) : plan.attachmentMime;
        if (!platform::iequals(att.mimeType, expectedMime)) {
            return Error::text(std::format(L"Attachment mime type mismatch: expected '{}', output has '{}'", expectedMime, att.mimeType));
        }
        const std::wstring expectedName = path::fileName(plan.lutPath);
        if (!platform::iequals(att.fileName, expectedName)) {
            return Error::text(std::format(L"Attachment name mismatch: expected '{}', output has '{}'", expectedName, att.fileName));
        }
        auto lutSize = platform::fileSize(plan.lutPath);
        if (!lutSize) {
            return Error::text(std::format(L"LUT could not be read for verification: {}", lutSize.error().toString()));
        }
        if (att.size != lutSize.value()) {
            return Error::text(std::format(L"Attachment size mismatch: LUT is {} bytes, attachment is {} bytes", lutSize.value(), att.size));
        }
    }

    // Soft checks: size window and duration drift only warn, because
    // mkvmerge legitimately drops filler NALUs and rewrites parameter sets.
    if (sourceLayout != nullptr && sourceLayout->mdatPayload > 0) {
        const double lowerBound = 0.97 * static_cast<double>(sourceLayout->mdatPayload);
        if (static_cast<double>(partialSize) < lowerBound) {
            const std::wstring warning = std::format(L"Output ({} bytes) is smaller than 97% of the source media payload ({} bytes)",
                                                     partialSize, sourceLayout->mdatPayload);
            HH_LOG_WARN(kLog, L"{}", warning);
            record.warnings.push_back(warning);
        }
    }
    if (sourceLayout != nullptr && sourceLayout->durationSec > 0.0 && outputIdent.durationSec > 0.0) {
        const double drift = std::fabs(sourceLayout->durationSec - outputIdent.durationSec);
        if (drift > 1.0) {
            const std::wstring warning = std::format(L"Duration differs: source {:.3f} s, output {:.3f} s",
                                                     sourceLayout->durationSec, outputIdent.durationSec);
            HH_LOG_WARN(kLog, L"{}", warning);
            record.warnings.push_back(warning);
        }
    }

    HH_LOG_INFO(kLog, L"verified '{}' ({} bytes, {} tracks, {} attachments)", plan.partialPath,
                partialSize, outputIdent.trackCount, outputIdent.attachments.size());
    return Result<void>::success();
}

// ---------------------------------------------------------------------------
// finalizeOutput
// ---------------------------------------------------------------------------

Result<std::wstring> MkvmergeRunner::finalizeOutput(const MuxPlan& plan) {
    // Both ends of the rename must be known.
    if (platform::trim(plan.partialPath).empty()) {
        return Error::text(L"Partial output path is empty");
    }
    if (platform::trim(plan.hintPath).empty()) {
        return Error::text(L"Hint output path is empty");
    }
    if (!platform::isFile(plan.partialPath)) {
        return Error::text(std::format(L"Partial output missing: '{}'", plan.partialPath));
    }

    // Conflict policy when the final name is already taken.
    if (platform::exists(plan.hintPath)) {
        const std::wstring_view policy = platform::trim(plan.onConflict);
        if (platform::iequals(policy, L"overwrite")) {
            auto moved = platform::moveReplace(plan.partialPath, plan.hintPath);
            if (!moved) {
                return Error::text(std::format(L"Could not replace '{}': {}", plan.hintPath, moved.error().toString()));
            }
            HH_LOG_INFO(kLog, L"replaced existing '{}'", plan.hintPath);
            return plan.hintPath;
        }
        if (platform::iequals(policy, L"skip")) {
            return Error::text(std::format(L"Output exists: '{}'", plan.hintPath));
        }

        // Default "increment": pick the first free "(n)" name.
        const std::wstring target = path::firstFreePath(plan.hintPath);
        if (target.empty()) {
            return Error::text(std::format(L"Could not find a free name next to '{}'", plan.hintPath));
        }
        auto moved = platform::moveNoReplace(plan.partialPath, target);
        if (!moved) {
            return Error::text(std::format(L"Could not rename to '{}': {}", target, moved.error().toString()));
        }
        HH_LOG_INFO(kLog, L"'{}' existed; wrote '{}'", plan.hintPath, target);
        return target;
    }

    // Plain rename to the final name.
    auto moved = platform::moveNoReplace(plan.partialPath, plan.hintPath);
    if (!moved) {
        return Error::text(std::format(L"Could not rename to '{}': {}", plan.hintPath, moved.error().toString()));
    }
    HH_LOG_INFO(kLog, L"finalized '{}'", plan.hintPath);
    return plan.hintPath;
}

} // namespace hh
