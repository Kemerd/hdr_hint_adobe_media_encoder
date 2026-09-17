// ---------------------------------------------------------------------------
// Engine.cpp - the orchestrator.
//
// The engine owns the worker threads (log tailer, folder watcher, readiness
// prober, mux worker, IPC server) and the job state machine. Workers never
// touch job state directly: they post EngineEvent values into the queue and
// the UI thread applies them in onEventMessage(). Every public method on this
// class is therefore UI-thread only, which keeps the whole state machine free
// of locks.
//
// Layout of this file:
//   1. file-local helpers (constants, scratch state, small utilities)
//   2. construction / start / stop / event pump / tick
//   3. queries
//   4. AME log events
//   5. folder watcher events
//   6. readiness probe events
//   7. plan resolution + dispatch
//   8. mux events, completion and the Recycle Bin step
//   9. user commands
//  10. IPC (panel + CEP bridge)
//  11. headless one-shot processing
//  12. registry / launcher.json / LUT listing / notifications
// ---------------------------------------------------------------------------
#include "core/Engine.h"

#include "core/IpcProtocol.h"

#include "core/Logger.h"
#include "core/MediaProbe.h"
#include "core/Mp4Boxes.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Handle.h"
#include "platform/KnownFolders.h"
#include "platform/RecycleBin.h"
#include "platform/Registry.h"
#include "platform/Time.h"
#include "platform/Utf.h"
#include "platform/Win.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hh {

// ===========================================================================
// 1. File-local helpers
// ===========================================================================
namespace {

using platform::icontains;
using platform::iequals;
using platform::toUtf8;
using platform::toWide;

/// Logger component tag for every line written from this file.
constexpr const wchar_t* kLog = L"Engine";
/// Application version reported to panels and the registry.
constexpr const wchar_t* kAppVersion = L"1.0.0";
/// Registry key the CEP panel reads to find the executable.
constexpr const wchar_t* kRegistryKey = L"Software\\HdrHint";
/// Named-pipe protocol version advertised in the registry and launcher.json.
constexpr DWORD kProtocolVersion = 1;
/// jobs.json is written at most this often while the store keeps changing.
constexpr uint64_t kSaveDebounceMs = 500;
/// FILETIME ticks (100 ns) per second / minute / millisecond.
constexpr uint64_t kTicksPerMs = 10000ull;
constexpr uint64_t kTicksPerSecond = 10000000ull;
constexpr uint64_t kTicksPerMinute = 60ull * kTicksPerSecond;
/// How many LUT paths the "recent" list keeps.
constexpr size_t kRecentLutsMax = 10;
/// Cap on the free-form notes kept per job (oldest dropped).
constexpr size_t kMaxNotesPerJob = 50;
/// Synchronous ffprobe budget when a job reaches Ready with an unknown transfer.
constexpr DWORD kReadyProbeTimeoutMs = 10000;
/// Longest hint path mkvmerge is asked to write.
constexpr size_t kMaxHintPathChars = 32000;
/// Upper bound on drain rounds per kick so a flood cannot starve the UI.
constexpr int kMaxDrainRounds = 64;
/// Key in cepStartedPaths_ that remembers the most recent encodingStarted path.
constexpr const char* kLastStartedKey = "last";

// Human reasons shown in the Held / Failed / Skipped rows. Some of them are
// matched again later (setAutoProcess looks for kReasonWaiting, the mkvmerge
// re-probe looks for the word "mkvmerge"), so keep the wording stable.
constexpr const wchar_t* kReasonWaiting = L"Waiting for you";
constexpr const wchar_t* kReasonHeld = L"Held";
constexpr const wchar_t* kReasonCatchUpLog = L"Found in the AME log after HDR Hint started";
constexpr const wchar_t* kReasonCatchUpFolder = L"Found in a watched folder after HDR Hint started";
constexpr const wchar_t* kReasonUnknownTransfer = L"Colour space unknown - choose a preset";
constexpr const wchar_t* kReasonSdrSkip = L"Rec. 709 export - nothing to do";
constexpr const wchar_t* kReasonSdrHold = L"Rec. 709 export - choose what to do";
constexpr const wchar_t* kReasonInbandHold = L"Source carries HDR10 metadata - choose a preset";
constexpr const wchar_t* kReasonInterrupted = L"Interrupted by app exit";
constexpr const wchar_t* kReasonOutputRemoved = L"Output removed before completion";
constexpr const wchar_t* kReasonSourceGone = L"Source file disappeared";
constexpr const wchar_t* kReasonSourceMissing = L"Source file not found";
constexpr const wchar_t* kReasonNoMuxWorker = L"Mux worker is not running";

/**
 * @brief Book-keeping the engine needs but the frozen header has no member for.
 *
 * Everything in here is touched from the UI thread only (the same contract as
 * Engine itself). There is exactly one Engine per process, so process-wide
 * scratch state is equivalent to a member; it lives here so the public header
 * stays untouched.
 *
 *  - probes:   jobs that currently have a live FileReadiness probe. The prober
 *              has no query API and re-scheduling would reset its stability
 *              timer and confirmation flag, so the engine tracks it.
 *  - forced:   jobs the user explicitly asked to run; onReady() skips the
 *              hold / auto-process / SDR-skip policies exactly once for them.
 *  - drainDepth / jobsChanged / linkChanged: notification coalescing while
 *              onEventMessage() drains a batch of worker events.
 */
struct UiThreadScratch {
    std::set<JobId> probes;
    std::set<JobId> forced;
    int drainDepth = 0;
    bool jobsChanged = false;
    bool linkChanged = false;
};

/// The process-wide scratch block (function-local static: constructed on first use).
UiThreadScratch& scratch() {
    static UiThreadScratch s;
    return s;
}

/// True when a readiness probe is believed to be live for the job.
bool probeTracked(JobId id) {
    return scratch().probes.count(id) != 0;
}

/// Forgets the probe book-keeping for a job (the prober already dropped it).
void untrackProbe(JobId id) {
    scratch().probes.erase(id);
}

/**
 * @brief Schedules (or re-schedules) a readiness probe for a job.
 *
 * FileReadiness::schedule replaces an existing probe for the id, which resets
 * its timers, so callers only call this when probeTracked() is false or when a
 * fresh probe is explicitly wanted (re-runs).
 */
void scheduleProbe(FileReadiness* readiness, const Job& job, bool requireConfirmation) {
    // Without a prober there is nothing to schedule; the caller logs context.
    if (!readiness) {
        HH_LOG_WARN(kLog, L"scheduleProbe: no readiness worker for job {}", job.id);
        return;
    }
    if (job.outputPath.empty()) {
        HH_LOG_WARN(kLog, L"scheduleProbe: job {} has no output path", job.id);
        return;
    }
    // Hand the path to the prober and remember that a probe is live.
    readiness->schedule(job.id, job.outputPath, requireConfirmation);
    scratch().probes.insert(job.id);
    HH_LOG_DEBUG(kLog, L"probe scheduled for job {} ({}) confirm={}", job.id, job.outputPath, requireConfirmation);
}

/// Cancels a live probe (if any) and forgets it.
void cancelProbe(FileReadiness* readiness, JobId id) {
    if (readiness && probeTracked(id)) {
        readiness->cancel(id);
    }
    untrackProbe(id);
}

/// Appends a note to a job, dropping the oldest when the cap is reached.
void addNote(Job& job, std::wstring text) {
    if (text.empty()) {
        return;
    }
    // Keep the list bounded so jobs.json cannot grow without limit.
    while (job.notes.size() >= kMaxNotesPerJob) {
        job.notes.erase(job.notes.begin());
    }
    job.notes.push_back(std::move(text));
}

/// Converts a platform file identity into the job model's SourceStamp.
SourceStamp stampFrom(const platform::FileIdentity& id) {
    SourceStamp s;
    s.size = id.size;
    s.lastWriteUtc = id.lastWriteUtc;
    s.creationUtc = id.creationUtc;
    s.fileIdLow = id.fileIdLow;
    s.fileIdHigh = id.fileIdHigh;
    s.volumeSerial = id.volumeSerial;
    s.hasFileId = id.hasFileId;
    s.valid = id.valid;
    return s;
}

/// Minutes -> FILETIME ticks (clamped at zero).
uint64_t minutesToTicks(int minutes) {
    if (minutes <= 0) {
        return 0;
    }
    return static_cast<uint64_t>(minutes) * kTicksPerMinute;
}

/**
 * @brief Normalises a folder for the watch lists: absolute, backslashes,
 *        no trailing separator (except a bare volume root).
 */
std::wstring normalizeFolder(std::wstring_view folder) {
    std::wstring trimmed(platform::trim(folder));
    if (trimmed.empty()) {
        return {};
    }
    // GetFullPathNameW converts forward slashes and resolves relative pieces.
    std::wstring full = platform::fullPath(trimmed);
    if (full.empty()) {
        full = path::normalizeSeparators(trimmed);
    }
    // Strip a trailing separator unless that would leave "C:" (keep "C:\").
    while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) {
        full.pop_back();
    }
    return full;
}

/// Case-insensitive membership test for folder lists.
bool containsFolder(const std::vector<std::wstring>& list, std::wstring_view folder) {
    for (const auto& f : list) {
        if (iequals(f, folder)) {
            return true;
        }
    }
    return false;
}

/// Case-insensitive test of a folder against the ignore list (normalised on the fly).
bool isIgnoredFolder(const Settings& settings, std::wstring_view folder) {
    for (const auto& raw : settings.ignoreFolders) {
        const std::wstring norm = normalizeFolder(settings.expand(raw));
        if (!norm.empty() && iequals(norm, folder)) {
            return true;
        }
    }
    return false;
}

/// True when an AME result string means the encode did not succeed.
bool isAmeFailureResult(std::wstring_view result) {
    static constexpr const wchar_t* kWords[] = {L"fail", L"error", L"abort", L"cancel", L"stop"};
    for (const wchar_t* w : kWords) {
        if (icontains(result, w)) {
            return true;
        }
    }
    return false;
}

/// Serialises JSON without ever throwing on odd UTF-8 (replacement characters instead).
std::string dumpJson(const nlohmann::json& j, int indent) {
    return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// Trims a UTF-8 line for logging so a giant payload cannot flood the log.
std::wstring previewLine(const std::string& line) {
    constexpr size_t kMax = 200;
    if (line.size() <= kMax) {
        return toWide(line);
    }
    return toWide(line.substr(0, kMax)) + L"...";
}

/// Outcome of re-checking a job's source on disk before a re-run / re-evaluation.
enum class SourceCheck { StillReady, NeedsProbe, Missing };

/**
 * @brief Cheap disk check used by the commands: is the file the same one the
 *        probe saw when the job became Ready, and is nobody writing it?
 */
SourceCheck checkSource(const Job& job) {
    if (job.outputPath.empty() || !platform::isFile(job.outputPath)) {
        return SourceCheck::Missing;
    }
    // A job that never went through the prober has no stamp to compare with.
    if (!job.sourceStamp.valid) {
        return SourceCheck::NeedsProbe;
    }
    const auto ident = platform::identity(job.outputPath);
    if (!ident) {
        return SourceCheck::NeedsProbe;
    }
    const SourceStamp now = stampFrom(ident.value());
    if (!now.sameFileAs(job.sourceStamp) || now.size != job.sourceStamp.size) {
        return SourceCheck::NeedsProbe;
    }
    // The stamp matches; make sure no writer holds the file right now.
    if (platform::probeDenyWrite(job.outputPath) != platform::OpenProbe::Ready) {
        return SourceCheck::NeedsProbe;
    }
    return SourceCheck::StillReady;
}

/// The TransferKind used for plan resolution when the real one is unknown.
TransferKind planKindFor(TransferKind t) {
    return t == TransferKind::Unknown ? TransferKind::PQ : t;
}

/// Builds the watcher configuration from the settings.
WatcherConfig makeWatcherConfig(const Settings& s) {
    WatcherConfig c;
    c.extensions = s.extensions;
    c.suffix = s.suffix;
    c.debounceMs = std::max(0, s.debounceMs);
    c.sidecarDetection = s.sidecarDetection;
    c.catchUpMinutes = std::max(0, s.catchUpMinutes);
    return c;
}

/// Builds the readiness prober configuration from the settings.
ReadinessConfig makeReadinessConfig(const Settings& s) {
    ReadinessConfig c;
    c.probeIntervalMs = std::max(100, s.probeIntervalMs);
    c.maxIntervalMs = std::max(c.probeIntervalMs, 5000);
    c.stableSeconds = std::max(0, s.stableSeconds);
    c.minOutputBytes = s.minOutputBytes;
    c.checkStructure = s.checkStructure;
    c.requireConfirmation = s.requireLogConfirmation;
    c.confirmTimeoutS = std::max(0, s.logConfirmTimeoutS);
    c.encodingTimeoutHours = std::max(1, s.encodingTimeoutHours);
    c.missingGraceS = std::max(0, s.missingGraceS);
    return c;
}

/// Builds the log tailer configuration from the settings.
TailerConfig makeTailerConfig(const Settings& s) {
    TailerConfig c;
    // Overrides may use %ENV% / <exe>; expand them before the tailer sees them.
    for (const auto& raw : s.logPathOverrides) {
        const std::wstring expanded = s.expand(raw);
        if (!expanded.empty()) {
            c.logOverrides.push_back(expanded);
        }
    }
    c.pollMs = std::max(250, s.logPollMs);
    c.preferDayFirstDates = s.dateDayFirst;
    c.recentFoldersCount = std::max(0, s.watchRecentFoldersCount);
    c.statePath = platform::appLocalDataFolder() + L"\\state.json";
    c.rediscoverSeconds = 60;
    return c;
}

/// Progress percentage text for the Encoding phase ("Encoding 42%").
std::wstring encodingPhaseText(float progress) {
    if (progress < 0.0f) {
        return L"Encoding in AME";
    }
    const int pct = static_cast<int>(std::lround(std::clamp(progress, 0.0f, 1.0f) * 100.0f));
    return std::format(L"Encoding {}%", pct);
}

} // namespace

