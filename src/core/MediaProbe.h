// ---------------------------------------------------------------------------
// MediaProbe.h - optional ffprobe-based inspection of the export.
//
// ffprobe is not required; when absent probeMedia() returns std::nullopt and
// the engine relies on the AME log / CEP for the colour space.
// ---------------------------------------------------------------------------
#pragma once

#include "core/JobModel.h"
#include "platform/Win.h"

#include <optional>
#include <string>

namespace hh {

struct MediaInfo {
    std::wstring codec;              ///< "hevc"
    std::wstring profile;            ///< "Main 10"
    int width = 0;
    int height = 0;
    double fps = 0.0;
    std::wstring colorTransfer;      ///< "smpte2084", "arib-std-b67", "bt709"
    std::wstring colorPrimaries;     ///< "bt2020"
    std::wstring colorSpace;         ///< "bt2020nc"
    std::wstring colorRange;         ///< "tv"
    TransferKind transfer = TransferKind::Unknown;
    bool hasMasteringDisplay = false;///< first-frame side data "Mastering display metadata"
    bool hasContentLightLevel = false;
    double durationSec = 0.0;
};

/// Finds ffprobe: configured path, PATH, C:\ffmpeg\bin, next to the exe. Empty when absent.
std::wstring locateFfprobe(const std::wstring& configuredPath = L"");

/// Runs ffprobe (JSON) on the file. std::nullopt when ffprobe is missing or fails.
std::optional<MediaInfo> probeMedia(const std::wstring& ffprobePath, const std::wstring& mediaPath, DWORD timeoutMs = 30000);

/// Maps ffprobe transfer names to TransferKind.
TransferKind transferFromFfprobeName(std::wstring_view name);

} // namespace hh
