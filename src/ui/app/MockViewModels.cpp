// ---------------------------------------------------------------------------
// MockViewModels.cpp - canned data for screenshots and UI tests.
// ---------------------------------------------------------------------------
#include "ui/app/MockViewModels.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/Terms.h"
#include "platform/Utf.h"
#include "ui/app/AppViewModels.h"
#include "ui/app/GuideResource.h"
#include "ui/screens/GuideContent.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"MockVM";

/// Separator used between subtitle / summary parts everywhere in the UI.
constexpr const wchar_t* kDot = L" · ";

/// Where the sample exports live and where mkvmerge is "installed" - spelled
/// the way each platform's screenshots should show them.
#if defined(_WIN32)
constexpr const wchar_t* kExportFolder = L"D:\\Exports\\YouTube";
constexpr const wchar_t* kMkvmergePath = L"C:\\Program Files\\MKVToolNix\\mkvmerge.exe";
constexpr const wchar_t* kMkvmergeFolder = L"C:\\Program Files\\MKVToolNix";
constexpr const wchar_t* kLutFolder = L"C:\\Program Files\\HDR Hint\\luts";
constexpr const wchar_t* kUserLutFolder = L"D:\\LUTs";
constexpr const wchar_t* kClientFolder = L"D:\\Exports\\Client Deliveries";
constexpr const wchar_t* kShortsFolder = L"D:\\Exports\\Shorts";
constexpr const wchar_t* kAmeLogPath = L"C:\\Users\\Editor\\Documents\\Adobe\\Adobe Media Encoder\\26.0\\AMEEncodingLog.txt";
#else
constexpr const wchar_t* kExportFolder = L"/Volumes/Media/Exports/YouTube";
constexpr const wchar_t* kMkvmergePath = L"/Applications/MKVToolNix-90.0.app/Contents/MacOS/mkvmerge";
constexpr const wchar_t* kMkvmergeFolder = L"/Applications/MKVToolNix-90.0.app/Contents/MacOS";
constexpr const wchar_t* kLutFolder = L"/Applications/HdrHint.app/Contents/Resources/luts";
constexpr const wchar_t* kUserLutFolder = L"/Volumes/Media/LUTs";
constexpr const wchar_t* kClientFolder = L"/Volumes/Media/Exports/Client Deliveries";
constexpr const wchar_t* kShortsFolder = L"/Volumes/Media/Exports/Shorts";
constexpr const wchar_t* kAmeLogPath = L"/Users/editor/Documents/Adobe/Adobe Media Encoder/26.0/AMEEncodingLog.txt";
#endif
constexpr const wchar_t* kPqLutName = L"PQ1000_to_Rec709_SDR_g24_YouTubeHint.cube";
constexpr const wchar_t* kHlgLutName = L"HLG_to_Rec709_SDR_YouTubeHint.cube";
constexpr const wchar_t* kSuffix = L"_REC709_HINT";
constexpr const wchar_t* kAboutLine = L"HDR Hint 1.0.0 · mkvmerge v82.0";

/**
 * @brief The built-in presets as the real registry lists them (id, label, family).
 */
struct MockPreset {
    const wchar_t* id;
    const wchar_t* label;
    TransferKind kind;
};

constexpr MockPreset kPresets[] = {
    {L"dji_pocket3_hlg", L"DJI Osmo Pocket 3 (HLG)",                    TransferKind::HLG},
    {L"dji_mavic_hlg",   L"DJI Mavic / Air / Mini (HLG)",               TransferKind::HLG},
    {L"sony_hlg",        L"Sony HLG (a7S III / FX3 / FX6)",             TransferKind::HLG},
    {L"panasonic_hlg",   L"Panasonic HLG (GH5/GH6/S5)",                 TransferKind::HLG},
    {L"generic_hlg",     L"Generic Rec.2100 HLG",                       TransferKind::HLG},
    {L"iphone_pq",       L"iPhone Dolby Vision / HDR10 (PQ)",           TransferKind::PQ},
    {L"sony_hdr10_pq",   L"Sony HDR10 (PQ)",                            TransferKind::PQ},
    {L"generic_pq_1000", L"Generic Rec.2100 PQ / HDR10 (1000 nits)",    TransferKind::PQ},
    {L"generic_pq_4000", L"Generic Rec.2100 PQ / HDR10 (4000 nits)",    TransferKind::PQ},
    {L"sdr_rec709",      L"SDR Rec.709 (no HDR)",                       TransferKind::SDR},
};

/**
 * @brief The LUT files the mock "found" (folder listing + one recent).
 */