// ===========================================================================
// 2. Construction / start / stop / event pump / tick
// ===========================================================================

/**
 * @brief Stores the references and works out where jobs.json lives.
 *        Nothing runs until start().
 */
Engine::Engine(Settings& settings, PresetRegistry& presets)
    : settings_(settings), presets_(presets) {
    jobsPath_ = platform::appLocalDataFolder() + L"\\jobs.json";
    HH_LOG_DEBUG(kLog, L"engine constructed; jobs file {}", jobsPath_);
}

/**
 * @brief Stops everything that is still running. A running mux is cancelled:
 *        destruction is the last resort, the app asks stop(true) first.
 */
Engine::~Engine() {
    if (started_) {
        stop(false);
    }
}

/**
 * @brief Starts the workers and re-arms probes for jobs that survived a restart.
 */
Result<void> Engine::start(HWND eventTarget, UINT eventMessage) {
    if (started_) {
        HH_LOG_WARN(kLog, L"start() called twice; ignoring");
        return Result<void>::success();
    }
    if (!eventTarget) {
        return Error::text(L"Engine::start: no event window");
    }
    HH_LOG_INFO(kLog, L"starting engine (message 0x{:X})", eventMessage);

    // The queue must know where to kick before any worker can post.
    queue_.setTarget(eventTarget, eventMessage);

    // ---- persisted state ---------------------------------------------------
    // A broken jobs.json must never stop the app; log and continue with an
    // empty table.
    if (auto loaded = store_.load(jobsPath_); !loaded) {
        HH_LOG_WARN(kLog, L"could not load jobs: {}", loaded.error().toString());
    } else {
        HH_LOG_INFO(kLog, L"loaded {} jobs from {}", store_.size(), jobsPath_);
    }

    // User presets live next to the settings; a missing file is fine.
    const std::wstring presetsFile = settings_.expand(settings_.userPresetsFile);
    if (!presetsFile.empty()) {
        if (auto r = presets_.loadUser(presetsFile); !r) {
            HH_LOG_WARN(kLog, L"could not load user presets: {}", r.error().toString());
        }
    }

    // ---- tools ---------------------------------------------------------------
    // Locate mkvmerge directly here (reprobeMkvmerge() also re-evaluates held
    // jobs, which needs the workers to exist first).
    mkvmerge_ = locateMkvmerge(settings_.expand(settings_.mkvmergePath), settings_.minMajorVersion);
    if (mkvmerge_.ok) {
        HH_LOG_INFO(kLog, L"mkvmerge {} at {}", mkvmerge_.shortVersion(), mkvmerge_.path);
    } else {
        HH_LOG_WARN(kLog, L"mkvmerge unavailable: {}", mkvmerge_.error);
    }
    ffprobePath_ = locateFfprobe();
    if (ffprobePath_.empty()) {
        HH_LOG_INFO(kLog, L"ffprobe not found; colour space comes from the log / CEP only");
    } else {
        HH_LOG_INFO(kLog, L"ffprobe at {}", ffprobePath_);
    }
    refreshLuts();

    // ---- workers ---------------------------------------------------------------
    // Fresh instances every start so a stop()/start() cycle never reuses a
    // joined thread object.
    tailer_ = std::make_unique<AmeLogTailer>(queue_);
    watcher_ = std::make_unique<FolderWatcher>(queue_);
    readiness_ = std::make_unique<FileReadiness>(queue_);
    mux_ = std::make_unique<MuxWorker>(queue_);
    ipc_ = std::make_unique<IpcServer>(queue_);

    readiness_->start(makeReadinessConfig(settings_));
    mux_->start();

    // The watcher starts with the configured extra folders plus whatever the
    // log history taught us last time (knownFolders_ fills in as the tailer
    // seeds, so this is usually just the extras at this point).
    watcher_->start(makeWatcherConfig(settings_));
    {
        std::vector<std::wstring> folders;
        for (const auto& raw : settings_.extraWatchFolders) {
            const std::wstring norm = normalizeFolder(settings_.expand(raw));
            if (!norm.empty() && !containsFolder(folders, norm)) {
                folders.push_back(norm);
            }
        }
        for (const auto& known : knownFolders_) {
            if (!containsFolder(folders, known)) {
                folders.push_back(known);
            }
        }
        watcher_->setFolders(folders);
    }

    if (settings_.watchLog) {
        tailer_->start(makeTailerConfig(settings_));
    } else {
        HH_LOG_INFO(kLog, L"AME log watching is disabled in settings");
    }

    // IPC is optional: the app is fully usable without the panel.
    if (settings_.ipcEnabled) {
        if (auto r = ipc_->start(settings_.pipeName, std::max(1, settings_.maxInstances)); !r) {
            HH_LOG_WARN(kLog, L"IPC server failed to start: {}", r.error().toString());
        }
    }

    // ---- repair loaded jobs ---------------------------------------------------
    // Jobs that were mid-mux when the app died are failed (the partial file is
    // orphaned); anything that was still waiting for its file is re-probed.
    for (Job* job : store_.all()) {
        if (!job) {
            continue;
        }
        switch (job->state) {
        case JobState::Muxing:
        case JobState::Verifying:
            transition(*job, JobState::Failed, kReasonInterrupted);
            break;
        case JobState::Discovered:
        case JobState::Encoding:
        case JobState::Ready:
            // No log is expected for a job from a previous session, so the
            // probe must not wait for confirmation.
            scheduleProbe(readiness_.get(), *job, false);
            break;
        default:
            break;
        }
    }

    // ---- registration ----------------------------------------------------------
    if (settings_.writeRegistry) {
        updateRegistry();
    }

    started_ = true;
    notifyLink();
    notifyJobs();
    HH_LOG_INFO(kLog, L"engine started");
    return Result<void>::success();
}

/**
 * @brief Stops the workers in dependency order and persists state.
 *
 * The mux worker is stopped last. MuxWorker::stop() joins its thread; when
 * @p waitForMux is true the current job is *not* cancelled so the join waits
 * for mkvmerge to finish naturally.
 */
void Engine::stop(bool waitForMux) {
    if (!started_) {
        return;
    }
    HH_LOG_INFO(kLog, L"stopping engine (waitForMux={})", waitForMux);

    // A running mux is cancelled unless the caller wants it to finish.
    if (mux_ && mux_->busy() && !waitForMux) {
        const JobId current = mux_->currentJob();
        if (current != 0) {
            HH_LOG_WARN(kLog, L"cancelling running mux for job {}", current);
            mux_->cancel(current);
            if (Job* job = store_.find(current)) {
                if (job->state == JobState::Muxing || job->state == JobState::Verifying) {
                    transition(*job, JobState::Cancelled, L"Cancelled at exit");
                }
            }
        }
    }

    // Order: producers first, the mux last (it may still be finishing).
    if (tailer_) {
        tailer_->stop();
    }
    if (watcher_) {
        watcher_->stop();
    }
    if (readiness_) {
        readiness_->stop();
    }
    if (ipc_) {
        ipc_->stop();
    }
    if (mux_) {
        mux_->stop();
    }

    // Anything the workers posted after the last drain is applied now so the
    // saved state reflects it (e.g. a mux that finished during the join).
    onEventMessage();

    // Drop the worker objects; start() creates fresh ones.
    tailer_.reset();
    watcher_.reset();
    readiness_.reset();
    ipc_.reset();
    mux_.reset();
    scratch().probes.clear();
    scratch().forced.clear();
    panelConnections_ = 0;

    // Persist jobs and settings synchronously on the way out.
    saveNow();
    started_ = false;
    HH_LOG_INFO(kLog, L"engine stopped");
}

/**
 * @brief Drains the event queue and applies every event on the UI thread.
 *
 * Notifications are coalesced: the apply() functions call notifyJobs() /
 * notifyLink() freely and the callbacks fire once at the end of the drain.
 */
void Engine::onEventMessage() {
    UiThreadScratch& s = scratch();
    ++s.drainDepth;

    // Drain in rounds: a worker may post while a batch is being applied and
    // the queue's kick flag is already clear, so keep going until empty.
    for (int round = 0; round < kMaxDrainRounds; ++round) {
        std::deque<EngineEvent> batch = queue_.drain();
        if (batch.empty()) {
            break;
        }
        for (EngineEvent& ev : batch) {
            std::visit([this](auto& e) { apply(e); }, ev);
        }
    }

    --s.drainDepth;
    if (s.drainDepth > 0) {
        return;   // nested drain (stop() during apply); the outer one notifies
    }
    // Fire the coalesced callbacks now that the batch is fully applied.
    if (s.jobsChanged) {
        s.jobsChanged = false;
        if (onJobsChanged) {
            onJobsChanged();
        }
    }
    if (s.linkChanged) {
        s.linkChanged = false;
        if (onLinkStateChanged) {
            onLinkStateChanged();
        }
    }
}

/**
 * @brief ~1 Hz housekeeping: debounced jobs.json save and history trimming.
 */
void Engine::tick() {
    if (!started_) {
        return;
    }
    // Trim only when the table actually exceeds the cap (cheap check first).
    const size_t historyMax = static_cast<size_t>(std::max(0, settings_.historyMax));
    if (historyMax > 0 && store_.size() > historyMax) {
        store_.trim(historyMax);
        notifyJobs();
        // A trim removed rows; make sure the debounced save below runs soon.
        store_.touch();
    }
    // Debounced save: at most one write per kSaveDebounceMs while dirty.
    if (store_.dirty()) {
        const uint64_t now = platform::nowMonotonicMs();
        if (now - lastSaveMs_ >= kSaveDebounceMs) {
            if (auto r = store_.save(jobsPath_); !r) {
                HH_LOG_WARN(kLog, L"jobs save failed: {}", r.error().toString());
            }
            lastSaveMs_ = now;
        }
    }
}

// ===========================================================================
// 3. Queries
// ===========================================================================

/// Snapshot of every job, newest first.
std::vector<Job> Engine::jobs() const {
    std::vector<Job> out;
    const auto all = store_.all();
    out.reserve(all.size());
    for (const Job* j : all) {
        if (j) {
            out.push_back(*j);
        }
    }
    return out;
}

/// Copy of one job, if it exists.
std::optional<Job> Engine::job(JobId id) const {
    const Job* j = store_.find(id);
    if (!j) {
        return std::nullopt;
    }
    return *j;
}

/// The footer / status-dot state with the derived counters filled in.
LinkState Engine::linkState() const {
    LinkState s = link_;
    int active = 0;
    for (const Job* j : store_.all()) {
        if (j && j->isActive()) {
            ++active;
        }
    }
    s.activeJobs = active;
    s.watchFolders = watcher_ ? static_cast<int>(watcher_->folders().size()) : 0;
    s.mkvmergeVersion = mkvmerge_.shortVersion();
    s.mkvmergeOk = mkvmerge_.ok;
    s.panelConnected = panelConnections_ > 0;
    return s;
}

/// .cube files known to the app (folder listing + recents), refreshed by refreshLuts().
std::vector<std::wstring> Engine::availableLuts() const {
    return luts_;
}

/// True while mkvmerge is running for some job.
bool Engine::muxRunning() const {
    return mux_ && mux_->busy();
}

/// The folders the watcher currently follows.
std::vector<std::wstring> Engine::watchFolders() const {
    if (!watcher_) {
        return {};
    }
    return watcher_->folders();
}

// ===========================================================================
// 4. AME log events
// ===========================================================================

/**
 * @brief Finds the active job for an output path or creates a fresh one.
 *
 * The key is the normalised path; a terminal job with the same key does not
 * count as active, so a re-export of the same name gets a new generation.
 */
Job& Engine::ensureJob(const std::wstring& outputPath, JobSource source, bool& created) {
    created = false;
    // Resolve to an absolute path once; the key is derived from that.
    std::wstring full = platform::fullPath(outputPath);
    if (full.empty()) {
        full = path::normalizeSeparators(outputPath);
    }
    const std::wstring key = path::normalizeKey(full);
    if (Job* existing = store_.findActiveByKey(key)) {
        return *existing;
    }

    // No live job: create one in Discovered and fill the basics.
    Job& job = store_.create(key, full, source);
    created = true;
    const uint64_t now = platform::nowUtc();
    if (job.createdUtc == 0) {
        job.createdUtc = now;
    }
    job.updatedUtc = now;
    job.state = JobState::Discovered;
    job.source = source;
    if (job.outputPath.empty()) {
        job.outputPath = full;
    }
    if (job.key.empty()) {
        job.key = key;
    }
    // The hint path preview lets the UI show where the result will land.
    job.hintPath = resolvePlan(job).hintPath;
    store_.touch();
    HH_LOG_INFO(kLog, L"job {} created (gen {}) for {} via {}", job.id, job.generation, full, toString(source));
    notifyJobs();
    return job;
}

/**
 * @brief Seed-mode catch-up: a recent successful export the app missed.
 *
 * Never auto-muxed: the job is created Held so the user decides.
 */
