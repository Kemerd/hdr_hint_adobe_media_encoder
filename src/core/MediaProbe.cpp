// ---------------------------------------------------------------------------
// MediaProbe.cpp - optional ffprobe-based inspection of the export.
//
// ffprobe is a nice-to-have: it tells us the transfer characteristic the
// encoder wrote and whether the first frame already carries mastering
// display / content light level SEI (in-band HDR10). When it is absent or
// fails, probeMedia() returns std::nullopt and the caller carries on.
// ---------------------------------------------------------------------------
#include "core/MediaProbe.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/Process.h"
#include "platform/Utf.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hh {

namespace {

using json = nlohmann::json;

/// Component tag used for every log line in this file.
constexpr const wchar_t* kLog = L"MediaProbe";

/// The conventional static-build install location on Windows.
constexpr const wchar_t* kDefaultFfmpegBin = L"C:\\ffmpeg\\bin\\ffprobe.exe";

/**
 * @brief Resolves a bare executable name through the Win32 search path.
 *
 * Mirrors the mkvmerge locator: application directory, current directory,
 * system directories, then every PATH entry. Grows the buffer once.
 */
std::wstring searchPathFor(const wchar_t* name, const wchar_t* ext) {
    if (name == nullptr || *name == L'\0') {
        return {};
    }
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD needed = ::SearchPathW(nullptr, name, ext, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (needed == 0) {
            return {};
        }
        if (needed < buffer.size()) {
            return std::wstring(buffer.data(), needed);
        }
        buffer.assign(static_cast<size_t>(needed) + 1u, L'\0');
    }
    return {};
}

// ---- JSON access helpers ----------------------------------------------------
// ffprobe prints numbers for width/height but strings for durations and
// frame rates, so each accessor tolerates both encodings.

/**
 * @brief Reads a string member as UTF-16 (numbers are stringified).
 */
std::wstring jsonString(const json& obj, const char* key) {
    if (!obj.is_object() || key == nullptr) {
        return {};
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return {};
    }
    if (it->is_string()) {
        return platform::toWide(it->get_ref<const std::string&>());
    }
    if (it->is_number_integer()) {
        return std::to_wstring(it->get<int64_t>());
    }
    if (it->is_number_unsigned()) {
        return std::to_wstring(it->get<uint64_t>());
    }
    if (it->is_number_float()) {
        return std::to_wstring(it->get<double>());
    }
    return {};
}

/**
 * @brief Reads an integer member (number or numeric string), else @p fallback.
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
    if (it->is_number_float()) {
        value = it->get<double>();
    } else if (it->is_number_unsigned()) {
        value = static_cast<double>(it->get<uint64_t>());
    } else if (it->is_number_integer()) {
        value = static_cast<double>(it->get<int64_t>());
    } else if (it->is_string()) {
        const auto parsed = platform::parseInt(platform::trim(platform::toWide(it->get_ref<const std::string&>())));
        if (!parsed.has_value()) {
            return fallback;
        }
        value = static_cast<double>(*parsed);
    } else {
        return fallback;
    }
    if (!std::isfinite(value) || value > static_cast<double>(INT32_MAX) || value < static_cast<double>(INT32_MIN)) {
        return fallback;
    }
    return static_cast<int>(value);
}

/**
 * @brief Reads a floating point member (number or numeric string), else @p fallback.
 */
double jsonDouble(const json& obj, const char* key, double fallback) {
    if (!obj.is_object() || key == nullptr) {
        return fallback;
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    if (it->is_number_float()) {
        const double v = it->get<double>();
        return std::isfinite(v) ? v : fallback;
    }
    if (it->is_number_unsigned()) {
        return static_cast<double>(it->get<uint64_t>());
    }
    if (it->is_number_integer()) {
        return static_cast<double>(it->get<int64_t>());
    }
    if (it->is_string()) {
        const auto parsed = platform::parseDouble(platform::trim(platform::toWide(it->get_ref<const std::string&>())));
        if (parsed.has_value() && std::isfinite(*parsed)) {
            return *parsed;
        }
    }
    return fallback;
}

/**
 * @brief Parses ffprobe's "num/den" rational ("60000/1001") into a double.
 *
 * A plain number without a slash is accepted too. Zero or negative
 * denominators yield 0.
 */
double parseRational(std::wstring_view text) {
    const std::wstring_view trimmed = platform::trim(text);
    if (trimmed.empty()) {
        return 0.0;
    }
    const size_t slash = trimmed.find(L'/');
    if (slash == std::wstring_view::npos) {
        const auto value = platform::parseDouble(trimmed);
        return (value.has_value() && std::isfinite(*value) && *value > 0.0) ? *value : 0.0;
    }
    const auto num = platform::parseDouble(platform::trim(trimmed.substr(0, slash)));
    const auto den = platform::parseDouble(platform::trim(trimmed.substr(slash + 1)));
    if (!num.has_value() || !den.has_value() || !std::isfinite(*num) || !std::isfinite(*den) || *den <= 0.0 || *num <= 0.0) {
        return 0.0;
    }
    return *num / *den;
}

/**
 * @brief Parses ffprobe's JSON, tolerating stray text around the object.
 *
 * With "-v error" ffprobe prints nothing but JSON on success, yet stderr is
 * merged into our capture so a non-fatal message could precede or follow the
 * object. A second attempt slices from the first '{' to the last '}'.
 */
json parseLenient(const std::string& text) {
    json root = json::parse(text, nullptr, false);
    if (!root.is_discarded() && root.is_object()) {
        return root;
    }
    const size_t open = text.find('{');
    const size_t close = text.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return json(json::value_t::discarded);
    }
    root = json::parse(std::string_view(text).substr(open, close - open + 1), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return json(json::value_t::discarded);
    }
    return root;
}

} // namespace