std::vector<std::wstring> mockLutPaths() {
    return {
        path::join(kLutFolder, kPqLutName),
        path::join(kLutFolder, kHlgLutName),
        path::join(kLutFolder, L"pocket3_dlogm_to_rec709.cube"),
        path::join(kUserLutFolder, L"Client_Grade_v3.cube"),
    };
}

/**
 * @brief Preset choices for a transfer, straight from the table (Unknown = all).
 */
std::vector<Choice> mockPresetChoices(TransferKind t) {
    std::vector<Choice> out;
    for (const MockPreset& p : kPresets) {
        if (t != TransferKind::Unknown && p.kind != t) {
            continue;
        }
        out.push_back(Choice{p.label, p.id, std::wstring()});
    }
    return out;
}

/**
 * @brief LUT choices: "None" first, then every file with its folder as subtitle.
 */
std::vector<Choice> mockLutChoices() {
    std::vector<Choice> out;
    out.push_back(Choice{L"None", std::wstring(), std::wstring()});
    for (const std::wstring& lut : mockLutPaths()) {
        out.push_back(Choice{path::fileName(lut), lut, path::parent(lut)});
    }
    // Last entry: reach any .cube on disk, not just the folder and recents.
    out.push_back(Choice{L"Choose a .cube file...", kBrowseLutValue, std::wstring()});
    return out;
}

/**
 * @brief Family of a preset id (falls back to PQ for unknown ids).
 */
TransferKind presetKindFor(std::wstring_view presetId) noexcept {
    for (const MockPreset& p : kPresets) {
        if (platform::iequals(p.id, presetId)) {
            return p.kind;
        }
    }
    return TransferKind::PQ;
}

/**
 * @brief Default preset id for a transfer (empty for Unknown, like the engine).
 */
std::wstring defaultPresetFor(TransferKind t) {
    switch (t) {
    case TransferKind::PQ:  return L"generic_pq_1000";
    case TransferKind::HLG: return L"generic_hlg";
    case TransferKind::SDR: return L"sdr_rec709";
    default:                return {};
    }
}

/**
 * @brief Default LUT for a transfer (PQ only, like the shipped settings).
 */
std::wstring defaultLutFor(TransferKind t) {
    return t == TransferKind::PQ ? path::join(kLutFolder, kPqLutName) : std::wstring();
}

/**
 * @brief The "Video:" summary line the AME log would have produced.
 */
std::wstring videoSummaryFor(TransferKind t) {
    switch (t) {
    case TransferKind::PQ:  return L"3840x2160 · 59.94 fps · Rec.2100 PQ · Hardware (Nvidia)";
    case TransferKind::HLG: return L"3840x2160 · 29.97 fps · Rec.2100 HLG · Hardware (Nvidia)";
    case TransferKind::SDR: return L"1920x1080 · 29.97 fps · Rec.709 SDR · Hardware (Nvidia)";
    default:                return {};
    }
}

/**
 * @brief Wraps a path in double quotes for the command preview.
 */
std::wstring quoted(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

/**
 * @brief mkvmerge colour flags for a preset family (track 0), mirroring what
 *        PresetRegistry::colourFlags emits for the built-ins.
 */
std::wstring colourFlagsFor(TransferKind kind) {
    switch (kind) {
    case TransferKind::HLG:
        return L" --color-matrix-coefficients 0:9 --color-range 0:1 --color-transfer-characteristics 0:18 --color-primaries 0:9";
    case TransferKind::SDR:
        return L" --color-matrix-coefficients 0:1 --color-range 0:1 --color-transfer-characteristics 0:1 --color-primaries 0:1";
    case TransferKind::PQ:
    default:
        return L" --color-matrix-coefficients 0:9 --color-range 0:1 --color-transfer-characteristics 0:16 --color-primaries 0:9"
               L" --max-content-light 0:1000 --max-frame-light 0:400"
               L" --chromaticity-coordinates 0:0.708,0.292,0.170,0.797,0.131,0.046 --white-color-coordinates 0:0.3127,0.3290"
               L" --max-luminance 0:1000 --min-luminance 0:0.0001";
    }
}

/**
 * @brief The command line dispatch would launch for a row.
 */
std::wstring commandFor(const JobView& row) {
    if (row.outputPath.empty() || row.hintPath.empty()) {
        return {};
    }
    std::wstring cmd = quoted(kMkvmergePath);
    cmd += L" --ui-language en --output " + quoted(path::partialPathFor(row.hintPath));
    if (!row.presetId.empty()) {
        cmd += colourFlagsFor(presetKindFor(row.presetId));
    }
    if (row.attachLut && !row.lutPath.empty()) {
        cmd += L" --attach-file " + quoted(row.lutPath) + L" --attachment-mime-type application/x-cube";
    }
    cmd += L" " + quoted(row.outputPath);
    return cmd;
}

/**
 * @brief 0..1 -> whole percent, clamped.
 */
int percentOf(float progress) noexcept {
    return static_cast<int>(std::lround(std::clamp(progress, 0.0f, 1.0f) * 100.0f));
}

/**
 * @brief "1 job" / "3 jobs".
 */
std::wstring jobCountText(size_t count) {
    return std::format(L"{} {}", count, count == 1 ? L"job" : L"jobs");
}

/**
 * @brief Footer text shared by the real and the mock queue.
 */
std::wstring footerFor(const std::vector<JobView>& rows) {
    if (rows.empty()) {
        return L"No jobs";
    }
    const auto done = static_cast<size_t>(std::count_if(rows.begin(), rows.end(), [](const JobView& r) {
        return r.state == JobState::Done;
    }));
    return std::format(L"{}{}{} done", jobCountText(rows.size()), kDot, done);
}

} // namespace