void Engine::catchUp(const AmeItemRecord& rec) {
    if (rec.result != AmeItemRecord::Result::Success || rec.outputPath.empty()) {
        return;
    }
    // A zero window disables catch-up entirely.
    const uint64_t window = minutesToTicks(settings_.catchUpMinutes);
    if (window == 0 || rec.statusUtc == 0) {
        return;
    }
    const uint64_t now = platform::nowUtc();
    if (rec.statusUtc + window < now) {
        return;   // older than the window: history only
    }
    // The output must still exist and must not have been processed already.
    if (!platform::isFile(rec.outputPath)) {
        return;
    }
    const std::wstring hint = path::hintPathFor(rec.outputPath, settings_.suffix, settings_.expand(settings_.outputFolder));
    if (!hint.empty() && platform::exists(hint)) {
        HH_LOG_DEBUG(kLog, L"catch-up skipped, hint exists: {}", hint);
        return;
    }
    // Any job that already knows this export (active, or finished after the
    // log line was written) means there is nothing to catch up.
    const std::wstring key = path::normalizeKey(platform::fullPath(rec.outputPath));
    if (const Job* known = store_.findByKey(key)) {
        if (known->isActive() || known->updatedUtc >= rec.statusUtc) {
            return;
        }
        // Same physical file as a finished job: also nothing to do.
        if (known->sourceStamp.valid) {
            if (auto ident = platform::identity(rec.outputPath); ident && stampFrom(ident.value()).sameFileAs(known->sourceStamp)) {
                return;
            }
        }
    }

    bool created = false;
    Job& job = ensureJob(rec.outputPath, JobSource::CatchUp, created);
    if (!created) {
        return;   // a live job appeared meanwhile
    }
    // Enrich from the log block and park it for the user.
    job.presetName = rec.presetName;
    job.video = rec.video;
    job.sourcePath = rec.sourcePath;
    job.logConfirmed = true;
    inferTransfer(job, rec.video.transfer, L"log");
    if (job.encodeStartUtc == 0) {
        if (auto ms = platform::parseHms(rec.encodingTime); ms && rec.statusUtc > *ms * kTicksPerMs) {
            job.encodeStartUtc = rec.statusUtc - *ms * kTicksPerMs;
        }
    }
    transition(job, JobState::Held, kReasonCatchUpLog);
    registerFolder(path::parent(job.outputPath));
}

/**
 * @brief A block finished in AMEEncodingLog.txt (or the error log).
 */
void Engine::apply(LogItemEvent& e) {
    const AmeItemRecord& rec = e.record;
    if (rec.outputPath.empty()) {
        HH_LOG_DEBUG(kLog, L"log item without output path ignored ({})", rec.statusText);
        return;
    }

    // The error log only ever enriches a failure reason we already have.
    if (e.fromErrorLog) {
        const std::wstring key = path::normalizeKey(platform::fullPath(rec.outputPath));
        if (Job* job = store_.findByKey(key)) {
            if (job->state == JobState::Failed && job->stateReason.empty() && !rec.failureReason.empty()) {
                job->stateReason = rec.failureReason;
                job->updatedUtc = platform::nowUtc();
                store_.touch();
                notifyJobs();
            }
        }
        return;
    }

    // History: only the catch-up rule may create anything.
    if (e.seed) {
        catchUp(rec);
        return;
    }

    switch (rec.result) {
    case AmeItemRecord::Result::Success: {
        // A finished job for the very same file (probe path completed before
        // the log block landed) must not spawn a second generation.
        const std::wstring key = path::normalizeKey(platform::fullPath(rec.outputPath));
        if (!store_.findActiveByKey(key)) {
            if (const Job* done = store_.findByKey(key); done && done->sourceStamp.valid) {
                if (auto ident = platform::identity(rec.outputPath); ident && stampFrom(ident.value()).sameFileAs(done->sourceStamp)) {
                    HH_LOG_DEBUG(kLog, L"log success for already finished job {} ignored", done->id);
                    return;
                }
            }
        }

        bool created = false;
        Job& job = ensureJob(rec.outputPath, JobSource::Log, created);
        job.presetName = rec.presetName;
        if (!rec.video.raw.empty() || job.video.width == 0) {
            job.video = rec.video;
        }
        if (job.sourcePath.empty()) {
            job.sourcePath = rec.sourcePath;
        }
        job.logConfirmed = true;
        inferTransfer(job, rec.video.transfer, L"log");
        // The log block carries the encode duration: derive the start time.
        if (job.encodeStartUtc == 0 && rec.statusUtc != 0) {
            if (auto ms = platform::parseHms(rec.encodingTime); ms && rec.statusUtc > *ms * kTicksPerMs) {
                job.encodeStartUtc = rec.statusUtc - *ms * kTicksPerMs;
            }
        }
        job.updatedUtc = platform::nowUtc();
        store_.touch();
        registerFolder(path::parent(job.outputPath));

        // The file is complete from AME's point of view: let the probe know.
        if (job.state == JobState::Discovered || job.state == JobState::Encoding) {
            if (!probeTracked(job.id)) {
                scheduleProbe(readiness_.get(), job, false);
            }
            if (readiness_) {
                readiness_->confirm(job.id);
            }
        }
        notifyJobs();
        break;
    }
    case AmeItemRecord::Result::Failed: {
        const std::wstring key = path::normalizeKey(platform::fullPath(rec.outputPath));
        Job* job = store_.findActiveByKey(key);
        if (!job) {
            HH_LOG_INFO(kLog, L"AME reported a failure for an untracked export: {}", rec.outputPath);
            return;
        }
        std::wstring reason = !rec.failureReason.empty() ? rec.failureReason
                            : (!rec.statusText.empty() ? rec.statusText : std::wstring(L"AME reported a failure"));
        if (job->state == JobState::Muxing || job->state == JobState::Verifying) {
            addNote(*job, L"AME reported a failure after the mux started: " + reason);
            store_.touch();
            notifyJobs();
            return;
        }
        // The probe owns the transition when it is live; otherwise fail now.
        if (probeTracked(job->id) && readiness_) {
            readiness_->fail(job->id, reason);
        } else {
            transition(*job, JobState::Failed, reason);
        }
        break;
    }
    case AmeItemRecord::Result::Incomplete:
    case AmeItemRecord::Result::Unknown:
    default:
        HH_LOG_DEBUG(kLog, L"log item with result {} ignored for {}", static_cast<int>(rec.result), rec.outputPath);
        break;
    }
}

/**
 * @brief Queue Started / Stopped / Paused. Paused still counts as running.
 */
void Engine::apply(LogQueueEvent& e) {
    bool running = link_.queueRunning;
    switch (e.event.kind) {
    case AmeQueueEvent::Kind::Started: running = true; break;
    case AmeQueueEvent::Kind::Stopped: running = false; break;
    case AmeQueueEvent::Kind::Paused: running = true; break;
    default: break;
    }
    if (running != link_.queueRunning) {
        link_.queueRunning = running;
        // Historical queue flips are replayed by the hundreds on start; keep them out of the info log.
        if (e.seed) {
            HH_LOG_DEBUG(kLog, L"AME queue {} (from history)", running ? L"running" : L"stopped");
        } else {
            HH_LOG_INFO(kLog, L"AME queue {}", running ? L"running" : L"stopped");
        }
        notifyLink();
    }
}

/**
 * @brief Output folders learned from the log history become watched folders.
 */
void Engine::apply(LogFoldersEvent& e) {
    bool changed = false;
    for (const auto& raw : e.folders) {
        const std::wstring folder = normalizeFolder(raw);
        if (folder.empty() || isIgnoredFolder(settings_, folder)) {
            continue;
        }
        if (containsFolder(knownFolders_, folder)) {
            continue;
        }
        // History can name folders that no longer exist (old drives, renamed
        // projects); watching those only produces noise.
        if (!platform::isDirectory(folder)) {
            HH_LOG_DEBUG(kLog, L"learned folder skipped (missing): {}", folder);
            continue;
        }
        knownFolders_.push_back(folder);
        changed = true;
    }
    if (!changed) {
        return;
    }
    // Replace the whole watch set: extras first, then the learned folders.
    std::vector<std::wstring> folders;
    for (const auto& raw : settings_.extraWatchFolders) {
        const std::wstring norm = normalizeFolder(settings_.expand(raw));
        if (!norm.empty() && !containsFolder(folders, norm)) {
            folders.push_back(norm);
        }
    }
    for (const auto& known : knownFolders_) {
        if (!containsFolder(folders, known)) {
            folders.push_back(known);
        }
    }
    if (watcher_) {
        watcher_->setFolders(folders);
    }
    HH_LOG_INFO(kLog, L"watching {} folders ({} learned from the AME log)", folders.size(), knownFolders_.size());
    notifyLink();
}

/**
 * @brief The tailer found or lost the AME log.
 */
void Engine::apply(LogStatusEvent& e) {
    const bool found = !e.primaryLogPath.empty();
    if (found != link_.logFound || e.primaryLogPath != link_.logPath) {
        link_.logFound = found;
        link_.logPath = e.primaryLogPath;
        if (found) {
            HH_LOG_INFO(kLog, L"AME log: {} ({} candidates)", e.primaryLogPath, e.candidateCount);
        } else {
            HH_LOG_WARN(kLog, L"AME log not found ({} candidates)", e.candidateCount);
        }
        notifyLink();
    }
}

// ===========================================================================
// 5. Folder watcher events
// ===========================================================================

/**
 * @brief An AME sidecar (<stem>.<pid>.<tid>.m4v / .aac) appeared, grew or vanished.
 *
 * Sidecars are the earliest sign of an export; the final container only
 * appears when the encode finishes, so the job is created Encoding with a
 * guessed ".mp4" output that OutputEvent upgrades later.
 */
void Engine::apply(SidecarEvent& e) {
    if (e.folder.empty() || e.stem.empty()) {
        return;
    }
    Job* job = store_.findActiveByStem(e.folder, e.stem);

    if (e.gone) {
        // Sidecars disappear when AME finished assembling the container.
        if (job && job->state == JobState::Encoding) {
            job->phase = L"Finalizing";
            job->updatedUtc = platform::nowUtc();
            store_.touch();
            notifyJobs();
        }
        return;
    }

    if (!job) {
        // Guess the container name; the real one replaces it on OutputEvent.
        const std::wstring guessed = path::join(e.folder, e.stem + L".mp4");
        bool created = false;
        job = &ensureJob(guessed, JobSource::Folder, created);
        if (created) {
            addNote(*job, L"Detected from AME sidecar files");
        }
    }
    // Anything Discovered/new moves to Encoding; other states just track bytes.
    if (job->state == JobState::Discovered) {
        job->encodeStartUtc = job->encodeStartUtc ? job->encodeStartUtc : platform::nowUtc();
        transition(*job, JobState::Encoding);
    }
    if (job->state == JobState::Encoding) {
        if (job->phase.empty() || job->phase == L"Finalizing") {
            job->phase = L"Encoding in AME";
        }
        if (job->encodeStartUtc == 0) {
            job->encodeStartUtc = platform::nowUtc();
        }
    }
    // Track the largest sidecar total seen; it is the finalize denominator.
    if (e.size > job->sidecarBytes) {
        job->sidecarBytes = e.size;
        if (readiness_) {
            readiness_->setSidecarBytes(job->id, job->sidecarBytes);
        }
    }
    job->updatedUtc = platform::nowUtc();
    store_.touch();
    notifyJobs();
}

/**
 * @brief A candidate output changed in a watched folder.
 */
void Engine::apply(OutputEvent& e) {
    if (e.path.empty()) {
        return;
    }
    // Our own results and partials never become jobs, whatever the watcher says.
    const std::wstring name = path::fileName(e.path);
    if (path::isOurOutput(name, settings_.suffix) || path::isPartialOutput(name)) {
        return;
    }
    const std::wstring full = platform::fullPath(e.path).empty() ? e.path : platform::fullPath(e.path);
    const std::wstring key = path::normalizeKey(full);

    // ---- Removed ---------------------------------------------------------------
    if (e.action == OutputEvent::Action::Removed) {
        Job* job = store_.findActiveByKey(key);
        if (!job) {
            return;
        }
        switch (job->state) {
        case JobState::Discovered:
        case JobState::Encoding:
            // AME may delete and recreate during a restart; the probe's
            // missing grace decides whether this is fatal.
            addNote(*job, L"Output file removed at " + platform::formatFriendly(platform::nowUtc()));
            if (!probeTracked(job->id)) {
                transition(*job, JobState::Failed, kReasonOutputRemoved);
            } else {
                store_.touch();
                notifyJobs();
            }
            break;
        case JobState::Ready:
        case JobState::Held:
            cancelProbe(readiness_.get(), job->id);
            transition(*job, JobState::Failed, kReasonSourceGone);
            break;
        case JobState::Muxing:
        case JobState::Verifying:
            addNote(*job, L"Source file removed while muxing");
            store_.touch();
            notifyJobs();
            break;
        default:
            break;
        }
        return;
    }

    // ---- Added / Modified -----------------------------------------------------
    Job* job = store_.findActiveByKey(key);
    if (!job) {
        // A sidecar-created job guessed ".mp4"; if the real container has a
        // different extension, upgrade that job instead of creating another.
        const std::wstring folder = path::parent(full);
        const std::wstring stem = path::stem(full);
        if (Job* stemJob = store_.findActiveByStem(folder, stem)) {
            const bool guessed = stemJob->state == JobState::Encoding && !iequals(stemJob->key, key)
                              && !platform::exists(stemJob->outputPath);
            if (guessed) {
                HH_LOG_INFO(kLog, L"job {} output upgraded {} -> {}", stemJob->id, stemJob->outputPath, full);
                stemJob->outputPath = full;
                stemJob->key = key;
                stemJob->hintPath = resolvePlan(*stemJob).hintPath;
                job = stemJob;
            }
        }
    }

    if (!job) {
        // A finished job for the very same physical file: nothing new happened
        // (Explorer touching attributes, an initial scan after a restart...).
        if (const Job* done = store_.findByKey(key); done && done->sourceStamp.valid) {
            if (auto ident = platform::identity(full); ident && stampFrom(ident.value()).sameFileAs(done->sourceStamp)
                && ident.value().size == done->sourceStamp.size) {
                HH_LOG_DEBUG(kLog, L"output event for finished job {} ignored", done->id);
                return;
            }
        }
        // A terminal job (Done/Failed/Skipped/Cancelled) is only superseded by a
        // genuinely new export, which always begins with the file being (re)created.
        // A bare "modified" notification for the same path (AME touching the
        // failed file, Explorer, antivirus...) must not spawn a new generation.
        if (e.action == OutputEvent::Action::Modified && !e.fromInitialScan) {
            if (const Job* previous = store_.findByKey(key); previous && previous->isTerminal()) {
                HH_LOG_DEBUG(kLog, L"modified event for terminal job {} ignored", previous->id);
                return;
            }
        }
        // Initial-scan files that already have a hint next to them are done.
        if (e.fromInitialScan) {
            const std::wstring hint = path::hintPathFor(full, settings_.suffix, settings_.expand(settings_.outputFolder));
            if (!hint.empty() && platform::exists(hint)) {
                HH_LOG_DEBUG(kLog, L"initial scan: {} already has {}", full, path::fileName(hint));
                return;
            }
        }

        bool created = false;
        job = &ensureJob(full, JobSource::Folder, created);
        if (created && e.fromInitialScan) {
            // A complete, unlocked file from before we started is a catch-up
            // candidate: park it for the user. A locked one is being written
            // right now and goes through the normal probe.
            if (platform::probeDenyWrite(full) == platform::OpenProbe::Ready) {
                transition(*job, JobState::Held, kReasonCatchUpFolder);
                return;
            }
        }
    }

    // Live jobs waiting for their file get a probe exactly once.
    if (job->state == JobState::Discovered || job->state == JobState::Encoding) {
        if (!probeTracked(job->id)) {
            const bool confirm = settings_.requireLogConfirmation && link_.logFound && !job->logConfirmed && !job->cepConfirmed;
            scheduleProbe(readiness_.get(), *job, confirm);
        }
    }
    // While AME assembles the container the size grows towards the sidecar total.
    if (job->state == JobState::Encoding && e.size > 0 && job->sidecarBytes > 0) {
        job->phase = L"Finalizing";
        const double ratio = static_cast<double>(e.size) / static_cast<double>(job->sidecarBytes);
        job->progress = static_cast<float>(std::clamp(ratio, 0.0, 1.0));
        job->updatedUtc = platform::nowUtc();
        store_.touch();
        notifyJobs();
    }
}