// ---------------------------------------------------------------------------
// locateFfprobe
// ---------------------------------------------------------------------------

std::wstring locateFfprobe(const std::wstring& configuredPath) {
    // 1. The configured path (a folder is accepted and completed).
    const std::wstring_view configured = platform::trim(configuredPath);
    if (!configured.empty()) {
        std::wstring candidate(configured);
        if (platform::isDirectory(candidate)) {
            candidate = path::join(candidate, L"ffprobe.exe");
        }
        if (platform::isFile(candidate)) {
            HH_LOG_DEBUG(kLog, L"using configured ffprobe '{}'", candidate);
            return candidate;
        }
        HH_LOG_WARN(kLog, L"configured ffprobe '{}' not found; searching", candidate);
    }

    // 2. PATH.
    const std::wstring onPath = searchPathFor(L"ffprobe", L".exe");
    if (!onPath.empty() && platform::isFile(onPath)) {
        HH_LOG_DEBUG(kLog, L"found ffprobe on PATH: '{}'", onPath);
        return onPath;
    }

    // 3. The conventional static-build location.
    if (platform::isFile(kDefaultFfmpegBin)) {
        HH_LOG_DEBUG(kLog, L"found ffprobe at '{}'", kDefaultFfmpegBin);
        return kDefaultFfmpegBin;
    }

    // 4. Next to our own executable.
    const std::wstring exeDir = platform::exeDirectory();
    if (!exeDir.empty()) {
        const std::wstring beside = path::join(exeDir, L"ffprobe.exe");
        if (platform::isFile(beside)) {
            HH_LOG_DEBUG(kLog, L"found ffprobe beside the executable: '{}'", beside);
            return beside;
        }
    }

    HH_LOG_DEBUG(kLog, L"ffprobe not found (optional)");
    return {};
}

// ---------------------------------------------------------------------------
// probeMedia
// ---------------------------------------------------------------------------