// ===========================================================================
// MockQueueViewModel
// ===========================================================================

MockQueueViewModel::MockQueueViewModel() : jobs_(sampleJobs()) {}

void MockQueueViewModel::notify() {
    if (onChanged) {
        onChanged();
    }
}

JobView* MockQueueViewModel::find(JobId id, const wchar_t* who) {
    for (JobView& row : jobs_) {
        if (row.id == id) {
            return &row;
        }
    }
    HH_LOG_WARN(kLog, L"{}: unknown job {}", who ? who : L"?", id);
    return nullptr;
}

// ---- canned data ------------------------------------------------------------

JobView MockQueueViewModel::sampleJob(JobId id, JobState state, TransferKind transfer,
                                      const std::wstring& name, const std::wstring& folder) {
    JobView v;
    v.id = id;
    v.generation = 1;
    v.name = name.empty() ? std::wstring(L"export.mp4") : name;
    v.state = state;
    v.transfer = transfer;

    // Paths: the export and the hint next to it, exactly like the engine names them.
    const std::wstring dir = folder.empty() ? std::wstring(kExportFolder) : folder;
    v.outputPath = path::join(dir, v.name);
    v.hintPath = path::hintPathFor(v.outputPath, kSuffix, std::wstring());

    // Plan from the transfer's defaults.
    v.presetId = defaultPresetFor(transfer);
    v.lutPath = defaultLutFor(transfer);
    v.attachLut = !v.lutPath.empty();

    // Subtitle: video summary + output name (empty parts skipped).
    std::vector<std::wstring> parts;
    const std::wstring video = videoSummaryFor(transfer);
    if (!video.empty()) {
        parts.push_back(video);
    }
    parts.push_back(L"out: " + path::fileName(v.hintPath));
    v.subtitle = platform::join(parts, kDot);

    // State-specific facts (progress, reasons, errors).
    switch (state) {
    case JobState::Encoding:
        v.progress = 0.43f;
        break;
    case JobState::Muxing:
        v.progress = 0.61f;
        break;
    case JobState::Held:
        v.stateReason = L"Waiting for you";
        break;
    case JobState::Failed:
        v.stateReason = L"mkvmerge: The file could not be opened";
        v.lastError = L"mkvmerge: The file could not be opened";
        break;
    case JobState::SkippedSdr:
        v.stateReason = L"Rec. 709 export - nothing to do";
        break;
    case JobState::Cancelled:
        v.stateReason = L"Removed before completion";
        break;
    default:
        break;
    }
    refreshRow(v);
    return v;
}