/**
 * @brief A watched folder became reachable or dropped off (drive unplugged).
 */
void Engine::apply(FolderAvailabilityEvent& e) {
    if (e.available) {
        HH_LOG_INFO(kLog, L"watch folder available: {}", e.folder);
    } else {
        HH_LOG_WARN(kLog, L"watch folder unavailable: {} ({})", e.folder, e.message);
        // Only folders the user configured deserve a toast; learned ones just log.
        bool configured = false;
        for (const auto& raw : settings_.extraWatchFolders) {
            if (iequals(normalizeFolder(settings_.expand(raw)), normalizeFolder(e.folder))) {
                configured = true;
                break;
            }
        }
        if (configured) {
            toast(ToastRequest::Tone::Warning, L"Cannot watch " + path::ellipsizeMiddle(e.folder, 48)
                  + (e.message.empty() ? std::wstring() : L": " + e.message));
        }
    }
    notifyLink();
}

// ===========================================================================
// 6. Readiness probe events
// ===========================================================================

/**
 * @brief The prober reported on a job's output file.
 */
void Engine::apply(ProbeEvent& e) {
    Job* job = store_.find(e.jobId);
    if (!job) {
        untrackProbe(e.jobId);
        HH_LOG_DEBUG(kLog, L"probe event for unknown job {} ignored", e.jobId);
        return;
    }

    switch (e.outcome) {
    case ProbeEvent::Outcome::Writing: {
        // Somebody (AME) still holds the file: the job is being encoded.
        if (job->state == JobState::Discovered) {
            if (job->encodeStartUtc == 0) {
                job->encodeStartUtc = platform::nowUtc();
            }
            transition(*job, JobState::Encoding);
        }
        if (job->state == JobState::Encoding) {
            job->phase = e.phase.empty() ? std::wstring(L"Encoding in AME") : e.phase;
            job->progress = e.progress;
            job->updatedUtc = platform::nowUtc();
            store_.touch();
            notifyJobs();
        }
        break;
    }
    case ProbeEvent::Outcome::Missing: {
        untrackProbe(job->id);
        if (job->isActive() && job->state != JobState::Muxing && job->state != JobState::Verifying) {
            transition(*job, JobState::Failed, e.message.empty() ? std::wstring(kReasonOutputRemoved) : e.message);
        }
        break;
    }
    case ProbeEvent::Outcome::Failed: {
        untrackProbe(job->id);
        if (job->isActive() && job->state != JobState::Muxing && job->state != JobState::Verifying) {
            transition(*job, JobState::Failed, e.message.empty() ? std::wstring(L"Readiness check failed") : e.message);
        }
        break;
    }
    case ProbeEvent::Outcome::Ready: {
        untrackProbe(job->id);
        // A stale Ready for a job that already moved on is ignored.
        if (job->state == JobState::Muxing || job->state == JobState::Verifying || job->state == JobState::Done) {
            HH_LOG_DEBUG(kLog, L"stale Ready for job {} in state {}", job->id, toString(job->state));
            break;
        }
        job->sourceStamp = e.stamp;
        if (!job->sourceStamp.valid) {
            // The prober could not read an identity; capture one ourselves.
            if (auto ident = platform::identity(job->outputPath)) {
                job->sourceStamp = stampFrom(ident.value());
            }
        }
        job->readyUtc = platform::nowUtc();
        if (e.mp4Complete && e.durationSec > 0.0 && job->video.fps <= 0.0) {
            addNote(*job, std::format(L"Duration {} (from moov)", platform::formatDuration(static_cast<uint64_t>(e.durationSec * 1000.0))));
        }
        onReady(*job);
        break;
    }
    default:
        break;
    }
}

// ===========================================================================
// 7. Plan resolution + dispatch
// ===========================================================================

/**
 * @brief Records what we learned about the export's colour transfer.
 *
 * Probe / identify / user values are authoritative and may correct an
 * earlier guess; log / cep values only fill in an unknown.
 */
void Engine::inferTransfer(Job& job, TransferKind kind, const wchar_t* source) {
    if (kind == TransferKind::Unknown) {
        return;
    }
    const std::wstring src = source ? source : L"";
    const bool authoritative = iequals(src, L"probe") || iequals(src, L"identify") || iequals(src, L"user");

    if (job.transfer == TransferKind::Unknown) {
        job.transfer = kind;
        job.transferSource = src;
        if (job.video.transfer == TransferKind::Unknown) {
            job.video.transfer = kind;
        }
        HH_LOG_INFO(kLog, L"job {} transfer {} (from {})", job.id, toString(kind), src);
        store_.touch();
        return;
    }
    if (job.transfer == kind) {
        return;
    }
    // Disagreement: authoritative sources win, others are noted and ignored.
    if (authoritative) {
        addNote(job, std::format(L"Colour space corrected: {} said {}, {} says {}",
                                 job.transferSource, toString(job.transfer), src, toString(kind)));
        HH_LOG_WARN(kLog, L"job {} transfer corrected {} -> {} ({})", job.id, toString(job.transfer), toString(kind), src);
        job.transfer = kind;
        job.transferSource = src;
        job.video.transfer = kind;
    } else {
        addNote(job, std::format(L"{} reports {} but {} was detected by {} - keeping {}",
                                 src, toString(kind), toString(job.transfer), job.transferSource, toString(job.transfer)));
        HH_LOG_WARN(kLog, L"job {} transfer mismatch: {} says {}, keeping {} from {}",
                    job.id, src, toString(kind), toString(job.transfer), job.transferSource);
    }
    store_.touch();
}

/**
 * @brief Resolves defaults + overrides into the plan for a job (no dispatch).
 *
 * Pure with respect to job state; it does touch the disk for the cheap
 * "does the LUT exist" check so the UI preview can show the same error the
 * dispatcher would.
 */
EffectivePlan Engine::resolvePlan(const Job& job) const {
    EffectivePlan p;
    const TransferKind kind = planKindFor(job.transfer);

    // Preset: override, else the default for the transfer.
    p.presetId = job.overrides.presetId ? *job.overrides.presetId : settings_.defaultPresetFor(static_cast<int>(kind));
    // LUT: override (empty string = none), else the default for the transfer.
    p.lutPath = job.overrides.lutPath ? *job.overrides.lutPath : settings_.defaultLutFor(static_cast<int>(kind));
    if (!p.lutPath.empty()) {
        p.lutPath = settings_.expand(p.lutPath);
    }
    p.attachLut = job.overrides.attachLut ? *job.overrides.attachLut : (settings_.attachLut && !p.lutPath.empty());
    if (p.attachLut && p.lutPath.empty()) {
        p.attachLut = false;
    }
    p.suffix = job.overrides.suffix ? *job.overrides.suffix : settings_.suffix;
    p.hintPath = job.outputPath.empty() ? std::wstring()
               : path::hintPathFor(job.outputPath, p.suffix, settings_.expand(settings_.outputFolder));

    // Validation, first problem wins (the UI shows one fix action at a time).
    if (job.outputPath.empty()) {
        p.error = L"No output path";
        return p;
    }
    if (p.presetId.empty()) {
        p.error = L"No preset selected";
        return p;
    }
    const HdrPreset* preset = presets_.find(p.presetId);
    if (!preset) {
        p.error = L"Preset not found: " + p.presetId;
        return p;
    }
    if (preset->isSentinel()) {
        p.error = L"Preset '" + preset->label + L"' cannot be used directly - pick a concrete preset";
        return p;
    }
    if (p.attachLut && !platform::isFile(p.lutPath)) {
        p.error = L"LUT file not found: " + p.lutPath;
        return p;
    }
    if (!mkvmerge_.ok) {
        p.error = mkvmerge_.error.empty() ? std::wstring(L"mkvmerge not found") : mkvmerge_.error;
        return p;
    }
    if (path::hasInvalidFileNameChars(p.suffix)) {
        p.error = L"Suffix contains characters not allowed in file names";
        return p;
    }
    if (p.hintPath.empty()) {
        p.error = L"Could not derive the hint file name";
        return p;
    }
    if (p.hintPath.size() > kMaxHintPathChars) {
        p.error = L"Hint path too long for mkvmerge";
        return p;
    }
    return p;
}

/**
 * @brief Disk-level checks just before dispatch (source present, output
 *        folder creatable, conflict policy). Fills plan.error on failure.
 */
Result<void> Engine::validatePlan(const Job& job, EffectivePlan& plan) const {
    if (!plan.valid()) {
        return Error::text(plan.error);
    }
    if (!platform::isFile(job.outputPath)) {
        plan.error = kReasonSourceMissing;
        return Error::text(plan.error);
    }
    // The hint must never be the source itself (a .mkv source with an empty suffix).
    if (iequals(plan.hintPath, job.outputPath)) {
        plan.error = L"Hint file name equals the source file name";
        return Error::text(plan.error);
    }
    // A configured output folder is created on demand.
    const std::wstring hintFolder = path::parent(plan.hintPath);
    if (!hintFolder.empty() && !platform::isDirectory(hintFolder)) {
        if (auto r = platform::createDirectories(hintFolder); !r) {
            plan.error = L"Cannot create output folder: " + r.error().toString();
            return Error::text(plan.error);
        }
    }
    // "skip" on conflict is decided before mkvmerge burns minutes on the mux.
    if (iequals(settings_.onConflict, L"skip") && platform::exists(plan.hintPath)) {
        plan.error = L"Hint file already exists: " + path::fileName(plan.hintPath);
        return Error::text(plan.error);
    }
    return Result<void>::success();
}

/**
 * @brief The job's file is complete and unlocked: apply the policies and
 *        either dispatch, skip or park it.
 */
void Engine::onReady(Job& job) {
    // A forced run (user clicked Run) skips the hold / auto / SDR policies once.
    const bool forced = scratch().forced.erase(job.id) > 0;
    transition(job, JobState::Ready);

    // Last chance to learn the colour space before the policy decisions: a
    // short synchronous ffprobe (header + first frame side data only).
    if (job.transfer == TransferKind::Unknown && !ffprobePath_.empty() && !job.overrides.presetId) {
        const uint64_t t0 = platform::nowMonotonicMs();
        if (auto info = probeMedia(ffprobePath_, job.outputPath, kReadyProbeTimeoutMs)) {
            inferTransfer(job, info->transfer, L"probe");
            if (info->hasMasteringDisplay || info->hasContentLightLevel) {
                job.inbandHdr10 = true;
            }
            if (job.video.width == 0 && info->width > 0) {
                job.video.width = info->width;
                job.video.height = info->height;
                job.video.fps = info->fps;
                job.video.codecHint = info->codec;
            }
        }
        HH_LOG_DEBUG(kLog, L"ffprobe for job {} took {} ms", job.id, platform::nowMonotonicMs() - t0);
    }

    const bool userPreset = job.overrides.presetId.has_value();

    // SDR policy (unless the user picked a preset explicitly).
    if (job.transfer == TransferKind::SDR && !userPreset && !forced) {
        if (iequals(settings_.sdrPolicy, L"skip")) {
            transition(job, JobState::SkippedSdr, kReasonSdrSkip);
            return;
        }
        if (iequals(settings_.sdrPolicy, L"hold")) {
            transition(job, JobState::Held, kReasonSdrHold);
            return;
        }
        // "tag_sdr": fall through and mux with the SDR preset.
    }
    // Unknown transfer: never guess silently when the settings say to hold.
    if (job.transfer == TransferKind::Unknown && settings_.holdWhenTransferUnknown && !userPreset) {
        transition(job, JobState::Held, kReasonUnknownTransfer);
        return;
    }
    // In-band HDR10 metadata with the "hold" policy needs a human decision.
    if (job.inbandHdr10 && iequals(settings_.inbandPolicy, L"hold") && !userPreset && !forced) {
        transition(job, JobState::Held, kReasonInbandHold);
        return;
    }
    // User hold / manual mode. Auto-processing is off globally, or off for
    // the trigger that found this file: a job is only ever created once (the
    // store dedupes every source onto one normalised path), so the source
    // recorded on the job is the one that discovered it first.
    if (!forced && (job.overrides.hold || !settings_.autoProcess || !autoProcessAllowed(job))) {
        transition(job, JobState::Held, kReasonWaiting);
        return;
    }
    dispatch(job);
}