std::optional<MediaInfo> probeMedia(const std::wstring& ffprobePath, const std::wstring& mediaPath, DWORD timeoutMs) {
    // Inputs must point at real files; ffprobe itself is optional so a
    // missing binary is a quiet nullopt rather than an error.
    if (platform::trim(ffprobePath).empty() || !platform::isFile(ffprobePath)) {
        HH_LOG_DEBUG(kLog, L"probeMedia: ffprobe not available ('{}')", ffprobePath);
        return std::nullopt;
    }
    if (platform::trim(mediaPath).empty() || !platform::isFile(mediaPath)) {
        HH_LOG_WARN(kLog, L"probeMedia: media file missing ('{}')", mediaPath);
        return std::nullopt;
    }

    // One call gives us the stream description and the first frame's side
    // data (mastering display / content light level SEI).
    const std::vector<std::wstring> args = {
        L"-v", L"error",
        L"-select_streams", L"v:0",
        L"-show_entries", L"stream=codec_name,profile,width,height,r_frame_rate,color_transfer,color_primaries,color_space,color_range,duration",
        L"-show_entries", L"frame=side_data_list",
        L"-read_intervals", L"%+#1",
        L"-show_frames",
        L"-of", L"json",
        mediaPath,
    };
    auto capture = platform::runCapture(ffprobePath, args, timeoutMs);
    if (!capture) {
        HH_LOG_WARN(kLog, L"probeMedia: could not run ffprobe: {}", capture.error().toString());
        return std::nullopt;
    }
    if (capture.value().timedOut) {
        HH_LOG_WARN(kLog, L"probeMedia: ffprobe timed out after {} ms on '{}'", timeoutMs, mediaPath);
        return std::nullopt;
    }
    if (capture.value().exitCode != 0) {
        HH_LOG_WARN(kLog, L"probeMedia: ffprobe exit {} on '{}': {}", capture.value().exitCode, mediaPath,
                    platform::trim(platform::toWide(capture.value().output)));
        return std::nullopt;
    }

    // Parse the JSON document.
    const json root = parseLenient(capture.value().output);
    if (root.is_discarded()) {
        HH_LOG_WARN(kLog, L"probeMedia: ffprobe output is not JSON for '{}'", mediaPath);
        return std::nullopt;
    }

    MediaInfo info;
    bool haveStream = false;

    // streams[0]: codec, geometry, frame rate, colour description, duration.
    const auto streamsIt = root.find("streams");
    if (streamsIt != root.end() && streamsIt->is_array() && !streamsIt->empty()) {
        const json& stream = streamsIt->front();
        if (stream.is_object()) {
            haveStream = true;
            info.codec = jsonString(stream, "codec_name");
            info.profile = jsonString(stream, "profile");
            info.width = jsonInt(stream, "width", 0);
            info.height = jsonInt(stream, "height", 0);
            info.fps = parseRational(jsonString(stream, "r_frame_rate"));
            info.colorTransfer = jsonString(stream, "color_transfer");
            info.colorPrimaries = jsonString(stream, "color_primaries");
            info.colorSpace = jsonString(stream, "color_space");
            info.colorRange = jsonString(stream, "color_range");
            info.durationSec = jsonDouble(stream, "duration", 0.0);
        }
    }

    // frames[0]: side data types, plus colour fields as a fallback when the
    // stream header did not carry them.
    const auto framesIt = root.find("frames");
    if (framesIt != root.end() && framesIt->is_array() && !framesIt->empty()) {
        const json& frame = framesIt->front();
        if (frame.is_object()) {
            if (info.colorTransfer.empty()) {
                info.colorTransfer = jsonString(frame, "color_transfer");
            }
            if (info.colorPrimaries.empty()) {
                info.colorPrimaries = jsonString(frame, "color_primaries");
            }
            if (info.colorSpace.empty()) {
                info.colorSpace = jsonString(frame, "color_space");
            }
            if (info.colorRange.empty()) {
                info.colorRange = jsonString(frame, "color_range");
            }
            if (info.width <= 0) {
                info.width = jsonInt(frame, "width", 0);
            }
            if (info.height <= 0) {
                info.height = jsonInt(frame, "height", 0);
            }

            // side_data_list[].side_data_type
            const auto sideIt = frame.find("side_data_list");
            if (sideIt != frame.end() && sideIt->is_array()) {
                for (const auto& side : *sideIt) {
                    if (!side.is_object()) {
                        continue;
                    }
                    const std::wstring type = jsonString(side, "side_data_type");
                    if (platform::icontains(type, L"Mastering display")) {
                        info.hasMasteringDisplay = true;
                    } else if (platform::icontains(type, L"Content light level")) {
                        info.hasContentLightLevel = true;
                    }
                }
            }
        }
    }

    // Without any stream description the probe told us nothing useful.
    if (!haveStream && info.colorTransfer.empty() && info.width <= 0) {
        HH_LOG_WARN(kLog, L"probeMedia: no video stream reported for '{}'", mediaPath);
        return std::nullopt;
    }

    info.transfer = transferFromFfprobeName(info.colorTransfer);
    HH_LOG_DEBUG(kLog, L"probeMedia '{}': {} {} {}x{} {:.3f} fps transfer '{}' ({}) mastering={} cll={} duration={:.3f}s",
                 mediaPath, info.codec, info.profile, info.width, info.height, info.fps, info.colorTransfer,
                 toString(info.transfer), info.hasMasteringDisplay, info.hasContentLightLevel, info.durationSec);
    return info;
}

// ---------------------------------------------------------------------------
// transferFromFfprobeName
// ---------------------------------------------------------------------------

TransferKind transferFromFfprobeName(std::wstring_view name) {
    const std::wstring lowered = platform::toLowerInvariant(platform::trim(name));
    if (lowered.empty()) {
        return TransferKind::Unknown;
    }

    // PQ (SMPTE ST 2084).
    if (lowered == L"smpte2084" || lowered == L"pq" || lowered == L"smpte st 2084" || lowered == L"smpte-st-2084") {
        return TransferKind::PQ;
    }

    // HLG (ARIB STD-B67).
    if (lowered == L"arib-std-b67" || lowered == L"arib_std_b67" || lowered == L"hlg") {
        return TransferKind::HLG;
    }

    // The common SDR gamma curves.
    if (lowered == L"bt709" || lowered == L"smpte170m" || lowered == L"bt470bg"
        || lowered == L"iec61966-2-1" || lowered == L"iec61966_2_1"
        || lowered == L"bt470m" || lowered == L"smpte240m"
        || lowered == L"bt2020-10" || lowered == L"bt2020-12") {
        return TransferKind::SDR;
    }

    return TransferKind::Unknown;
}

} // namespace hh