std::vector<JobView> MockQueueViewModel::sampleJobs() {
    // One row per state, PQ / HLG / SDR / unknown mixed, realistic shop-episode names.
    std::vector<JobView> rows;
    rows.push_back(sampleJob(1,  JobState::Discovered, TransferKind::Unknown, L"Episode_49_Rivet_Line_Teaser.mp4",    kExportFolder));
    rows.push_back(sampleJob(2,  JobState::Encoding,   TransferKind::PQ,      L"Episode_48_Drilling_Flap_Holes.mp4",  kExportFolder));
    rows.push_back(sampleJob(3,  JobState::Ready,      TransferKind::HLG,     L"Wing_Skin_Prep_B-Roll.mov",           kExportFolder));
    rows.push_back(sampleJob(4,  JobState::Held,       TransferKind::Unknown, L"Cockpit_Wiring_Walkthrough.mp4",      kExportFolder));
    rows.push_back(sampleJob(5,  JobState::Muxing,     TransferKind::PQ,      L"Episode_47_Firewall_Forward.mp4",     kExportFolder));
    rows.push_back(sampleJob(6,  JobState::Verifying,  TransferKind::HLG,     L"Hangar_Tour_Autumn.mp4",              kExportFolder));
    rows.push_back(sampleJob(7,  JobState::Done,       TransferKind::PQ,      L"Episode_46_Engine_Mount_Fit.mp4",     kExportFolder));
    rows.push_back(sampleJob(8,  JobState::Failed,     TransferKind::PQ,      L"Fuel_System_Pressure_Test.mp4",       kExportFolder));
    rows.push_back(sampleJob(9,  JobState::SkippedSdr, TransferKind::SDR,     L"Shop_Vlog_Rec709_Cut.mp4",            kExportFolder));
    rows.push_back(sampleJob(10, JobState::Cancelled,  TransferKind::HLG,     L"Taxi_Test_Raw_Dump.mp4",              kExportFolder));
    return rows;
}

void MockQueueViewModel::refreshRow(JobView& row) {
    // Progress only means something while encoding or muxing.
    if (row.state != JobState::Encoding && row.state != JobState::Muxing) {
        row.progress = -1.0f;
    }
    row.held = row.held || row.state == JobState::Held;

    // Chip text (same rules as the app view-model).
    switch (row.state) {
    case JobState::Discovered:
        row.stateLabel = L"Detected";
        break;
    case JobState::Encoding:
        row.stateLabel = row.progress >= 0.0f ? std::format(L"Encoding {}%", percentOf(row.progress))
                                              : std::wstring(L"Encoding in AME");
        break;
    case JobState::Ready:
        row.stateLabel = L"Ready";
        break;
    case JobState::Held:
        row.stateLabel = L"Held";
        break;
    case JobState::Muxing:
        row.stateLabel = std::format(L"Muxing {}%", percentOf(row.progress < 0.0f ? 0.0f : row.progress));
        break;
    case JobState::Verifying:
        row.stateLabel = L"Verifying";
        break;
    case JobState::Done:
        row.stateLabel = L"Done";
        break;
    case JobState::Failed:
        row.stateLabel = L"Failed";
        break;
    case JobState::SkippedSdr:
        row.stateLabel = L"SDR - skipped";
        break;
    case JobState::Cancelled:
        row.stateLabel = L"Cancelled";
        break;
    }
    row.showProgress = (row.state == JobState::Encoding && row.progress >= 0.0f) || row.state == JobState::Muxing;

    // What the card may do.
    switch (row.state) {
    case JobState::Held:
    case JobState::Ready:
    case JobState::Failed:
    case JobState::SkippedSdr:
    case JobState::Cancelled:
    case JobState::Done:
        row.canRun = true;
        break;
    default:
        row.canRun = false;
        break;
    }
    switch (row.state) {
    case JobState::Failed:     row.runLabel = L"Retry";     break;
    case JobState::SkippedSdr: row.runLabel = L"Run anyway"; break;
    case JobState::Done:       row.runLabel = L"Run again";  break;
    default:                   row.runLabel = L"Run";        break;
    }
    row.canHold = (row.state == JobState::Ready || row.state == JobState::Encoding) && !row.held;
    switch (row.state) {
    case JobState::Discovered:
    case JobState::Encoding:
    case JobState::Ready:
    case JobState::Held:
    case JobState::Failed:
    case JobState::SkippedSdr:
    case JobState::Cancelled:
        row.canEditPlan = true;
        break;
    default:
        row.canEditPlan = false;
        break;
    }
    row.canRemove = true;
    // No disk in the harness: the export "exists" for every row, the hint once done.
    row.canRevealHint = row.state == JobState::Done;
    row.canReveal = !row.outputPath.empty() || row.canRevealHint;

    // A LUT that is not there cannot be attached; the error only belongs to Failed.
    if (row.lutPath.empty()) {
        row.attachLut = false;
    }
    if (row.state != JobState::Failed) {
        row.lastError.clear();
    }
    row.mkvmergeCommand = commandFor(row);
}

void MockQueueViewModel::setJobs(std::vector<JobView> rows) {
    for (JobView& row : rows) {
        refreshRow(row);
        // Keep the id generator ahead of whatever the caller handed us.
        if (row.id >= nextId_) {
            nextId_ = row.id + 1;
        }
    }
    jobs_ = std::move(rows);
    notify();
}

