// ---------------------------------------------------------------------------
// MkvmergeRunner.h - identification (-J), argv building, muxing, verification.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "core/HdrPresets.h"
#include "core/JobModel.h"
#include "core/Mp4Boxes.h"
#include "platform/Win.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hh {

/**
 * @brief Subset of `mkvmerge -J` output we care about.
 */
struct Identification {
    bool recognized = false;
    bool supported = false;
    std::wstring containerType;          ///< "QuickTime/MP4", "Matroska"
    std::wstring writingApplication;     ///< Matroska only
    double durationSec = 0.0;            ///< Matroska only (container.properties.duration ns)
    int videoTrackId = -1;               ///< id of the first video track (-1 = none)
    std::wstring videoCodec;
    std::wstring pixelDimensions;        ///< "3840x2160"
    int audioTrackCount = 0;
    int trackCount = 0;

    // Colour properties of the first video track (Matroska outputs only).
    bool hasColour = false;
    int matrix = -1, range = -1, transfer = -1, primaries = -1;
    bool hasMastering = false;
    int maxCll = -1, maxFall = -1;
    double maxLuminance = -1.0, minLuminance = -1.0;
    std::wstring chromaticity;           ///< "rx,ry,gx,gy,bx,by" (as printed)
    std::wstring whitePoint;             ///< "x,y"

    struct Attachment {
        std::wstring fileName;
        std::wstring mimeType;
        uint64_t size = 0;
    };
    std::vector<Attachment> attachments;
    std::vector<std::wstring> errors;
    std::vector<std::wstring> warnings;
};

/**
 * @brief Everything the mux needs, resolved by the engine before dispatch.
 */
struct MuxPlan {
    std::wstring mkvmergePath;
    bool supportsUiLanguage = false;
    std::wstring inputPath;       ///< AME output
    std::wstring hintPath;        ///< final name
    std::wstring partialPath;     ///< "<hint>.hdrhint-partial.mkv"
    HdrPreset preset;             ///< colour values (may be sentinel-free custom values)
    std::wstring lutPath;         ///< empty = no attachment
    std::wstring attachmentMime = L"application/x-cube";
    bool attachLut = false;
    int trackId = 0;              ///< video track id from identification
    bool lowerPriority = true;
    bool failOnWarnings = false;
    bool setTitle = false;
    std::wstring title;
    std::wstring onConflict = L"increment";   ///< increment | overwrite | skip
    bool keepPartialOnFailure = false;
    DWORD timeoutMs = 6u * 3600u * 1000u;
};

/// Outcome of a mux run (before verification).
struct MuxRunResult {
    enum class Status { Done, DoneWithWarnings, Failed, Cancelled, Crashed, TimedOut };
    Status status = Status::Failed;
    std::wstring message;
};

class MkvmergeRunner {
public:
    /// Runs `mkvmerge -J <file>` and parses the JSON.
    static Result<Identification> identify(const std::wstring& mkvmergePath, const std::wstring& file, DWORD timeoutMs = 60000);
    /// Parses -J JSON text (for tests).
    static Result<Identification> parseIdentification(std::string_view json);

    /// Builds the argv (without the exe) in the documented order.
    static std::vector<std::wstring> buildArgs(const MuxPlan& plan);
    /// The full command line as it will be launched (for logs / "Copy command").
    static std::wstring commandLine(const MuxPlan& plan);

    /**
     * @brief Runs the mux, streaming progress.
     * @param cancelEvent  manual-reset event that aborts the run
     * @param onProgress   0..1, called from the calling thread
     * @param record       filled with command line, exit code, warnings, tail lines
     */
    static MuxRunResult run(const MuxPlan& plan, HANDLE cancelEvent,
                            const std::function<void(float)>& onProgress, MuxRecord& record);

    /**
     * @brief Verifies the partial output against the plan and the source.
     * @param source        identification of the input (track counts / dims)
     * @param sourceLayout  optional MP4 layout of the input (duration / mdat)
     */
    static Result<void> verify(const MuxPlan& plan, const Identification& source,
                               const Mp4Layout* sourceLayout, Identification& outputIdent, MuxRecord& record);

    /// Renames partial -> hint honouring onConflict. Returns the final path.
    static Result<std::wstring> finalizeOutput(const MuxPlan& plan);

    /// Parses one mkvmerge output line: "#GUI#progress 42%" / "Progress: 42%" -> 0.42.
    static bool parseProgressLine(std::string_view line, float& progress);
};

} // namespace hh