/**
 * @brief Whether the trigger that found this job may process it unattended.
 *
 * Two independent switches, so a user who only wants folder watching can turn
 * the Media Encoder side off and vice versa. A file is only ever one job (the
 * store dedupes every trigger onto one normalised output path), so the source
 * stored on the job is whichever trigger saw it first and there is no risk of
 * a file being processed twice because it matched both.
 *
 * Manual, catch-up and re-run jobs are the user asking directly: those are
 * never gated here.
 */
bool Engine::autoProcessAllowed(const Job& job) const {
    switch (job.source) {
    case JobSource::Log:
    case JobSource::Cep:
        return settings_.autoProcessAme;
    case JobSource::Folder:
        return settings_.autoProcessWatched;
    case JobSource::Manual:
    case JobSource::CatchUp:
    default:
        return true;
    }
}

/**
 * @brief Resolves and validates the plan, then hands the job to the mux worker
 *        (or parks it Held with the plan error).
 */
void Engine::dispatch(Job& job) {
    EffectivePlan plan = resolvePlan(job);
    validatePlan(job, plan);
    job.plan = plan;
    if (!plan.valid()) {
        transition(job, JobState::Held, plan.error);
        return;
    }
    if (!mux_) {
        transition(job, JobState::Held, kReasonNoMuxWorker);
        return;
    }

    // Capture the plan on the job for reproducibility, then queue it.
    job.hintPath = plan.hintPath;
    job.muxStartUtc = platform::nowUtc();
    job.progress = 0.0f;
    job.phase = L"Queued for mkvmerge";
    transition(job, JobState::Muxing);

    MuxRequest req;
    req.jobId = job.id;
    req.plan = makeMuxPlan(job, plan);
    req.ffprobePath = ffprobePath_;
    // The worker probes the input when we still do not know the transfer or
    // when the in-band policy needs the SEI facts.
    req.probeInput = job.transfer == TransferKind::Unknown || !iequals(settings_.inbandPolicy, L"preset");
    mux_->enqueue(std::move(req));
    HH_LOG_INFO(kLog, L"job {} dispatched: {} -> {} (preset {}, lut {})",
                job.id, job.outputPath, plan.hintPath, plan.presetId, plan.attachLut ? plan.lutPath : L"none");
}

/**
 * @brief Everything the mux worker needs, copied out of the job and settings.
 */
MuxPlan Engine::makeMuxPlan(const Job& job, const EffectivePlan& plan) const {
    MuxPlan m;
    m.mkvmergePath = mkvmerge_.path;
    m.supportsUiLanguage = mkvmerge_.supportsUiLanguage;
    m.inputPath = job.outputPath;
    m.hintPath = plan.hintPath;
    m.partialPath = path::partialPathFor(plan.hintPath);
    // The preset was validated by resolvePlan; fall back to an empty preset
    // (no colour flags) rather than dereferencing null if it vanished.
    if (const HdrPreset* preset = presets_.find(plan.presetId)) {
        m.preset = *preset;
    } else {
        HH_LOG_ERROR(kLog, L"makeMuxPlan: preset {} vanished; muxing without colour flags", plan.presetId);
        m.preset = HdrPreset{};
        m.preset.id = plan.presetId;
    }
    m.lutPath = plan.attachLut ? plan.lutPath : std::wstring();
    m.attachmentMime = settings_.attachmentMime.empty() ? std::wstring(L"application/x-cube") : settings_.attachmentMime;
    m.attachLut = plan.attachLut;
    m.trackId = 0;   // the worker replaces this with the identified video track
    m.lowerPriority = settings_.lowerPriority;
    m.failOnWarnings = settings_.failOnWarnings;
    m.setTitle = settings_.setTitle;
    m.title = path::stem(job.outputPath);
    m.onConflict = settings_.onConflict.empty() ? std::wstring(L"increment") : settings_.onConflict;
    m.keepPartialOnFailure = settings_.keepPartialOnFailure;
    const uint64_t hours = static_cast<uint64_t>(std::max(1, settings_.timeoutHours));
    const uint64_t ms = std::min<uint64_t>(hours * 3600ull * 1000ull, 0xFFFFFFFEull);
    m.timeoutMs = static_cast<DWORD>(ms);
    return m;
}

// ===========================================================================
// 8. Mux events, completion, Recycle Bin
// ===========================================================================

/**
 * @brief Progress / completion from the mux worker.
 */
void Engine::apply(MuxEvent& e) {
    Job* job = store_.find(e.jobId);
    if (!job) {
        HH_LOG_DEBUG(kLog, L"mux event for unknown job {} ignored", e.jobId);
        return;
    }
    // Facts the worker learned about the input apply regardless of outcome.
    if (e.probedTransfer != TransferKind::Unknown) {
        inferTransfer(*job, e.probedTransfer, L"probe");
    }
    if (e.inbandHdr10 && !job->inbandHdr10) {
        job->inbandHdr10 = true;
        addNote(*job, L"Source carries in-band HDR10 metadata (SEI); container tags follow the preset");
        store_.touch();
    }

    const bool inMux = job->state == JobState::Muxing || job->state == JobState::Verifying;
    switch (e.kind) {
    case MuxEvent::Kind::Started:
        if (inMux) {
            job->phase = e.phase.empty() ? std::wstring(L"Identifying") : e.phase;
            job->progress = 0.0f;
            job->updatedUtc = platform::nowUtc();
            store_.touch();
            notifyJobs();
        }
        break;
    case MuxEvent::Kind::Progress:
        if (inMux) {
            if (!e.phase.empty()) {
                job->phase = e.phase;
            }
            job->progress = std::clamp(e.progress, 0.0f, 1.0f);
            // The verification phase has its own state for the UI.
            if (job->state == JobState::Muxing && iequals(e.phase, L"Verifying")) {
                const std::wstring keepPhase = job->phase;
                transition(*job, JobState::Verifying);
                job->phase = keepPhase;
                job->progress = 1.0f;
            }
            job->updatedUtc = platform::nowUtc();
            store_.touch();
            notifyJobs();
        }
        break;
    case MuxEvent::Kind::Finished:
        switch (e.outcome) {
        case MuxEvent::Outcome::Done:
            if (inMux) {
                finishDone(*job, e);
            } else {
                HH_LOG_WARN(kLog, L"mux finished for job {} in state {}", job->id, toString(job->state));
            }
            break;
        case MuxEvent::Outcome::Failed:
            if (inMux) {
                job->mux = e.record;
                transition(*job, JobState::Failed, e.message.empty() ? std::wstring(L"mkvmerge failed") : e.message);
                toast(ToastRequest::Tone::Error, L"Failed: " + job->displayName() + L" - " + job->stateReason, job->id);
            }
            break;
        case MuxEvent::Outcome::Cancelled:
            if (inMux) {
                job->mux = e.record;
                transition(*job, JobState::Cancelled, e.message.empty() ? std::wstring(L"Cancelled") : e.message);
            }
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

/**
 * @brief The hint file exists and verified: record, toast, recycle policy.
 */
void Engine::finishDone(Job& job, MuxEvent& e) {
    if (!e.hintPath.empty()) {
        job.hintPath = e.hintPath;
    }
    job.mux = e.record;
    job.doneUtc = platform::nowUtc();
    job.progress = 1.0f;
    if (!job.mux.warnings.empty()) {
        addNote(job, std::format(L"mkvmerge reported {} warning(s)", job.mux.warnings.size()));
    }
    transition(job, JobState::Done);
    HH_LOG_INFO(kLog, L"job {} done: {} ({} bytes, {} ms)", job.id, job.hintPath, job.mux.outputSize, job.mux.durationMs);

    if (settings_.toastOnDone) {
        toast(ToastRequest::Tone::Success, L"Created " + path::fileName(job.hintPath), job.id, L"Reveal");
    }
    if (settings_.revealOnDone && !job.hintPath.empty()) {
        if (!platform::revealInExplorer(job.hintPath)) {
            HH_LOG_WARN(kLog, L"could not reveal {}", job.hintPath);
        }
    }
    maybeRecycle(job);
}

/**
 * @brief Applies [recycle] mode after a successful mux.
 */
void Engine::maybeRecycle(Job& job) {
    if (iequals(settings_.recycleMode, L"never")) {
        job.recycle = RecycleStatus::KeptDisabled;
        job.recycleMessage = L"Recycling is disabled in settings";
        store_.touch();
        return;
    }
    if (iequals(settings_.recycleMode, L"auto")) {
        job.recycle = RecycleStatus::Pending;
        store_.touch();
        recycleJob(job.id);
        return;
    }
    // "ask" (and anything unrecognised): the UI offers a button.
    job.recycle = RecycleStatus::Pending;
    job.recycleMessage.clear();
    store_.touch();
    toast(ToastRequest::Tone::Warning, L"Move " + job.displayName() + L" to the Recycle Bin?", job.id, L"Recycle");
    notifyJobs();
}

/**
 * @brief Moves the AME original to the Recycle Bin after re-verifying that it
 *        is still the file we muxed and that the hint file is intact.
 */
void Engine::recycleJob(JobId id) {
    Job* job = store_.find(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"recycleJob: unknown job {}", id);
        return;
    }
    if (job->state != JobState::Done) {
        HH_LOG_WARN(kLog, L"recycleJob: job {} is {} - only Done jobs are recycled", id, toString(job->state));
        return;
    }
    if (job->recycle == RecycleStatus::Recycled) {
        return;
    }

    // 1. The original must be the exact file the probe stamped.
    auto ident = platform::identity(job->outputPath);
    if (!ident) {
        job->recycle = RecycleStatus::KeptSourceChanged;
        job->recycleMessage = L"Original could not be opened: " + ident.error().toString();
    } else {
        const SourceStamp now = stampFrom(ident.value());
        if (!job->sourceStamp.valid || !now.sameFileAs(job->sourceStamp) || now.size != job->sourceStamp.size) {
            job->recycle = RecycleStatus::KeptSourceChanged;
            job->recycleMessage = L"Original changed since the mux - kept";
        }
    }
    // 2. The hint file must exist with the size mkvmerge reported.
    if (job->recycle != RecycleStatus::KeptSourceChanged) {
        bool hintOk = !job->hintPath.empty() && platform::isFile(job->hintPath);
        if (hintOk && job->mux.outputSize > 0) {
            auto size = platform::fileSize(job->hintPath);
            hintOk = size && size.value() == job->mux.outputSize;
        }
        if (!hintOk) {
            job->recycle = RecycleStatus::Failed;
            job->recycleMessage = L"Hint file missing or size mismatch - original kept";
        }
    }
    // 3. The shell move (never a permanent delete; the platform layer guarantees it).
    if (job->recycle != RecycleStatus::KeptSourceChanged && job->recycle != RecycleStatus::Failed) {
        const platform::RecycleResult res = platform::recycleFile(job->outputPath);
        job->recycleMessage = res.message;
        switch (res.outcome) {
        case platform::RecycleOutcome::Recycled:
            job->recycle = RecycleStatus::Recycled;
            break;
        case platform::RecycleOutcome::KeptRemote:
        case platform::RecycleOutcome::KeptNoBin:
        case platform::RecycleOutcome::KeptTooLarge:
            job->recycle = RecycleStatus::KeptNoBin;
            break;
        case platform::RecycleOutcome::KeptNotFound:
            job->recycle = RecycleStatus::KeptSourceChanged;
            break;
        case platform::RecycleOutcome::Failed:
        default:
            job->recycle = RecycleStatus::Failed;
            break;
        }
    }

    job->updatedUtc = platform::nowUtc();
    store_.touch();
    HH_LOG_INFO(kLog, L"job {} recycle: {} ({})", id, toString(job->recycle), job->recycleMessage);
    if (job->recycle == RecycleStatus::Recycled) {
        toast(ToastRequest::Tone::Info, L"Moved " + job->displayName() + L" to the Recycle Bin", id);
    } else {
        toast(ToastRequest::Tone::Warning, L"Original kept: " + job->recycleMessage, id);
    }
    notifyJobs();
}

// ===========================================================================
// 9. User commands
// ===========================================================================

/**
 * @brief Run / retry: dispatches immediately when the file is still the one
 *        we saw, otherwise re-probes it (the Ready event then dispatches).
 */
void Engine::runJob(JobId id) {
    Job* job = store_.find(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"runJob: unknown job {}", id);
        return;
    }
    switch (job->state) {
    case JobState::Muxing:
    case JobState::Verifying:
        HH_LOG_INFO(kLog, L"runJob: job {} is already muxing", id);
        return;
    case JobState::Discovered:
    case JobState::Encoding:
        // Still being written: just make sure nothing holds it afterwards.
        job->overrides.hold = false;
        scratch().forced.insert(id);
        store_.touch();
        notifyJobs();
        return;
    default:
        break;
    }

    job->overrides.hold = false;
    switch (checkSource(*job)) {
    case SourceCheck::Missing:
        transition(*job, JobState::Failed, kReasonSourceMissing);
        return;
    case SourceCheck::StillReady:
        // Same file, nobody writing: run the policies with the forced flag.
        scratch().forced.insert(id);
        onReady(*job);
        return;
    case SourceCheck::NeedsProbe:
    default:
        break;
    }
    // Fresh probe; no log is expected for a manual re-run.
    scratch().forced.insert(id);
    job->sourceStamp = SourceStamp{};
    transition(*job, JobState::Discovered);
    scheduleProbe(readiness_.get(), *job, false);
}

/**
 * @brief Hold: parks a Ready/Held job, or flags an in-flight one to be held
 *        when it becomes Ready.
 */
void Engine::holdJob(JobId id) {
    Job* job = store_.find(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"holdJob: unknown job {}", id);
        return;
    }
    switch (job->state) {
    case JobState::Ready:
    case JobState::Held:
        job->overrides.hold = true;
        scratch().forced.erase(id);
        transition(*job, JobState::Held, kReasonHeld);
        break;
    case JobState::Discovered:
    case JobState::Encoding:
        job->overrides.hold = true;
        scratch().forced.erase(id);
        store_.touch();
        notifyJobs();
        break;
    default:
        HH_LOG_INFO(kLog, L"holdJob: job {} is {} - nothing to hold", id, toString(job->state));
        break;
    }
}