// ---- queries ----------------------------------------------------------------

std::vector<JobView> MockQueueViewModel::jobs() const {
    return jobs_;
}

std::vector<Choice> MockQueueViewModel::presetChoices(TransferKind t) const {
    return mockPresetChoices(t);
}

std::vector<Choice> MockQueueViewModel::lutChoices() const {
    return mockLutChoices();
}

bool MockQueueViewModel::autoProcess() const {
    return autoProcess_;
}

std::wstring MockQueueViewModel::footerSummary() const {
    return footerFor(jobs_);
}

// ---- commands ---------------------------------------------------------------

void MockQueueViewModel::run(JobId id) {
    JobView* row = find(id, L"run");
    if (!row) {
        return;
    }
    if (!row->canRun) {
        HH_LOG_INFO(kLog, L"run: job {} is {} - nothing to run", id, toString(row->state));
        return;
    }
    // Pretend dispatch happened and mkvmerge is a little way in.
    row->state = JobState::Muxing;
    row->progress = 0.12f;
    row->held = false;
    row->stateReason.clear();
    row->lastError.clear();
    refreshRow(*row);
    notify();
}

void MockQueueViewModel::hold(JobId id) {
    JobView* row = find(id, L"hold");
    if (!row) {
        return;
    }
    switch (row->state) {
    case JobState::Ready:
    case JobState::Held:
        row->state = JobState::Held;
        row->stateReason = L"Held";
        row->held = true;
        break;
    case JobState::Discovered:
    case JobState::Encoding:
        // Still being written: only the flag changes, the state follows later.
        row->held = true;
        break;
    default:
        HH_LOG_INFO(kLog, L"hold: job {} is {} - nothing to hold", id, toString(row->state));
        return;
    }
    refreshRow(*row);
    notify();
}

void MockQueueViewModel::resume(JobId id) {
    JobView* row = find(id, L"resume");
    if (!row) {
        return;
    }
    row->held = false;
    if (row->state == JobState::Held) {
        row->state = JobState::Ready;
        row->stateReason.clear();
    }
    refreshRow(*row);
    notify();
}

void MockQueueViewModel::remove(JobId id) {
    const auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const JobView& r) { return r.id == id; });
    if (it == jobs_.end()) {
        HH_LOG_WARN(kLog, L"remove: unknown job {}", id);
        return;
    }
    jobs_.erase(it);
    notify();
}

void MockQueueViewModel::reveal(JobId id, bool hintFile) {
    // The harness never opens Explorer; remember the request for assertions.
    if (const JobView* row = find(id, L"reveal")) {
        lastRevealed_ = id;
        HH_LOG_INFO(kLog, L"reveal {} ({})", hintFile ? row->hintPath : row->outputPath, hintFile ? L"hint" : L"source");
    }
}

void MockQueueViewModel::setPreset(JobId id, const std::wstring& presetId) {
    JobView* row = find(id, L"setPreset");
    if (!row) {
        return;
    }
    // Empty = back to the transfer's default, like the engine.
    row->presetId = presetId.empty() ? defaultPresetFor(row->transfer) : presetId;
    refreshRow(*row);
    notify();
}

void MockQueueViewModel::setLut(JobId id, const std::wstring& lutPath) {
    JobView* row = find(id, L"setLut");
    if (!row) {
        return;
    }
    row->lutPath = lutPath;
    // Picking a LUT implies attaching it; "None" switches the attachment off.
    row->attachLut = !lutPath.empty();
    refreshRow(*row);
    notify();
}

/// The mock has no shell dialogs; pretend the user picked a file off D:.
void MockQueueViewModel::browseLut(JobId id) {
    setLut(id, path::join(kUserLutFolder, L"Picked_From_Disk.cube"));
}

void MockQueueViewModel::setAttachLut(JobId id, bool attach) {
    JobView* row = find(id, L"setAttachLut");
    if (!row) {
        return;
    }
    row->attachLut = attach && !row->lutPath.empty();
    refreshRow(*row);
    notify();
}

void MockQueueViewModel::copyCommand(JobId id) {
    const JobView* row = find(id, L"copyCommand");
    if (!row) {
        return;
    }
    if (row->mkvmergeCommand.empty()) {
        HH_LOG_INFO(kLog, L"copyCommand: job {} has no command", id);
        return;
    }
    if (!copyTextToClipboard(nullptr, row->mkvmergeCommand)) {
        HH_LOG_WARN(kLog, L"copyCommand: clipboard write failed");
    }
}

