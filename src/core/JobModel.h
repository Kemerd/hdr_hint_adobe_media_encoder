// ---------------------------------------------------------------------------
// JobModel.h - the data model for one AME export being post-processed.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hh {

using JobId = uint64_t;

/// Lifecycle of a job. Terminal states: Done, Failed, SkippedSdr, Cancelled.
enum class JobState {
    Discovered,   ///< a candidate file appeared; readiness not yet known
    Encoding,     ///< AME is still writing (sidecars / lock / CEP started)
    Ready,        ///< file is complete and unlocked; waiting for dispatch
    Held,         ///< waiting for the user (hold, unknown transfer, plan error)
    Muxing,       ///< mkvmerge running
    Verifying,    ///< mkvmerge finished; checking the output
    Done,         ///< hint file produced and verified
    Failed,       ///< something went wrong (see stateReason)
    SkippedSdr,   ///< export was SDR; nothing to do by policy
    Cancelled,    ///< removed / cancelled by the user
};

/// Colour transfer of the export as far as we know.
enum class TransferKind { Unknown, PQ, HLG, SDR };

/// Which signal first created the job.
enum class JobSource { Log, Folder, Cep, Manual, CatchUp };

/// Outcome of the "move original to Recycle Bin" step.
enum class RecycleStatus {
    NotRequested, Pending, Recycled, KeptNoBin, KeptSourceChanged, KeptDisabled, Failed
};

/**
 * @brief What the AME log's "Video:" line (or a probe) told us about the export.
 */
struct VideoSummary {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    TransferKind transfer = TransferKind::Unknown;
    bool hardwareEncoding = false;
    std::wstring vendor;         ///< "Nvidia", "Intel", "AMD", ...
    std::wstring codecHint;      ///< e.g. "Apple ProRes 422 HQ" when present
    std::wstring raw;            ///< the original line for display

    /// "3840x2160 · 59.94 fps · Rec.2100 PQ · Nvidia" style summary.
    [[nodiscard]] std::wstring describe() const;
};

/**
 * @brief Per-job user choices that override the global defaults.
 */
struct JobOverrides {
    std::optional<std::wstring> lutPath;      ///< empty string = no LUT
    std::optional<std::wstring> presetId;
    std::optional<bool> attachLut;
    std::optional<std::wstring> suffix;
    bool hold = false;
};

/**
 * @brief Identity of the AME output at the moment it became Ready.
 */
struct SourceStamp {
    uint64_t size = 0;
    uint64_t lastWriteUtc = 0;
    uint64_t creationUtc = 0;
    uint64_t fileIdLow = 0;
    uint64_t fileIdHigh = 0;
    uint32_t volumeSerial = 0;
    bool hasFileId = false;
    bool valid = false;

    [[nodiscard]] bool sameFileAs(const SourceStamp& o) const noexcept;
};

/**
 * @brief Everything recorded about the mkvmerge run.
 */
struct MuxRecord {
    std::wstring commandLine;
    DWORD exitCode = 0;
    std::vector<std::wstring> warnings;
    std::vector<std::wstring> lastLines;   ///< tail of mkvmerge's output (cap 200)
    std::wstring errorText;
    uint64_t outputSize = 0;
    uint64_t durationMs = 0;
};

/**
 * @brief The resolved plan (defaults + overrides) captured at dispatch time.
 */
struct EffectivePlan {
    std::wstring presetId;
    std::wstring lutPath;
    bool attachLut = false;
    std::wstring suffix;
    std::wstring hintPath;
    std::wstring error;          ///< non-empty = plan invalid (job goes Held)

    [[nodiscard]] bool valid() const noexcept { return error.empty(); }
};

/**
 * @brief One AME export tracked by HdrHint.
 */
struct Job {
    JobId id = 0;
    std::wstring key;                 ///< normalised output path (upper-cased, absolute)
    uint32_t generation = 1;          ///< increments when the same path is exported again

    JobState state = JobState::Discovered;
    std::wstring stateReason;         ///< human text for Held/Failed/Skipped
    std::wstring phase;               ///< "Finalizing", "Waiting for AME", ...
    float progress = 0.0f;            ///< 0..1 for Encoding(Finalizing)/Muxing

    std::wstring outputPath;          ///< AME's output = our mux input
    std::wstring sourcePath;          ///< AME's source (.prproj or media), display only
    std::wstring hintPath;            ///< our result once known
    std::wstring presetName;          ///< AME preset name from the log

    TransferKind transfer = TransferKind::Unknown;
    std::wstring transferSource;      ///< "log" | "cep" | "probe" | "identify" | "user"
    VideoSummary video;
    bool inbandHdr10 = false;         ///< source already carries mastering/CLL SEI

    JobSource source = JobSource::Folder;
    JobOverrides overrides;
    EffectivePlan plan;

    uint64_t createdUtc = 0;
    uint64_t updatedUtc = 0;
    uint64_t encodeStartUtc = 0;
    uint64_t readyUtc = 0;
    uint64_t muxStartUtc = 0;
    uint64_t doneUtc = 0;

    SourceStamp sourceStamp;
    MuxRecord mux;
    RecycleStatus recycle = RecycleStatus::NotRequested;
    std::wstring recycleMessage;
    std::vector<std::wstring> notes;

    bool logConfirmed = false;        ///< AME log reported success for this generation
    bool cepConfirmed = false;        ///< CEP bridge reported completion
    uint64_t sidecarBytes = 0;        ///< sum of sidecar sizes seen (progress hint)

    [[nodiscard]] bool isTerminal() const noexcept;
    [[nodiscard]] bool isActive() const noexcept { return !isTerminal(); }
    /// File name of outputPath for display.
    [[nodiscard]] std::wstring displayName() const;
};

// ---- enum <-> text ----------------------------------------------------------
const wchar_t* toString(JobState s) noexcept;
const wchar_t* toString(TransferKind t) noexcept;
const wchar_t* toString(JobSource s) noexcept;
const wchar_t* toString(RecycleStatus s) noexcept;
std::optional<JobState> jobStateFromString(std::wstring_view s) noexcept;
std::optional<TransferKind> transferKindFromString(std::wstring_view s) noexcept;
std::optional<JobSource> jobSourceFromString(std::wstring_view s) noexcept;
std::optional<RecycleStatus> recycleStatusFromString(std::wstring_view s) noexcept;

} // namespace hh