/**
 * @brief Resume: clears the hold flag and re-evaluates a Held job.
 */
void Engine::resumeJob(JobId id) {
    Job* job = store_.find(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"resumeJob: unknown job {}", id);
        return;
    }
    job->overrides.hold = false;
    store_.touch();
    if (job->state != JobState::Held) {
        notifyJobs();
        return;
    }
    // Held jobs that never went through the prober (catch-ups) need a probe;
    // the others re-run the policies if the file is unchanged.
    switch (checkSource(*job)) {
    case SourceCheck::Missing:
        transition(*job, JobState::Failed, kReasonSourceMissing);
        break;
    case SourceCheck::StillReady:
        onReady(*job);
        break;
    case SourceCheck::NeedsProbe:
    default:
        job->sourceStamp = SourceStamp{};
        transition(*job, JobState::Discovered);
        scheduleProbe(readiness_.get(), *job, false);
        break;
    }
}

/**
 * @brief Remove: cancels whatever is running for the job and drops the row.
 */
void Engine::removeJob(JobId id) {
    Job* job = store_.find(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"removeJob: unknown job {}", id);
        return;
    }
    HH_LOG_INFO(kLog, L"job {} removed by the user (state {})", id, toString(job->state));
    cancelProbe(readiness_.get(), id);
    if (mux_) {
        mux_->cancel(id);
    }
    scratch().forced.erase(id);
    cepStartedPathsEraseFor(id);
    store_.remove(id);
    store_.touch();
    notifyJobs();
    notifyLink();
}

/**
 * @brief Replaces the per-job overrides and re-evaluates a Held job.
 */
void Engine::setJobOverrides(JobId id, const JobOverrides& overrides) {
    Job* job = store_.find(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"setJobOverrides: unknown job {}", id);
        return;
    }
    job->overrides = overrides;
    // A hand-picked LUT joins the recent list so it shows up in the chooser.
    if (overrides.lutPath && !overrides.lutPath->empty()) {
        rememberLut(*overrides.lutPath);
    }
    if (overrides.presetId && !overrides.presetId->empty()) {
        if (const HdrPreset* preset = presets_.find(*overrides.presetId)) {
            const TransferKind kind = preset->transferKind();
            if (kind != TransferKind::Unknown && job->transfer == TransferKind::Unknown) {
                inferTransfer(*job, kind, L"user");
            }
        }
    }
    // Keep the hint preview in sync unless the mux already fixed it.
    if (job->state != JobState::Muxing && job->state != JobState::Verifying && job->state != JobState::Done) {
        job->hintPath = resolvePlan(*job).hintPath;
    }
    job->updatedUtc = platform::nowUtc();
    store_.touch();
    HH_LOG_INFO(kLog, L"job {} overrides updated (preset {}, lut {}, hold {})", id,
                overrides.presetId ? *overrides.presetId : L"<default>",
                overrides.lutPath ? *overrides.lutPath : L"<default>", overrides.hold);

    if (job->state == JobState::Held) {
        if (overrides.hold) {
            transition(*job, JobState::Held, kReasonHeld);
        } else {
            resumeJob(id);
        }
        return;
    }
    notifyJobs();
}

/**
 * @brief Opens Explorer with the hint (or the source) selected.
 */
void Engine::revealJob(JobId id, bool hintFile) {
    const Job* job = store_.find(id);
    if (!job) {
        HH_LOG_WARN(kLog, L"revealJob: unknown job {}", id);
        return;
    }
    std::wstring target = hintFile ? job->hintPath : job->outputPath;
    if (target.empty() || !platform::exists(target)) {
        // Fall back to whichever of the two still exists.
        target = platform::exists(job->hintPath) ? job->hintPath : job->outputPath;
    }
    if (target.empty()) {
        return;
    }
    if (!platform::revealInExplorer(target)) {
        HH_LOG_WARN(kLog, L"could not reveal {}", target);
        toast(ToastRequest::Tone::Warning, L"Could not open Explorer for " + path::fileName(target), id);
    }
}

/**
 * @brief Enqueues a file the user dropped or picked by hand.
 */
void Engine::addManualFile(const std::wstring& path) {
    const std::wstring trimmed(platform::trim(path));
    if (trimmed.empty()) {
        return;
    }
    if (!platform::isFile(trimmed)) {
        HH_LOG_WARN(kLog, L"addManualFile: not a file: {}", trimmed);
        toast(ToastRequest::Tone::Error, L"Not a file: " + path::ellipsizeMiddle(trimmed, 48));
        return;
    }
    const std::wstring name = path::fileName(trimmed);
    if (path::isOurOutput(name, settings_.suffix) || path::isPartialOutput(name)) {
        toast(ToastRequest::Tone::Info, name + L" is an HDR Hint output");
        return;
    }
    bool created = false;
    Job& job = ensureJob(trimmed, JobSource::Manual, created);
    if (!created) {
        HH_LOG_INFO(kLog, L"addManualFile: {} is already job {} ({})", trimmed, job.id, toString(job.state));
        toast(ToastRequest::Tone::Info, name + L" is already in the list", job.id);
        return;
    }
    // No log is expected for a manual file: probe without confirmation.
    scheduleProbe(readiness_.get(), job, false);
    HH_LOG_INFO(kLog, L"manual file added as job {}: {}", job.id, trimmed);
}

/**
 * @brief Toggles automatic processing; turning it on releases the jobs that
 *        were parked with "Waiting for you".
 */
void Engine::setAutoProcess(bool on) {
    if (settings_.autoProcess == on) {
        return;
    }
    settings_.autoProcess = on;
    HH_LOG_INFO(kLog, L"auto-process {}", on ? L"on" : L"off");
    if (on) {
        // Collect ids first: resumeJob() may transition and re-order the table.
        std::vector<JobId> waiting;
        for (const Job* j : store_.all()) {
            if (j && j->state == JobState::Held && iequals(j->stateReason, kReasonWaiting) && !j->overrides.hold) {
                waiting.push_back(j->id);
            }
        }
        for (JobId id : waiting) {
            resumeJob(id);
        }
    }
    if (auto r = settings_.save(Settings::defaultPath()); !r) {
        HH_LOG_WARN(kLog, L"settings save failed: {}", r.error().toString());
    }
    notifyJobs();
}

/**
 * @brief Adds a folder to [ame] extra_watch_folders and the live watcher.
 */
void Engine::addWatchFolder(const std::wstring& folder) {
    const std::wstring norm = normalizeFolder(folder);
    if (norm.empty()) {
        return;
    }
    if (!platform::isDirectory(norm)) {
        HH_LOG_WARN(kLog, L"addWatchFolder: not a directory: {}", norm);
        toast(ToastRequest::Tone::Error, L"Not a folder: " + path::ellipsizeMiddle(norm, 48));
        return;
    }
    bool present = false;
    for (const auto& raw : settings_.extraWatchFolders) {
        if (iequals(normalizeFolder(settings_.expand(raw)), norm)) {
            present = true;
            break;
        }
    }
    if (!present) {
        settings_.extraWatchFolders.push_back(norm);
        if (auto r = settings_.save(Settings::defaultPath()); !r) {
            HH_LOG_WARN(kLog, L"settings save failed: {}", r.error().toString());
        }
    }
    if (watcher_) {
        watcher_->addFolder(norm);
    }
    HH_LOG_INFO(kLog, L"watch folder added: {}", norm);
    notifyLink();
}

/**
 * @brief Removes a folder from the extras (and from the learned list) and the watcher.
 */
void Engine::removeWatchFolder(const std::wstring& folder) {
    const std::wstring norm = normalizeFolder(folder);
    if (norm.empty()) {
        return;
    }
    // Drop every spelling of the folder from the settings list.
    auto& extras = settings_.extraWatchFolders;
    const size_t before = extras.size();
    extras.erase(std::remove_if(extras.begin(), extras.end(), [&](const std::wstring& raw) {
        return iequals(normalizeFolder(settings_.expand(raw)), norm);
    }), extras.end());
    if (extras.size() != before) {
        if (auto r = settings_.save(Settings::defaultPath()); !r) {
            HH_LOG_WARN(kLog, L"settings save failed: {}", r.error().toString());
        }
    }
    // A learned folder is forgotten too, otherwise the next log seed re-adds it.
    knownFolders_.erase(std::remove_if(knownFolders_.begin(), knownFolders_.end(), [&](const std::wstring& k) {
        return iequals(k, norm);
    }), knownFolders_.end());
    if (watcher_) {
        watcher_->removeFolder(norm);
    }
    HH_LOG_INFO(kLog, L"watch folder removed: {}", norm);
    notifyLink();
}

/**
 * @brief Re-locates mkvmerge and releases jobs that were held for its absence.
 */
void Engine::reprobeMkvmerge() {
    const bool wasOk = mkvmerge_.ok;
    mkvmerge_ = locateMkvmerge(settings_.expand(settings_.mkvmergePath), settings_.minMajorVersion);
    if (mkvmerge_.ok) {
        HH_LOG_INFO(kLog, L"mkvmerge {} at {}", mkvmerge_.shortVersion(), mkvmerge_.path);
    } else {
        HH_LOG_WARN(kLog, L"mkvmerge unavailable: {}", mkvmerge_.error);
    }
    // Jobs parked on "mkvmerge not found / too old" can flow again.
    if (mkvmerge_.ok && started_) {
        std::vector<JobId> held;
        for (const Job* j : store_.all()) {
            if (j && j->state == JobState::Held && icontains(j->stateReason, L"mkvmerge") && !j->overrides.hold) {
                held.push_back(j->id);
            }
        }
        for (JobId id : held) {
            resumeJob(id);
        }
    }
    if (wasOk != mkvmerge_.ok) {
        HH_LOG_INFO(kLog, L"mkvmerge availability changed: {}", mkvmerge_.ok ? L"ok" : L"missing");
    }
    notifyLink();
}

/**
 * @brief Pushes changed settings to the workers and refreshes derived state.
 */
void Engine::settingsChanged() {
    const std::wstring configuredMkvmerge = settings_.expand(settings_.mkvmergePath);
    applySettingsToWorkers();
    refreshLuts();
    // Re-locate mkvmerge when the configured path no longer matches what we use.
    const bool pathChanged = !configuredMkvmerge.empty() ? !iequals(configuredMkvmerge, mkvmerge_.path) : !mkvmerge_.ok;
    if (pathChanged || !mkvmerge_.ok) {
        reprobeMkvmerge();
    }
    if (auto r = settings_.save(Settings::defaultPath()); !r) {
        HH_LOG_WARN(kLog, L"settings save failed: {}", r.error().toString());
    }
    // Hint previews depend on suffix / output folder.
    for (Job* j : store_.all()) {
        if (j && j->isActive() && j->state != JobState::Muxing && j->state != JobState::Verifying) {
            j->hintPath = resolvePlan(*j).hintPath;
        }
    }
    store_.touch();
    notifyJobs();
    notifyLink();
}

/**
 * @brief Synchronous save of jobs and settings.
 */
void Engine::saveNow() {
    if (auto r = store_.save(jobsPath_); !r) {
        HH_LOG_WARN(kLog, L"jobs save failed: {}", r.error().toString());
    }
    if (auto r = settings_.save(Settings::defaultPath()); !r) {
        HH_LOG_WARN(kLog, L"settings save failed: {}", r.error().toString());
    }
    lastSaveMs_ = platform::nowMonotonicMs();
}

/**
 * @brief Re-applies settings that the workers can pick up without a restart.
 */
void Engine::applySettingsToWorkers() {
    if (!started_) {
        return;
    }
    // Watcher: extensions / suffix / debounce, and the folder set.
    if (watcher_) {
        watcher_->updateConfig(makeWatcherConfig(settings_));
        std::vector<std::wstring> folders;
        for (const auto& raw : settings_.extraWatchFolders) {
            const std::wstring norm = normalizeFolder(settings_.expand(raw));
            if (!norm.empty() && !containsFolder(folders, norm)) {
                folders.push_back(norm);
            }
        }
        for (const auto& known : knownFolders_) {
            if (!containsFolder(folders, known) && !isIgnoredFolder(settings_, known)) {
                folders.push_back(known);
            }
        }
        watcher_->setFolders(folders);
    }
    // Tailer: on/off and a rescan so new overrides are discovered.
    if (tailer_) {
        if (settings_.watchLog && !tailer_->running()) {
            tailer_->start(makeTailerConfig(settings_));
        } else if (!settings_.watchLog && tailer_->running()) {
            tailer_->stop();
            link_.logFound = false;
            link_.logPath.clear();
        } else if (tailer_->running()) {
            tailer_->rescan();
        }
    }
    // IPC: on/off (the pipe name is fixed for the process lifetime).
    if (ipc_) {
        if (settings_.ipcEnabled && !ipc_->running()) {
            if (auto r = ipc_->start(settings_.pipeName, std::max(1, settings_.maxInstances)); !r) {
                HH_LOG_WARN(kLog, L"IPC server failed to start: {}", r.error().toString());
            }
        } else if (!settings_.ipcEnabled && ipc_->running()) {
            ipc_->stop();
            panelConnections_ = 0;
        }
    }
    // Readiness thresholds are read at probe start; live probes keep theirs.
}