void MockQueueViewModel::setAutoProcess(bool on) {
    if (autoProcess_ == on) {
        return;
    }
    autoProcess_ = on;
    // Auto-process on releases the rows that were waiting for the user.
    if (on) {
        for (JobView& row : jobs_) {
            if (row.state == JobState::Held && platform::iequals(row.stateReason, L"Waiting for you")) {
                row.state = JobState::Ready;
                row.stateReason.clear();
                row.held = false;
                refreshRow(row);
            }
        }
    }
    notify();
}

void MockQueueViewModel::addFiles(const std::vector<std::wstring>& paths) {
    bool added = false;
    for (const std::wstring& p : paths) {
        const std::wstring trimmed(platform::trim(p));
        if (trimmed.empty()) {
            continue;
        }
        // Dropped files are detected, not yet probed - newest first like the engine.
        const std::wstring folder = path::parent(trimmed);
        JobView row = sampleJob(nextId_++, JobState::Discovered, TransferKind::Unknown, path::fileName(trimmed),
                                folder.empty() ? std::wstring(kExportFolder) : folder);
        jobs_.insert(jobs_.begin(), std::move(row));
        added = true;
    }
    if (added) {
        notify();
    }
}

// ===========================================================================
// MockSettingsViewModel
// ===========================================================================

MockSettingsViewModel::MockSettingsViewModel() : view_(sampleView()) {}

void MockSettingsViewModel::notify() {
    if (onChanged) {
        onChanged();
    }
}

SettingsView MockSettingsViewModel::sampleView() {
    SettingsView v;
    // Tools: auto-located mkvmerge, LUTs next to the app.
    v.mkvmergePath.clear();
    v.mkvmergeStatus = std::wstring(L"v82.0") + kDot + kMkvmergeFolder;
    v.mkvmergeOk = true;
    v.lutFolder = kLutFolder;
    // Defaults.
    v.defaultLutPq = path::join(kLutFolder, kPqLutName);
    v.defaultLutHlg.clear();
    v.presetPq = L"generic_pq_1000";
    v.presetHlg = L"generic_hlg";
    v.suffix = kSuffix;
    v.attachLutByDefault = true;
    // Watch folders.
    v.watchFolders = {kExportFolder, kClientFolder};
    // Behaviour.
    v.autoProcess = true;
    v.autoProcessAme = true;
    v.autoProcessWatched = true;
    v.recycleOriginal = true;
    v.dockInsideAme = true;
    v.alwaysOnTop = true;
    v.minimizeToTray = true;
    v.startMinimized = false;
    v.startWithWindows = false;
    v.quitWithAme = true;
    v.showOnAmeLaunch = true;
    v.appearance = 0;
    v.accent = 0;
    v.reduceTransparency = false;
    // Media Encoder.
    v.panelStatus = L"Installed v1.0.0";
    v.panelInstalled = true;
    v.aboutLine = kAboutLine;
    return v;
}

void MockSettingsViewModel::refreshStatus() {
    // An explicit path that is not mkvmerge.exe reads as "not found"; anything
    // else is the shipped v82.0.
    const std::wstring name = path::fileName(view_.mkvmergePath);
    if (!view_.mkvmergePath.empty() && !platform::iequals(name, platform::terms::kMkvmergeExe)) {
        view_.mkvmergeOk = false;
        view_.mkvmergeStatus = L"not found - install MKVToolNix";
        view_.aboutLine = L"HDR Hint 1.0.0 · mkvmerge not found";
        return;
    }
    const std::wstring folder = view_.mkvmergePath.empty() ? std::wstring(kMkvmergeFolder) : path::parent(view_.mkvmergePath);
    view_.mkvmergeOk = true;
    view_.mkvmergeStatus = std::wstring(L"v82.0") + kDot + folder;
    view_.aboutLine = kAboutLine;
}

void MockSettingsViewModel::setView(SettingsView view) {
    view_ = std::move(view);
    notify();
}

// ---- queries ----------------------------------------------------------------

SettingsView MockSettingsViewModel::view() const {
    return view_;
}

std::vector<Choice> MockSettingsViewModel::presetChoices(TransferKind t) const {
    return mockPresetChoices(t);
}

std::vector<Choice> MockSettingsViewModel::lutChoices() const {
    return mockLutChoices();
}

// ---- setters ----------------------------------------------------------------

void MockSettingsViewModel::setMkvmergePath(const std::wstring& path) {
    view_.mkvmergePath = std::wstring(platform::trim(path));
    refreshStatus();
    notify();
}

void MockSettingsViewModel::browseMkvmerge() {
    // The "dialog" always picks the standard install.
    setMkvmergePath(kMkvmergePath);
}

void MockSettingsViewModel::setLutFolder(const std::wstring& path) {
    view_.lutFolder = std::wstring(platform::trim(path));
    notify();
}

void MockSettingsViewModel::browseLutFolder() {
    setLutFolder(kUserLutFolder);
}

/// The mock has no shell dialogs; pretend the user picked a file off D:.
void MockSettingsViewModel::browseLut(TransferKind t) {
    setDefaultLut(t, path::join(kUserLutFolder, L"Picked_From_Disk.cube"));
}

void MockSettingsViewModel::setDefaultLut(TransferKind t, const std::wstring& path) {
    switch (t) {
    case TransferKind::PQ:
        view_.defaultLutPq = path;
        break;
    case TransferKind::HLG:
        view_.defaultLutHlg = path;
        break;
    default:
        HH_LOG_WARN(kLog, L"setDefaultLut: no slot for {}", toString(t));
        return;
    }
    notify();
}

void MockSettingsViewModel::setDefaultPreset(TransferKind t, const std::wstring& presetId) {
    if (presetId.empty()) {
        return;
    }
    switch (t) {
    case TransferKind::PQ:
        view_.presetPq = presetId;
        break;
    case TransferKind::HLG:
        view_.presetHlg = presetId;
        break;
    default:
        HH_LOG_WARN(kLog, L"setDefaultPreset: no slot for {}", toString(t));
        return;
    }
    notify();
}

void MockSettingsViewModel::setSuffix(const std::wstring& suffix) {
    const std::wstring trimmed(platform::trim(suffix));
    // Same rules as the app: non-empty and file-name safe.
    if (trimmed.empty() || path::hasInvalidFileNameChars(trimmed)) {
        HH_LOG_WARN(kLog, L"setSuffix: '{}' rejected", suffix);
        return;
    }
    view_.suffix = trimmed;
    notify();
}

void MockSettingsViewModel::setAttachLutByDefault(bool on) {
    view_.attachLutByDefault = on;
    notify();
}

void MockSettingsViewModel::addWatchFolder() {
    // Each "pick" yields a fresh folder so repeated clicks are visible.
    ++addedFolders_;
    std::wstring folder = kShortsFolder;
    if (addedFolders_ > 1) {
        folder += std::format(L" {}", addedFolders_);
    }
    view_.watchFolders.push_back(std::move(folder));
    notify();
}

void MockSettingsViewModel::removeWatchFolder(const std::wstring& folder) {
    auto& list = view_.watchFolders;
    const size_t before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&folder](const std::wstring& f) {
        return platform::iequals(f, folder);
    }), list.end());
    if (list.size() == before) {
        HH_LOG_WARN(kLog, L"removeWatchFolder: '{}' not in the list", folder);
        return;
    }
    notify();
}

void MockSettingsViewModel::setAutoProcess(bool on) {
    view_.autoProcess = on;
    notify();
}

void MockSettingsViewModel::setAutoProcessAme(bool on) {
    view_.autoProcessAme = on;
    notify();
}

void MockSettingsViewModel::setAutoProcessWatched(bool on) {
    view_.autoProcessWatched = on;
    notify();
}

void MockSettingsViewModel::setRecycleOriginal(bool on) {
    view_.recycleOriginal = on;
    notify();
}

void MockSettingsViewModel::setDockInsideAme(bool on) {
    view_.dockInsideAme = on;
    notify();
}

void MockSettingsViewModel::setAlwaysOnTop(bool on) {
    view_.alwaysOnTop = on;
    notify();
}

void MockSettingsViewModel::setMinimizeToTray(bool on) {
    view_.minimizeToTray = on;
    notify();
}

void MockSettingsViewModel::setStartMinimized(bool on) {
    view_.startMinimized = on;
    notify();
}

void MockSettingsViewModel::setStartWithWindows(bool on) {
    view_.startWithWindows = on;
    notify();
}

void MockSettingsViewModel::setShowOnAmeLaunch(bool on) {
    view_.showOnAmeLaunch = on;
    notify();
}

void MockSettingsViewModel::setQuitWithAme(bool on) {
    view_.quitWithAme = on;
    notify();
}

void MockSettingsViewModel::setAppearance(int mode) {
    view_.appearance = (mode >= 0 && mode <= 2) ? mode : 0;
    notify();
}