// ===========================================================================
// 10. IPC (panel + CEP bridge)
// ===========================================================================

/// Queues a line to one panel connection (no-op when IPC is down).
void Engine::ipcSend(uint32_t connectionId, const std::string& jsonLine) {
    if (!ipc_ || !ipc_->running()) {
        return;
    }
    if (jsonLine.empty()) {
        return;
    }
    ipc_->send(connectionId, jsonLine);
}

/// Queues a line to every panel connection (no-op when IPC is down).
void Engine::ipcBroadcast(const std::string& jsonLine) {
    if (!ipc_ || !ipc_->running()) {
        return;
    }
    if (jsonLine.empty()) {
        return;
    }
    ipc_->broadcast(jsonLine);
}

/**
 * @brief A panel connected or went away.
 */
void Engine::apply(IpcClientEvent& e) {
    if (e.connected) {
        ++panelConnections_;
        HH_LOG_INFO(kLog, L"panel connection {} opened ({} total)", e.connectionId, panelConnections_);
        // Greet the client so it knows the protocol version and app version.
        ipcSend(e.connectionId, ipc::line(ipc::makeWelcome(kAppVersion, false)));
    } else {
        panelConnections_ = std::max(0, panelConnections_ - 1);
        HH_LOG_INFO(kLog, L"panel connection {} closed ({} left)", e.connectionId, panelConnections_);
    }
    link_.panelConnected = panelConnections_ > 0;
    if (onPanelConnection) {
        onPanelConnection(e.connectionId, e.connected);
    }
    notifyLink();
}

/**
 * @brief One JSON line from a panel. The engine owns "ping" and "ame";
 *        everything else is handed to the UI.
 */
void Engine::apply(IpcMessageEvent& e) {
    auto parsed = ipc::parseLine(e.jsonLine);
    if (!parsed) {
        HH_LOG_WARN(kLog, L"IPC {}: unparseable line: {}", e.connectionId, previewLine(e.jsonLine));
        return;
    }
    const std::string kind = ipc::kindOf(*parsed);
    if (kind.empty()) {
        HH_LOG_WARN(kLog, L"IPC {}: message without kind: {}", e.connectionId, previewLine(e.jsonLine));
        return;
    }
    if (kind == "ping") {
        ipcSend(e.connectionId, ipc::line(ipc::makePong()));
        return;
    }
    if (kind == "ame") {
        applyAmeMessage(e.connectionId, ipc::str(*parsed, "type"), e.jsonLine);
        return;
    }
    // hello / panelBounds / command / calibrate / panelState / ... belong to the UI.
    if (onPanelMessage) {
        onPanelMessage(e.connectionId, kind, e.jsonLine);
    } else {
        HH_LOG_DEBUG(kLog, L"IPC {}: no panel handler for kind {}", e.connectionId, toWide(kind));
    }
}

/**
 * @brief Events relayed from AME by the CEP bridge.
 *
 * CEP signals never bypass the readiness probe: they create / confirm / fail
 * jobs but the file itself decides when it is complete.
 */
void Engine::applyAmeMessage(uint32_t conn, const std::string& type, const std::string& jsonLine) {
    auto parsed = ipc::parseLine(jsonLine);
    if (!parsed) {
        return;
    }
    const ipc::json& msg = *parsed;

    if (type == "encodingStarted") {
        const std::wstring output = ipc::wstr(msg, "outputFilePath");
        if (output.empty()) {
            HH_LOG_WARN(kLog, L"CEP {}: encodingStarted without outputFilePath", conn);
            return;
        }
        bool created = false;
        Job& job = ensureJob(output, JobSource::Cep, created);
        if (job.sourcePath.empty()) {
            job.sourcePath = ipc::wstr(msg, "sourceFilePath");
        }
        if (job.state == JobState::Discovered) {
            transition(job, JobState::Encoding);
        }
        if (job.state == JobState::Encoding) {
            job.phase = L"Encoding in AME";
            job.progress = -1.0f;
            if (job.encodeStartUtc == 0) {
                job.encodeStartUtc = platform::nowUtc();
            }
        }
        // The container does not exist until the encode ends, so no probe yet:
        // the folder watcher (registered here) or encodeComplete schedules it.
        registerFolder(path::parent(job.outputPath));
        cepStartedPaths_[kLastStartedKey] = job.outputPath;
        job.updatedUtc = platform::nowUtc();
        store_.touch();
        HH_LOG_INFO(kLog, L"CEP: encoding started for job {} ({})", job.id, job.outputPath);
        notifyJobs();
        return;
    }

    if (type == "progress") {
        const std::wstring output = ipc::wstr(msg, "outputFilePath");
        if (output.empty()) {
            return;
        }
        Job* job = store_.findActiveByKey(path::normalizeKey(platform::fullPath(output)));
        if (!job || job->state != JobState::Encoding) {
            return;
        }
        // AME reports 0..1 or 0..100 depending on the bridge build.
        double p = ipc::num(msg, "progress", -1.0);
        if (p > 1.0) {
            p /= 100.0;
        }
        if (p < 0.0) {
            return;
        }
        job->progress = static_cast<float>(std::clamp(p, 0.0, 1.0));
        job->phase = encodingPhaseText(job->progress);
        job->updatedUtc = platform::nowUtc();
        store_.touch();
        notifyJobs();
        return;
    }

    if (type == "encodeComplete") {
        const std::wstring output = ipc::wstr(msg, "outputFilePath");
        if (output.empty()) {
            HH_LOG_WARN(kLog, L"CEP {}: encodeComplete without outputFilePath", conn);
            return;
        }
        const std::wstring result = ipc::wstr(msg, "result");
        bool created = false;
        Job& job = ensureJob(output, JobSource::Cep, created);
        if (job.sourcePath.empty()) {
            job.sourcePath = ipc::wstr(msg, "sourceFilePath");
        }
        job.cepConfirmed = true;
        registerFolder(path::parent(job.outputPath));
        HH_LOG_INFO(kLog, L"CEP: encode complete for job {} result '{}'", job.id, result);

        if (isAmeFailureResult(result)) {
            const std::wstring reason = L"AME reported: " + result;
            if (job.state == JobState::Muxing || job.state == JobState::Verifying) {
                addNote(job, reason);
                store_.touch();
                notifyJobs();
            } else if (probeTracked(job.id) && readiness_) {
                readiness_->fail(job.id, reason);
            } else {
                transition(job, JobState::Failed, reason);
            }
            return;
        }
        // Success: make sure a probe runs and tell it the file is confirmed.
        if (job.state == JobState::Discovered || job.state == JobState::Encoding) {
            if (!probeTracked(job.id)) {
                scheduleProbe(readiness_.get(), job, false);
            }
            if (readiness_) {
                readiness_->confirm(job.id);
            }
        }
        job.updatedUtc = platform::nowUtc();
        store_.touch();
        notifyJobs();
        return;
    }

    if (type == "batchItemStatus") {
        // Status codes from AME's batch encoder: 2 = Failed, 4 = Encoding, 6 = Stopped.
        const int status = static_cast<int>(ipc::num(msg, "status", -1.0));
        const int group = static_cast<int>(ipc::num(msg, "groupIndex", -1.0));
        const int item = static_cast<int>(ipc::num(msg, "itemIndex", -1.0));
        if (group < 0 || item < 0) {
            return;
        }
        const std::string pairKey = std::to_string(group) + "," + std::to_string(item);
        if (status == 4) {
            // Pair the item with the most recent encodingStarted path.
            auto last = cepStartedPaths_.find(kLastStartedKey);
            if (last != cepStartedPaths_.end() && !last->second.empty()) {
                cepStartedPaths_[pairKey] = last->second;
            }
            return;
        }
        if (status == 2 || status == 6) {
            auto it = cepStartedPaths_.find(pairKey);
            if (it == cepStartedPaths_.end()) {
                HH_LOG_DEBUG(kLog, L"CEP: batch item {} status {} without a known path", toWide(pairKey), status);
                return;
            }
            const std::wstring output = it->second;
            cepStartedPaths_.erase(it);
            Job* job = store_.findActiveByKey(path::normalizeKey(platform::fullPath(output)));
            if (!job) {
                return;
            }
            const std::wstring reason = status == 2 ? L"AME reported: Failed" : L"AME reported: Stopped";
            if (job->state == JobState::Muxing || job->state == JobState::Verifying) {
                addNote(*job, reason);
                store_.touch();
                notifyJobs();
            } else if (probeTracked(job->id) && readiness_) {
                readiness_->fail(job->id, reason);
            } else {
                transition(*job, JobState::Failed, reason);
            }
        }
        return;
    }

    if (type == "queueStatus") {
        // Either a string ("running"/"stopped") or a boolean-ish number.
        bool running = link_.queueRunning;
        const std::string text = ipc::str(msg, "batchEncoderStatus");
        if (!text.empty()) {
            running = icontains(toWide(text), L"run") || icontains(toWide(text), L"encod");
        } else if (msg.contains("batchEncoderStatus")) {
            running = ipc::num(msg, "batchEncoderStatus", 0.0) != 0.0 || ipc::boolean(msg, "batchEncoderStatus", false);
        }
        if (running != link_.queueRunning) {
            link_.queueRunning = running;
            HH_LOG_INFO(kLog, L"CEP: AME queue {}", running ? L"running" : L"stopped");
            notifyLink();
        }
        return;
    }

    if (type == "bridgeReady") {
        HH_LOG_INFO(kLog, L"CEP {}: AME bridge ready", conn);
        return;
    }

    // Panel lifecycle events (appBeforeQuit, extensionUnloaded, workspaceChanged,
    // visibility, themeChanged, ...) belong to the docking layer: forward them
    // with an "ame:" prefix so the app can route them without re-parsing here.
    if (onPanelMessage) {
        onPanelMessage(conn, "ame:" + type, jsonLine);
    } else {
        HH_LOG_DEBUG(kLog, L"CEP {}: unhandled AME message type '{}'", conn, toWide(type));
    }
}

// ===========================================================================
// 11. Headless one-shot processing (--process)
// ===========================================================================

/**
 * @brief Muxes one file synchronously on the calling thread. No workers, no
 *        job table; used by the command-line mode.
 */