void MockSettingsViewModel::setAccent(int mode) {
    view_.accent = mode == 1 ? 1 : 0;
    notify();
}

void MockSettingsViewModel::setReduceTransparency(bool on) {
    view_.reduceTransparency = on;
    notify();
}

void MockSettingsViewModel::installPanel() {
    view_.panelInstalled = true;
    view_.panelStatus = L"Installed v1.0.0";
    notify();
}

void MockSettingsViewModel::openLogFolder() {
    // Nothing to open in the harness.
    HH_LOG_INFO(kLog, L"openLogFolder requested");
}

void MockSettingsViewModel::reprobeMkvmerge() {
    refreshStatus();
    notify();
}

// ===========================================================================
// MockLinkViewModel
// ===========================================================================

MockLinkViewModel::MockLinkViewModel() {
    LinkView v;
    v.panelLinked = true;
    v.logFound = true;
    v.queueRunning = false;
    v.docked = false;
#if !defined(_WIN32)
    // Docking is a Windows feature; the Mac screenshots show no dock toggle.
    v.dockingSupported = false;
#endif
    setLink(std::move(v));
}

void MockLinkViewModel::notify() {
    if (onChanged) {
        onChanged();
    }
}

void MockLinkViewModel::setLink(LinkView link) {
    link_ = std::move(link);
    // Derive the tooltip from the flags unless the caller wrote one.
    if (link_.tooltip.empty()) {
        std::vector<std::wstring> lines;
        lines.push_back(link_.panelLinked ? L"Media Encoder panel connected" : L"Media Encoder panel not connected");
        lines.push_back(link_.logFound ? std::wstring(L"AME log: ") + kAmeLogPath : std::wstring(L"AME log not found"));
        lines.push_back(link_.queueRunning ? L"Queue running" : L"Queue idle");
        lines.push_back(L"mkvmerge v82.0");
        if (link_.docked) {
            lines.push_back(L"Docked inside Media Encoder");
        }
        link_.tooltip = platform::join(lines, L"\n");
    }
    notify();
}

LinkView MockLinkViewModel::link() const {
    return link_;
}

void MockLinkViewModel::toggleDock() {
    LinkView next = link_;
    next.docked = !next.docked;
    next.tooltip.clear();   // rebuilt from the new flags
    setLink(std::move(next));
}

// ===========================================================================
// MockGuideViewModel
// ===========================================================================

MockGuideViewModel::MockGuideViewModel(const ISettingsViewModel* settings) : settings_(settings) {}

std::wstring MockGuideViewModel::markdown() const {
    // The real loader already falls back to the built-in text, so screenshots
    // show the genuine guide wherever it can be found.
    if (markdownCache_.empty()) {
        markdownCache_ = loadGuideMarkdown();
    }
    return markdownCache_;
}

std::wstring MockGuideViewModel::summaryText() const {
    if (settings_) {
        const SettingsView view = settings_->view();
        return guideSummaryText(&view);
    }
    return guideSummaryText(nullptr);
}

void MockGuideViewModel::copySummary() {
    lastCopied_ = summaryText();
    if (lastCopied_.empty()) {
        HH_LOG_WARN(kLog, L"copySummary: summary is empty");
        return;
    }
    if (!copyTextToClipboard(nullptr, lastCopied_)) {
        HH_LOG_WARN(kLog, L"copySummary: clipboard write failed");
    }
}

void MockGuideViewModel::openGuideFile() {
    ++openRequests_;
    HH_LOG_INFO(kLog, L"openGuideFile requested ({} so far)", openRequests_);
}

// ===========================================================================
// mockApplyState
// ===========================================================================

void mockApplyState(MockQueueViewModel& vm, int scenario) {
    switch (scenario) {
    case 1:
        // Empty queue: the EmptyState illustration.
        vm.setJobs({});
        return;
    case 2: {
        // One finished job: the "all good" card with Reveal enabled.
        std::vector<JobView> rows;
        rows.push_back(MockQueueViewModel::sampleJob(7, JobState::Done, TransferKind::PQ,
                                                     L"Episode_46_Engine_Mount_Fit.mp4", kExportFolder));
        vm.setJobs(std::move(rows));
        return;
    }
    case 0:
        break;
    default:
        HH_LOG_WARN(kLog, L"mockApplyState: unknown scenario {}; showing the full queue", scenario);
        break;
    }
    vm.setJobs(MockQueueViewModel::sampleJobs());
}

} // namespace hh::ui