Result<std::wstring> Engine::processOneShot(const std::wstring& path, const std::wstring& presetId,
                                            const std::wstring& lutPath, const std::function<void(float)>& onProgress) {
    const std::wstring input = platform::fullPath(std::wstring(platform::trim(path)));
    if (input.empty() || !platform::isFile(input)) {
        return Error::text(L"Input file not found: " + path);
    }
    HH_LOG_INFO(kLog, L"one-shot: {}", input);

    // ---- mkvmerge ---------------------------------------------------------------
    if (!mkvmerge_.ok) {
        mkvmerge_ = locateMkvmerge(settings_.expand(settings_.mkvmergePath), settings_.minMajorVersion);
    }
    if (!mkvmerge_.ok) {
        return Error::text(mkvmerge_.error.empty() ? std::wstring(L"mkvmerge not found") : mkvmerge_.error);
    }

    // ---- identify the input -------------------------------------------------------
    auto identified = MkvmergeRunner::identify(mkvmerge_.path, input);
    if (!identified) {
        return Error::text(L"mkvmerge could not identify the input: " + identified.error().toString());
    }
    const Identification& ident = identified.value();
    if (!ident.recognized || !ident.supported) {
        return Error::text(L"mkvmerge does not support this input");
    }
    if (ident.videoTrackId < 0) {
        return Error::text(L"No video track in the input");
    }

    // ---- colour transfer ---------------------------------------------------------
    TransferKind transfer = TransferKind::Unknown;
    bool inband = false;
    if (presetId.empty()) {
        if (ffprobePath_.empty()) {
            ffprobePath_ = locateFfprobe();
        }
        if (!ffprobePath_.empty()) {
            if (auto info = probeMedia(ffprobePath_, input)) {
                transfer = info->transfer;
                inband = info->hasMasteringDisplay || info->hasContentLightLevel;
            }
        }
        if (transfer == TransferKind::Unknown) {
            return Error::text(L"Cannot determine colour space; pass --preset");
        }
        HH_LOG_INFO(kLog, L"one-shot: transfer {} (ffprobe){}", toString(transfer), inband ? L", in-band HDR10 SEI present" : L"");
    }

    // ---- preset ----------------------------------------------------------------
    const std::wstring effectivePresetId = !presetId.empty() ? presetId : settings_.defaultPresetFor(static_cast<int>(transfer));
    const HdrPreset* preset = presets_.find(effectivePresetId);
    if (!preset) {
        return Error::text(L"Preset not found: " + effectivePresetId);
    }
    if (preset->isSentinel()) {
        return Error::text(L"Preset '" + preset->label + L"' cannot be used directly");
    }
    if (transfer == TransferKind::Unknown) {
        transfer = preset->transferKind();
    }

    // ---- LUT ----------------------------------------------------------------------
    std::wstring lut;
    bool attach = false;
    if (iequals(lutPath, L"none")) {
        attach = false;
    } else if (lutPath.empty()) {
        lut = settings_.defaultLutFor(static_cast<int>(planKindFor(transfer)));
        attach = settings_.attachLut && !lut.empty();
    } else {
        lut = settings_.expand(lutPath);
        attach = true;
    }
    if (attach && !platform::isFile(lut)) {
        return Error::text(L"LUT file not found: " + lut);
    }

    // ---- plan ------------------------------------------------------------------------
    if (path::hasInvalidFileNameChars(settings_.suffix)) {
        return Error::text(L"Suffix contains characters not allowed in file names");
    }
    const std::wstring hint = path::hintPathFor(input, settings_.suffix, settings_.expand(settings_.outputFolder));
    if (hint.empty()) {
        return Error::text(L"Could not derive the hint file name");
    }
    if (iequals(hint, input)) {
        return Error::text(L"Hint file name equals the source file name");
    }
    const std::wstring hintFolder = path::parent(hint);
    if (!hintFolder.empty() && !platform::isDirectory(hintFolder)) {
        if (auto r = platform::createDirectories(hintFolder); !r) {
            return Error::text(L"Cannot create output folder: " + r.error().toString());
        }
    }
    if (iequals(settings_.onConflict, L"skip") && platform::exists(hint)) {
        return Error::text(L"Hint file already exists: " + hint);
    }

    MuxPlan plan;
    plan.mkvmergePath = mkvmerge_.path;
    plan.supportsUiLanguage = mkvmerge_.supportsUiLanguage;
    plan.inputPath = input;
    plan.hintPath = hint;
    plan.partialPath = path::partialPathFor(hint);
    plan.preset = *preset;
    plan.lutPath = attach ? lut : std::wstring();
    plan.attachmentMime = settings_.attachmentMime.empty() ? std::wstring(L"application/x-cube") : settings_.attachmentMime;
    plan.attachLut = attach;
    plan.trackId = ident.videoTrackId;
    plan.lowerPriority = settings_.lowerPriority;
    plan.failOnWarnings = settings_.failOnWarnings;
    plan.setTitle = settings_.setTitle;
    plan.title = path::stem(input);
    plan.onConflict = settings_.onConflict.empty() ? std::wstring(L"increment") : settings_.onConflict;
    plan.keepPartialOnFailure = settings_.keepPartialOnFailure;
    {
        const uint64_t hours = static_cast<uint64_t>(std::max(1, settings_.timeoutHours));
        plan.timeoutMs = static_cast<DWORD>(std::min<uint64_t>(hours * 3600ull * 1000ull, 0xFFFFFFFEull));
    }

    // ---- run ---------------------------------------------------------------------------
    // A manual-reset event that is never set: nothing cancels a one-shot.
    platform::UniqueHandle cancel = platform::makeEvent(true, false);
    if (!cancel) {
        return Error::fromLastError(L"CreateEvent");
    }
    MuxRecord record;
    const auto progress = [&onProgress](float p) {
        if (onProgress) {
            onProgress(p);
        }
    };
    const MuxRunResult run = MkvmergeRunner::run(plan, cancel.get(), progress, record);
    HH_LOG_INFO(kLog, L"one-shot mkvmerge exit {} status {} ({} ms)", record.exitCode, static_cast<int>(run.status), record.durationMs);

    // Anything but Done / DoneWithWarnings is a failure; drop the partial.
    if (run.status != MuxRunResult::Status::Done && run.status != MuxRunResult::Status::DoneWithWarnings) {
        if (!plan.keepPartialOnFailure) {
            if (auto r = platform::deleteFile(plan.partialPath); !r) {
                HH_LOG_WARN(kLog, L"could not delete partial {}: {}", plan.partialPath, r.error().toString());
            }
        }
        std::wstring why = run.message.empty() ? std::wstring(L"mkvmerge failed") : run.message;
        if (!record.errorText.empty()) {
            why += L": " + record.errorText;
        }
        return Error::text(why);
    }

    // ---- verify ------------------------------------------------------------------------
    // The MP4 layout (duration / mdat) tightens the verification when available.
    const Mp4Layout* layoutPtr = nullptr;
    Mp4Layout layout;
    if (auto inspected = inspectMp4(input)) {
        layout = inspected.value();
        layoutPtr = &layout;
    }
    Identification outputIdent;
    if (auto v = MkvmergeRunner::verify(plan, ident, layoutPtr, outputIdent, record); !v) {
        if (!plan.keepPartialOnFailure) {
            if (auto r = platform::deleteFile(plan.partialPath); !r) {
                HH_LOG_WARN(kLog, L"could not delete partial {}: {}", plan.partialPath, r.error().toString());
            }
        }
        return Error::text(L"Verification failed: " + v.error().toString());
    }

    // ---- finalize -------------------------------------------------------------------
    auto finalPath = MkvmergeRunner::finalizeOutput(plan);
    if (!finalPath) {
        return Error::text(L"Could not finalize the output: " + finalPath.error().toString());
    }
    HH_LOG_INFO(kLog, L"one-shot done: {}", finalPath.value());
    return finalPath.value();
}

// ===========================================================================
// 12. Registry / launcher.json / LUT listing / folders / notifications
// ===========================================================================

/**
 * @brief Writes HKCU\Software\HdrHint and %APPDATA%\HdrHint\launcher.json so
 *        the CEP panel can find and launch the executable.
 */
void Engine::updateRegistry() {
    const std::wstring exe = platform::exePath();
    if (exe.empty()) {
        HH_LOG_WARN(kLog, L"updateRegistry: executable path unknown");
        return;
    }
    const std::wstring pipeName = settings_.pipeName.empty() ? std::wstring(L"HdrHint") : settings_.pipeName;
    const std::wstring fullPipe = L"\\\\.\\pipe\\" + pipeName;

    // Registry values; each failure is logged and the rest still written.
    if (auto r = platform::regWriteString(HKEY_CURRENT_USER, kRegistryKey, L"ExePath", exe); !r) {
        HH_LOG_WARN(kLog, L"registry ExePath: {}", r.error().toString());
    }
    if (auto r = platform::regWriteString(HKEY_CURRENT_USER, kRegistryKey, L"Version", kAppVersion); !r) {
        HH_LOG_WARN(kLog, L"registry Version: {}", r.error().toString());
    }
    if (auto r = platform::regWriteString(HKEY_CURRENT_USER, kRegistryKey, L"PipeName", pipeName); !r) {
        HH_LOG_WARN(kLog, L"registry PipeName: {}", r.error().toString());
    }
    if (auto r = platform::regWriteDword(HKEY_CURRENT_USER, kRegistryKey, L"ProtocolVersion", kProtocolVersion); !r) {
        HH_LOG_WARN(kLog, L"registry ProtocolVersion: {}", r.error().toString());
    }

    // launcher.json: the panel reads this with fs instead of parsing reg.exe.
    nlohmann::json j = nlohmann::json::object();
    j["exePath"] = toUtf8(exe);
    j["pipeName"] = toUtf8(fullPipe);
    j["protocolVersion"] = kProtocolVersion;
    j["version"] = toUtf8(kAppVersion);
    const std::wstring folder = platform::appRoamingDataFolder();
    if (folder.empty()) {
        HH_LOG_WARN(kLog, L"updateRegistry: roaming data folder unknown; launcher.json not written");
        return;
    }
    const std::wstring launcher = folder + L"\\launcher.json";
    if (auto r = platform::writeAllAtomic(launcher, dumpJson(j, 2)); !r) {
        HH_LOG_WARN(kLog, L"launcher.json: {}", r.error().toString());
    } else {
        HH_LOG_INFO(kLog, L"registered {} ({})", exe, launcher);
    }
}

/**
 * @brief Adds a folder learned from a live signal (log block, CEP event) to
 *        the watch set, unless it is ignored or already watched.
 */
void Engine::registerFolder(const std::wstring& folder) {
    const std::wstring norm = normalizeFolder(folder);
    if (norm.empty()) {
        return;
    }
    if (isIgnoredFolder(settings_, norm)) {
        return;
    }
    if (containsFolder(knownFolders_, norm)) {
        return;
    }
    for (const auto& raw : settings_.extraWatchFolders) {
        if (iequals(normalizeFolder(settings_.expand(raw)), norm)) {
            return;
        }
    }
    if (!platform::isDirectory(norm)) {
        HH_LOG_DEBUG(kLog, L"registerFolder: not a directory (yet): {}", norm);
        return;
    }
    knownFolders_.push_back(norm);
    if (watcher_) {
        watcher_->addFolder(norm);
    }
    HH_LOG_INFO(kLog, L"now watching {} (learned from a live export)", norm);
    notifyLink();
}

/**
 * @brief Rebuilds the LUT list: *.cube in the LUT folder plus recents that exist.
 */
void Engine::refreshLuts() {
    std::vector<std::wstring> found;
    std::set<std::wstring> seen;   // lower-cased full paths for de-duplication

    // The configured folder first.
    const std::wstring folder = settings_.expand(settings_.lutFolder);
    if (!folder.empty() && platform::isDirectory(folder)) {
        if (auto listed = platform::listDirectory(folder)) {
            for (const auto& entry : listed.value()) {
                if (entry.isDirectory || !iequals(path::extension(entry.name), L".cube")) {
                    continue;
                }
                const std::wstring full = path::join(folder, entry.name);
                if (seen.insert(platform::toLowerInvariant(full)).second) {
                    found.push_back(full);
                }
            }
        } else {
            HH_LOG_WARN(kLog, L"cannot list LUT folder {}: {}", folder, listed.error().toString());
        }
    }
    // The configured defaults next: a LUT that lives outside the folder (a
    // migrated Python-tool path, say) must still be selectable in the
    // choosers instead of showing up as "missing".
    for (const std::wstring* raw : {&settings_.pqLutPath, &settings_.hlgLutPath}) {
        const std::wstring full = settings_.expand(*raw);
        if (full.empty() || !platform::isFile(full)) {
            continue;
        }
        if (seen.insert(platform::toLowerInvariant(full)).second) {
            found.push_back(full);
        }
    }
    // Then the recents that still exist.
    for (const auto& raw : settings_.recentLuts) {
        const std::wstring full = settings_.expand(raw);
        if (full.empty() || !platform::isFile(full)) {
            continue;
        }
        if (seen.insert(platform::toLowerInvariant(full)).second) {
            found.push_back(full);
        }
    }
    // Stable, case-insensitive ordering by file name for the chooser.
    std::sort(found.begin(), found.end(), [](const std::wstring& a, const std::wstring& b) {
        const std::wstring na = platform::toLowerInvariant(path::fileName(a));
        const std::wstring nb = platform::toLowerInvariant(path::fileName(b));
        return na != nb ? na < nb : platform::toLowerInvariant(a) < platform::toLowerInvariant(b);
    });
    luts_ = std::move(found);
    HH_LOG_DEBUG(kLog, L"{} LUT(s) available", luts_.size());
}

/**
 * @brief Moves a LUT to the front of the recent list (capped) and refreshes.
 */
void Engine::rememberLut(const std::wstring& lutPath) {
    if (lutPath.empty()) {
        return;
    }
    auto& recents = settings_.recentLuts;
    recents.erase(std::remove_if(recents.begin(), recents.end(), [&](const std::wstring& r) {
        return iequals(r, lutPath);
    }), recents.end());
    recents.insert(recents.begin(), lutPath);
    while (recents.size() > kRecentLutsMax) {
        recents.pop_back();
    }
    refreshLuts();
}

/**
 * @brief Drops the CEP pairing entries that point at a removed job's path.
 */
void Engine::cepStartedPathsEraseFor(JobId id) {
    const Job* job = store_.find(id);
    if (!job) {
        return;
    }
    const std::wstring key = job->key;
    for (auto it = cepStartedPaths_.begin(); it != cepStartedPaths_.end();) {
        if (it->first != kLastStartedKey && iequals(path::normalizeKey(it->second), key)) {
            it = cepStartedPaths_.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @brief Moves a job to a new state, keeping the bookkeeping consistent.
 */
void Engine::transition(Job& job, JobState state, std::wstring reason) {
    const JobState prev = job.state;
    // Phase / progress belong to the in-flight states only.
    const bool leavingInFlight = prev == JobState::Encoding || prev == JobState::Muxing || prev == JobState::Verifying;
    if (leavingInFlight && state != prev) {
        job.phase.clear();
        job.progress = 0.0f;
    }
    job.state = state;
    job.stateReason = std::move(reason);
    job.updatedUtc = platform::nowUtc();

    // A terminal state must not leave a probe or a forced-run marker behind.
    if (job.isTerminal()) {
        cancelProbe(readiness_.get(), job.id);
        scratch().forced.erase(job.id);
    }
    if (prev != state) {
        HH_LOG_INFO(kLog, L"job {} {} -> {}{}", job.id, toString(prev), toString(state),
                    job.stateReason.empty() ? std::wstring() : L" (" + job.stateReason + L")");
    } else if (!job.stateReason.empty()) {
        HH_LOG_DEBUG(kLog, L"job {} stays {} ({})", job.id, toString(state), job.stateReason);
    }
    store_.touch();
    notifyJobs();
    notifyLink();
}

/// Fires onJobsChanged (deferred to the end of a drain when one is running).
void Engine::notifyJobs() {
    UiThreadScratch& s = scratch();
    if (s.drainDepth > 0) {
        s.jobsChanged = true;
        return;
    }
    if (onJobsChanged) {
        onJobsChanged();
    }
}

/// Fires onLinkStateChanged (deferred to the end of a drain when one is running).
void Engine::notifyLink() {
    UiThreadScratch& s = scratch();
    if (s.drainDepth > 0) {
        s.linkChanged = true;
        return;
    }
    if (onLinkStateChanged) {
        onLinkStateChanged();
    }
}

/// Hands a toast to the UI when a handler is wired.
void Engine::toast(ToastRequest::Tone tone, std::wstring text, JobId jobId, std::wstring action) {
    if (text.empty()) {
        return;
    }
    if (!onToast) {
        HH_LOG_DEBUG(kLog, L"toast (no handler): {}", text);
        return;
    }
    ToastRequest req;
    req.tone = tone;
    req.text = std::move(text);
    req.jobId = jobId;
    req.actionLabel = std::move(action);
    onToast(req);
}

/**
 * @brief A note from a worker for the log pane.
 */
void Engine::apply(WorkerNoteEvent& e) {
    const std::wstring component = e.component.empty() ? std::wstring(L"Worker") : e.component;
    if (e.isError) {
        HH_LOG_ERROR(kLog, L"[{}] {}", component, e.text);
    } else {
        HH_LOG_INFO(kLog, L"[{}] {}", component, e.text);
    }
}

} // namespace hh
