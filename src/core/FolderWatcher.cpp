// ---------------------------------------------------------------------------
// FolderWatcher.cpp - worker thread watching render folders for AME activity.
//
// One thread owns every DirectoryWatch. The UI thread only ever touches the
// command queue (folder set / config) under mutex_ and kicks wakeEvent_; the
// worker applies those commands at the top of each loop iteration, so the
// watches, snapshots and debounce maps need no locking at all.
//
// Change notifications are coalesced per file name for debounceMs before
// classification, because AME fires FILE_ACTION_MODIFIED many times per
// second on a growing sidecar. Classification then decides between "AME
// sidecar", "candidate output" and "not interesting".
// ---------------------------------------------------------------------------
#include "core/FolderWatcher.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Process.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hh {

namespace {

/// Component tag used for every log line in this file.
constexpr const wchar_t* kLog = L"Watcher";

/// waitAny() takes 64 handles: stop + wake + 60 directories.
constexpr size_t kMaxWatchedFolders = 60;
/// Loop timeout: flushes debounced entries and drives the retry timer.
constexpr DWORD kLoopTimeoutMs = 250;
/// Unavailable folders are re-opened this often.
constexpr uint64_t kRetryIntervalMs = 30000;
/// Upper bound accepted for the debounce window.
constexpr int kMaxDebounceMs = 10000;
/// FILETIME ticks (100 ns) in one hour: the sidecar trust window.
constexpr uint64_t kOneHourTicks = 3600ull * 10000000ull;
/// FILETIME ticks in one minute (catch-up window is configured in minutes).
constexpr uint64_t kOneMinuteTicks = 60ull * 10000000ull;
/// What we ask ReadDirectoryChangesW to report.
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;

/// Command codes stored in FolderWatcher::commands_.
constexpr int kCmdReset = 0;
constexpr int kCmdAdd = 1;
constexpr int kCmdRemove = 2;

/// What a change action means to us once rename halves are folded in.
enum class ActionKind { Add, Modify, Remove };

/**
 * @brief Maps a FILE_ACTION_* code onto add / modify / remove.
 */
ActionKind toKind(DWORD action) noexcept {
    switch (action) {
    case FILE_ACTION_ADDED:
    case FILE_ACTION_RENAMED_NEW_NAME:
        return ActionKind::Add;
    case FILE_ACTION_REMOVED:
    case FILE_ACTION_RENAMED_OLD_NAME:
        return ActionKind::Remove;
    default:
        return ActionKind::Modify;
    }
}

/**
 * @brief Folds a new action into the one already pending for the same name.
 *
 * A remove always wins (the file is gone), an add supersedes anything that
 * came before it (delete + recreate), and a modify never downgrades a
 * pending add: the engine must still learn that the file is new.
 */
DWORD mergeAction(DWORD pending, DWORD incoming) noexcept {
    const ActionKind in = toKind(incoming);
    const ActionKind old = toKind(pending);
    if (in == ActionKind::Remove) {
        return FILE_ACTION_REMOVED;
    }
    if (in == ActionKind::Add) {
        return FILE_ACTION_ADDED;
    }
    // Incoming is a modify.
    return (old == ActionKind::Add) ? FILE_ACTION_ADDED : FILE_ACTION_MODIFIED;
}

/**
 * @brief Normalises a folder for the watch list: absolute, backslashes,
 *        no trailing separator (drive roots keep theirs).
 */
std::wstring normalizeFolder(std::wstring_view input) {
    const std::wstring_view trimmed = platform::trim(input);
    if (trimmed.empty()) {
        return {};
    }

    // GetFullPathNameW resolves ".." and relative pieces; fall back to a
    // plain separator clean-up when it refuses the input.
    std::wstring full = platform::fullPath(trimmed);
    if (full.empty()) {
        full = path::normalizeSeparators(trimmed);
    }

    // Strip trailing separators but keep "C:\" intact.
    while (full.size() > 1 && (full.back() == L'\\' || full.back() == L'/')) {
        if (full.size() == 3 && full[1] == L':') {
            break;
        }
        full.pop_back();
    }
    return full;
}

/**
 * @brief Case-insensitive membership test for a folder list.
 */
bool containsFolder(const std::vector<std::wstring>& list, std::wstring_view folder) {
    for (const std::wstring& f : list) {
        if (platform::iequals(f, folder)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief True when the pid belongs to a live Adobe Media Encoder process.
 */
bool pidBelongsToAme(DWORD pid) {
    if (pid == 0) {
        return false;
    }
    const std::wstring image = platform::processImageName(pid);
    return !image.empty() && platform::icontains(image, L"Adobe Media Encoder");
}

/**
 * @brief True when a last-write time lies within the last hour.
 */
bool writtenWithinHour(uint64_t lastWriteUtc, uint64_t nowUtc) noexcept {
    if (lastWriteUtc == 0 || nowUtc < lastWriteUtc) {
        return false;
    }
    return (nowUtc - lastWriteUtc) <= kOneHourTicks;
}

/**
 * @brief Clamps the watcher configuration and tidies the extension list.
 */
WatcherConfig sanitize(const WatcherConfig& in) {
    WatcherConfig c = in;
    c.debounceMs = std::clamp(c.debounceMs, 0, kMaxDebounceMs);
    c.catchUpMinutes = std::clamp(c.catchUpMinutes, 0, 60 * 24 * 365);

    // Extensions are matched with the dot; add it when the INI omitted it.
    std::vector<std::wstring> exts;
    for (const std::wstring& e : c.extensions) {
        const std::wstring_view t = platform::trim(e);
        if (t.empty() || t == L".") {
            continue;
        }
        std::wstring withDot = (t.front() == L'.') ? std::wstring(t) : (L"." + std::wstring(t));
        if (!containsFolder(exts, withDot)) {
            exts.push_back(std::move(withDot));
        }
    }
    c.extensions = std::move(exts);
    return c;
}

/// Result of classifying one file name.
struct NameClass {
    enum class Kind { Ignore, Sidecar, Candidate };
    Kind kind = Kind::Ignore;
    path::Sidecar sidecar;
    const wchar_t* why = L"";
};

/**
 * @brief Applies the name rules shared by live events, the initial scan and
 *        rescans.
 *
 * Sidecar confidence rule: a "<stem>.<pid>.<tid>.<ext>" name is only a
 * sidecar when the pid is a running AME or the file was written within the
 * last hour. Otherwise names like "Show.2024.06.m4v" fall through and are
 * treated as ordinary candidates.
 *
 * @param knownLastWrite last-write time when the caller already has it
 *                       (directory listing); otherwise the file is stat'ed
 *                       only when the pid rule alone cannot decide
 */
NameClass classifyName(const std::wstring& name, const WatcherConfig& cfg, bool isRemove, const std::wstring& folder,
                       std::optional<uint64_t> knownLastWrite) {
    NameClass out;
    if (name.empty()) {
        out.why = L"empty name";
        return out;
    }

    // Temp files (AME's "<hex>-<hex>-<hex>-<hex>.tmp" and friends) and our own
    // outputs never become jobs.
    if (path::isTemporaryName(name)) {
        out.why = L"temporary";
        return out;
    }
    if (path::isOurOutput(name, cfg.suffix) || path::isPartialOutput(name)) {
        out.why = L"our output";
        return out;
    }

    // AME sidecar?
    if (cfg.sidecarDetection) {
        const auto sc = path::parseSidecar(name);
        if (sc) {
            bool trusted = pidBelongsToAme(sc->pid);
            if (!trusted && !isRemove) {
                // The pid is not AME: fall back to the age rule.
                uint64_t lastWrite = 0;
                if (knownLastWrite) {
                    lastWrite = *knownLastWrite;
                } else {
                    auto lw = platform::lastWriteUtc(path::join(folder, name));
                    if (lw) {
                        lastWrite = lw.value();
                    }
                }
                trusted = writtenWithinHour(lastWrite, platform::nowUtc());
            }
            if (trusted) {
                out.kind = NameClass::Kind::Sidecar;
                out.sidecar = *sc;
                out.why = L"sidecar";
                return out;
            }
            // Not trusted: treat the name like any other file below.
        }
    }

    // Candidate output by extension.
    if (path::hasExtension(name, cfg.extensions)) {
        out.kind = NameClass::Kind::Candidate;
        out.why = L"candidate";
        return out;
    }
    out.why = L"extension not watched";
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Binds the watcher to the sink that receives its events.
 */
FolderWatcher::FolderWatcher(IEngineSink& sink) : sink_(sink) {}

/**
 * @brief Stops the worker (if still running) before the members go away.
 */
FolderWatcher::~FolderWatcher() {
    stop();
}

/**
 * @brief Starts the worker thread. While running, a second call only
 *        updates the configuration.
 */
void FolderWatcher::start(const WatcherConfig& config) {
    if (running_.load()) {
        updateConfig(config);
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    config_ = sanitize(config);

    // Manual-reset stop event, auto-reset wake event.
    stopEvent_ = platform::makeWaitableEvent(true);
    wakeEvent_ = platform::makeWaitableEvent(false);
    if (!stopEvent_ || !wakeEvent_) {
        HH_LOG_ERROR(kLog, L"could not create thread events: {}", Error::fromLastError(L"CreateEvent").toString());
        stopEvent_.close();
        wakeEvent_.close();
        return;
    }

    // Launch; std::thread throws on resource exhaustion, which becomes a log line.
    running_.store(true);
    try {
        thread_ = std::thread([this] { threadMain(); });
    } catch (const std::system_error& e) {
        running_.store(false);
        HH_LOG_ERROR(kLog, L"could not start watcher thread: {}", platform::toWide(e.what()));
        return;
    }
    HH_LOG_INFO(kLog, L"started (debounce {} ms, suffix '{}', {} extensions, sidecars {}, catch-up {} min)",
                config_.debounceMs, config_.suffix, config_.extensions.size(), config_.sidecarDetection,
                config_.catchUpMinutes);
}

/**
 * @brief Signals the worker and joins it. The watches are closed on the
 *        worker thread before it exits.
 */
void FolderWatcher::stop() {
    running_.store(false);
    if (stopEvent_) {
        stopEvent_.set();
    }
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            HH_LOG_ERROR(kLog, L"stop() called from the watcher thread; detaching");
            thread_.detach();
        } else {
            thread_.join();
        }
    }

    // The worker has cleared watched_ on its way out; drop what is left.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.clear();
        configPending_ = false;
    }
    stopEvent_.close();
    wakeEvent_.close();
}

// ---------------------------------------------------------------------------
// Commands from the UI thread
// ---------------------------------------------------------------------------

/**
 * @brief Replaces the whole folder set (duplicates and non-directories are ignored).
 */
void FolderWatcher::setFolders(const std::vector<std::wstring>& folders) {
    // Normalise and deduplicate first so the worker sees a clean list.
    std::vector<std::wstring> clean;
    clean.reserve(folders.size());
    for (const std::wstring& raw : folders) {
        const std::wstring f = normalizeFolder(raw);
        if (f.empty()) {
            continue;
        }
        if (platform::isFile(f)) {
            HH_LOG_WARN(kLog, L"'{}' is a file, not a folder; ignored", f);
            continue;
        }
        if (!containsFolder(clean, f)) {
            clean.push_back(f);
        }
    }

    // Publish the desired set and queue the reset + adds for the worker.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        desiredFolders_ = clean;
        commands_.emplace_back(kCmdReset, std::wstring());
        for (const std::wstring& f : clean) {
            commands_.emplace_back(kCmdAdd, f);
        }
    }
    if (wakeEvent_) {
        wakeEvent_.set();
    }
    HH_LOG_INFO(kLog, L"folder set replaced: {} folder(s)", clean.size());
}

/**
 * @brief Adds one folder (no-op when already present).
 */
void FolderWatcher::addFolder(const std::wstring& folder) {
    const std::wstring f = normalizeFolder(folder);
    if (f.empty()) {
        HH_LOG_WARN(kLog, L"addFolder: empty path ignored");
        return;
    }
    if (platform::isFile(f)) {
        HH_LOG_WARN(kLog, L"addFolder: '{}' is a file, not a folder; ignored", f);
        return;
    }

    bool added = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!containsFolder(desiredFolders_, f)) {
            desiredFolders_.push_back(f);
            commands_.emplace_back(kCmdAdd, f);
            added = true;
        }
    }
    if (!added) {
        HH_LOG_DEBUG(kLog, L"addFolder: '{}' already watched", f);
        return;
    }
    if (wakeEvent_) {
        wakeEvent_.set();
    }
    HH_LOG_INFO(kLog, L"folder added: '{}'", f);
}

/**
 * @brief Removes one folder (no-op when not present).
 */
void FolderWatcher::removeFolder(const std::wstring& folder) {
    const std::wstring f = normalizeFolder(folder);
    if (f.empty()) {
        return;
    }

    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(desiredFolders_.begin(), desiredFolders_.end(),
                               [&](const std::wstring& d) { return platform::iequals(d, f); });
        if (it != desiredFolders_.end()) {
            desiredFolders_.erase(it);
            commands_.emplace_back(kCmdRemove, f);
            removed = true;
        }
    }
    if (!removed) {
        HH_LOG_DEBUG(kLog, L"removeFolder: '{}' was not watched", f);
        return;
    }
    if (wakeEvent_) {
        wakeEvent_.set();
    }
    HH_LOG_INFO(kLog, L"folder removed: '{}'", f);
}

/**
 * @brief Current folder set (normalised, in the order they were added).
 */
std::vector<std::wstring> FolderWatcher::folders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return desiredFolders_;
}

/**
 * @brief Updates the suffix/extensions without restarting.
 */
void FolderWatcher::updateConfig(const WatcherConfig& config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingConfig_ = config;
        configPending_ = true;
    }
    if (wakeEvent_) {
        wakeEvent_.set();
    }
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

/**
 * @brief Main loop: apply commands, wait on {stop, wake, directory events},
 *        drain whatever fired, then flush debounced names and retry folders.
 */
void FolderWatcher::threadMain() {
    HH_LOG_INFO(kLog, L"watcher thread started");

    std::vector<platform::WaitHandle> handles;
    std::vector<Watched*> byHandle;
    handles.reserve(platform::kMaxWaitHandles);
    byHandle.reserve(platform::kMaxWaitHandles);

    while (running_.load()) {
        // Folder set / config changes are applied here, on this thread only.
        applyPendingCommands();

        // Build the wait list: stop, wake, then one event per live watch.
        handles.clear();
        byHandle.clear();
        handles.push_back(stopEvent_.handle());
        handles.push_back(wakeEvent_.handle());
        for (const auto& w : watched_) {
            if (!w || !w->available || !w->watch || !w->watch->active()) {
                continue;
            }
            const platform::WaitHandle ev = w->watch->event();
            if (ev == platform::kInvalidWaitHandle || handles.size() >= platform::kMaxWaitHandles) {
                continue;
            }
            handles.push_back(ev);
            byHandle.push_back(w.get());
        }

        // Sleep until something fires or the housekeeping tick elapses.
        const DWORD r = platform::waitAny(handles.data(), handles.size(),
                                                 kLoopTimeoutMs);
        if (r == 0) {
            break;
        }
        if (r == platform::kWaitFailed) {
            // Should never happen with valid handles; avoid a hot loop anyway.
            HH_LOG_ERROR(kLog, L"wait failed: {}", Error::fromLastError(L"wait").toString());
            if (stopEvent_.wait(1000)) {
                break;
            }
            continue;
        }
        if (!running_.load()) {
            break;
        }

        // waitAny() reports only the lowest signalled handle, so
        // poll every watch event with a zero timeout to keep busy folders
        // from starving quieter ones further down the list.
        if (r != platform::kWaitTimeout) {
            for (Watched* w : byHandle) {
                if (!w || !w->available || !w->watch || !w->watch->active()) {
                    continue;
                }
                if (platform::isSignalled(w->watch->event())) {
                    processChanges(*w);
                }
            }
        }

        // Housekeeping: retry unavailable folders and flush debounced names.
        const uint64_t now = platform::nowMonotonicMs();
        for (const auto& w : watched_) {
            if (!w) {
                continue;
            }
            if (!w->available) {
                if (now >= w->nextRetryMs) {
                    // A folder that never listed successfully gets the catch-up
                    // scan; one that did gets a diff against its snapshot.
                    openWatch(*w, w->snapshot.empty());
                }
                continue;
            }
            flushDue(*w, now);
        }
    }

    // Close every watch on this thread so the overlapped buffers drain safely.
    for (const auto& w : watched_) {
        if (w && w->watch) {
            w->watch->stop();
        }
    }
    watched_.clear();
    HH_LOG_INFO(kLog, L"watcher thread stopped");
}

/**
 * @brief Applies queued folder commands and configuration updates.
 *
 * desiredFolders_ is the source of truth; the command list tells us that it
 * changed (and what the UI asked for, for the log). The watched list is
 * reconciled against it: kept entries keep their watch and snapshot, new
 * ones are opened with a catch-up scan, dropped ones are stopped.
 */
void FolderWatcher::applyPendingCommands() {
    std::vector<std::pair<int, std::wstring>> cmds;
    std::vector<std::wstring> desired;
    bool configChanged = false;
    WatcherConfig newConfig;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cmds.swap(commands_);
        if (configPending_) {
            newConfig = pendingConfig_;
            configPending_ = false;
            configChanged = true;
        }
        if (!cmds.empty()) {
            desired = desiredFolders_;
        }
    }

    // Configuration first so any scan below uses the new rules.
    if (configChanged) {
        config_ = sanitize(newConfig);
        HH_LOG_INFO(kLog, L"config updated (debounce {} ms, suffix '{}', {} extensions, sidecars {}, catch-up {} min)",
                    config_.debounceMs, config_.suffix, config_.extensions.size(), config_.sidecarDetection,
                    config_.catchUpMinutes);
    }
    if (cmds.empty()) {
        return;
    }
    for (const auto& [code, folder] : cmds) {
        const wchar_t* what = (code == kCmdReset) ? L"reset" : (code == kCmdAdd) ? L"add" : (code == kCmdRemove) ? L"remove" : L"?";
        HH_LOG_DEBUG(kLog, L"command {} '{}'", what, folder);
    }

    // Reconcile: walk the desired list, reusing existing watches by name.
    std::vector<std::unique_ptr<Watched>> next;
    next.reserve(std::min(desired.size(), kMaxWatchedFolders));
    for (size_t i = 0; i < desired.size(); ++i) {
        const std::wstring& folder = desired[i];
        if (i >= kMaxWatchedFolders) {
            HH_LOG_WARN(kLog, L"folder limit of {} reached; not watching '{}'", kMaxWatchedFolders, folder);
            continue;
        }

        // Keep an existing entry as-is (watch, snapshot, debounce state).
        auto it = std::find_if(watched_.begin(), watched_.end(), [&](const std::unique_ptr<Watched>& w) {
            return w && platform::iequals(w->folder, folder);
        });
        if (it != watched_.end()) {
            next.push_back(std::move(*it));
            watched_.erase(it);
            continue;
        }

        // New folder: open the watch and report what is already there.
        auto w = std::make_unique<Watched>();
        w->folder = folder;
        openWatch(*w, true);
        next.push_back(std::move(w));
    }

    // Whatever is still in watched_ was removed from the desired set.
    for (const auto& w : watched_) {
        if (!w) {
            continue;
        }
        if (w->watch) {
            w->watch->stop();
        }
        HH_LOG_INFO(kLog, L"stopped watching '{}'", w->folder);
    }
    watched_ = std::move(next);
}

/**
 * @brief Opens (or re-opens) the directory watch for a folder and reports
 *        availability. On success the folder is scanned: a catch-up scan for
 *        a fresh folder, a snapshot diff for one that recovered.
 */
void FolderWatcher::openWatch(Watched& w, bool initialScan) {
    const uint64_t now = platform::nowMonotonicMs();
    if (!w.watch) {
        w.watch = std::make_unique<platform::DirectoryWatch>();
    }
    if (w.watch->active()) {
        w.watch->stop();
    }

    // nextRetryMs == 0 means "never failed before" (or was available), which
    // is when an unavailability event is worth posting; later retries only log.
    const bool firstFailureWouldBeNews = w.available || (w.nextRetryMs == 0);

    auto r = w.watch->start(w.folder, false, kNotifyFilter);
    if (!r) {
        w.available = false;
        w.nextRetryMs = now + kRetryIntervalMs;
        w.pending.clear();
        w.pendingAction.clear();
        const std::wstring text = r.error().toString();
        if (firstFailureWouldBeNews) {
            HH_LOG_WARN(kLog, L"'{}' unavailable: {} (retrying every {} s)", w.folder, text, kRetryIntervalMs / 1000);
            FolderAvailabilityEvent ev;
            ev.folder = w.folder;
            ev.available = false;
            ev.message = std::format(L"Folder unavailable: {}", text);
            sink_.post(EngineEvent{std::move(ev)});
        } else {
            HH_LOG_DEBUG(kLog, L"'{}' still unavailable: {}", w.folder, text);
        }
        return;
    }

    // Open: scan, then tell the engine the folder is live.
    const bool recovered = !w.available && (w.nextRetryMs != 0);
    w.available = true;
    w.nextRetryMs = 0;
    w.pending.clear();
    w.pendingAction.clear();
    if (initialScan) {
        this->initialScan(w);
    } else {
        rescanDiff(w);
    }

    FolderAvailabilityEvent ev;
    ev.folder = w.folder;
    ev.available = true;
    ev.message = recovered ? L"Folder available again" : L"Watching";
    sink_.post(EngineEvent{std::move(ev)});
    HH_LOG_INFO(kLog, L"{} '{}'", recovered ? L"recovered" : L"watching", w.folder);
}

/**
 * @brief Drains a signalled watch into the debounce maps.
 *
 * An overflow (the OS dropped notifications) triggers a snapshot diff; a
 * fatal error (volume gone) marks the folder unavailable and starts the
 * retry timer.
 */
void FolderWatcher::processChanges(Watched& w) {
    if (!w.watch || !w.watch->active()) {
        return;
    }

    bool overflowed = false;
    const std::vector<platform::DirectoryChange> changes = w.watch->drain(overflowed);
    const uint64_t now = platform::nowMonotonicMs();

    // Fatal: the watch closed itself (network share dropped, folder deleted).
    if (!w.watch->active()) {
        const DWORD err = w.watch->lastError();
        w.watch->stop();
        w.available = false;
        w.nextRetryMs = now + kRetryIntervalMs;
        w.pending.clear();
        w.pendingAction.clear();
        const std::wstring text = win32ErrorText(err);
        HH_LOG_WARN(kLog, L"'{}' watch failed: {} (0x{:08X}); retrying every {} s", w.folder, text, err,
                    kRetryIntervalMs / 1000);
        FolderAvailabilityEvent ev;
        ev.folder = w.folder;
        ev.available = false;
        ev.message = std::format(L"Folder unavailable: {}", text);
        sink_.post(EngineEvent{std::move(ev)});
        return;
    }

    // Overflow: whatever we had pending is covered by the full diff.
    if (overflowed) {
        HH_LOG_WARN(kLog, L"'{}': notification buffer overflowed; rescanning", w.folder);
        w.pending.clear();
        w.pendingAction.clear();
        rescanDiff(w);
    }

    // Coalesce per name. The due time is fixed by the first event of a burst
    // so a continuously growing file still yields an update every debounceMs.
    const uint64_t debounce = static_cast<uint64_t>(std::clamp(config_.debounceMs, 0, kMaxDebounceMs));
    for (const platform::DirectoryChange& c : changes) {
        if (c.name.empty()) {
            continue;
        }
        // We watch non-recursively; anything with a separator is not ours.
        if (c.name.find(L'\\') != std::wstring::npos || c.name.find(L'/') != std::wstring::npos) {
            continue;
        }
        auto action = w.pendingAction.find(c.name);
        if (action == w.pendingAction.end()) {
            w.pendingAction.emplace(c.name, c.action);
            w.pending.emplace(c.name, now + debounce);
        } else {
            action->second = mergeAction(action->second, c.action);
            if (w.pending.find(c.name) == w.pending.end()) {
                w.pending.emplace(c.name, now + debounce);
            }
        }
    }
}

/**
 * @brief Classifies every debounced name whose window has elapsed.
 */
void FolderWatcher::flushDue(Watched& w, uint64_t nowMs) {
    if (w.pending.empty()) {
        return;
    }

    // Collect first: classify() may touch the maps indirectly via snapshots.
    std::vector<std::pair<std::wstring, DWORD>> due;
    for (auto it = w.pending.begin(); it != w.pending.end();) {
        if (it->second > nowMs) {
            ++it;
            continue;
        }
        DWORD action = FILE_ACTION_MODIFIED;
        auto a = w.pendingAction.find(it->first);
        if (a != w.pendingAction.end()) {
            action = a->second;
            w.pendingAction.erase(a);
        }
        due.emplace_back(it->first, action);
        it = w.pending.erase(it);
    }

    for (const auto& [name, action] : due) {
        classify(w, name, action);
    }
}

/**
 * @brief Turns one coalesced change into a SidecarEvent / OutputEvent (or nothing).
 */
void FolderWatcher::classify(Watched& w, const std::wstring& name, DWORD action) {
    if (name.empty()) {
        return;
    }
    const ActionKind kind = toKind(action);
    const bool isRemove = (kind == ActionKind::Remove);
    const std::wstring full = path::join(w.folder, name);
    const NameClass nc = classifyName(name, config_, isRemove, w.folder, std::nullopt);

    switch (nc.kind) {
    case NameClass::Kind::Ignore:
        HH_LOG_TRACE(kLog, L"ignored '{}' ({})", name, nc.why);
        return;

    case NameClass::Kind::Sidecar: {
        SidecarEvent ev;
        ev.folder = w.folder;
        ev.stem = nc.sidecar.stem;
        ev.pid = nc.sidecar.pid;
        ev.tid = nc.sidecar.tid;
        ev.ext = nc.sidecar.ext;
        ev.gone = isRemove;
        if (!isRemove) {
            auto size = platform::fileSize(full);
            if (!size) {
                // Vanished between the event and the flush: report "gone" only
                // if we ever told the engine about it.
                if (w.snapshot.find(name) == w.snapshot.end()) {
                    HH_LOG_DEBUG(kLog, L"sidecar '{}' came and went within the debounce window", name);
                    return;
                }
                ev.gone = true;
            } else {
                ev.size = size.value();
            }
        }
        if (ev.gone) {
            w.snapshot.erase(name);
            HH_LOG_INFO(kLog, L"sidecar gone: '{}' (stem '{}', pid {})", name, ev.stem, ev.pid);
        } else {
            w.snapshot[name] = ev.size;
            HH_LOG_DEBUG(kLog, L"sidecar '{}' {} bytes (stem '{}', pid {}, tid {})", name, ev.size, ev.stem, ev.pid,
                         ev.tid);
        }
        sink_.post(EngineEvent{std::move(ev)});
        return;
    }

    case NameClass::Kind::Candidate: {
        OutputEvent ev;
        ev.path = full;
        if (isRemove) {
            ev.action = OutputEvent::Action::Removed;
            w.snapshot.erase(name);
            HH_LOG_INFO(kLog, L"output removed: '{}'", full);
            sink_.post(EngineEvent{std::move(ev)});
            return;
        }
        auto size = platform::fileSize(full);
        if (!size) {
            // Gone already: only report it if the engine ever heard of it.
            if (w.snapshot.find(name) == w.snapshot.end()) {
                HH_LOG_DEBUG(kLog, L"candidate '{}' came and went within the debounce window", name);
                return;
            }
            ev.action = OutputEvent::Action::Removed;
            w.snapshot.erase(name);
            HH_LOG_INFO(kLog, L"output removed: '{}'", full);
            sink_.post(EngineEvent{std::move(ev)});
            return;
        }
        ev.size = size.value();
        // A modify for a name we never reported is still an add from the engine's view.
        const bool known = (w.snapshot.find(name) != w.snapshot.end());
        ev.action = (kind == ActionKind::Add || !known) ? OutputEvent::Action::Added : OutputEvent::Action::Modified;
        w.snapshot[name] = ev.size;
        if (ev.action == OutputEvent::Action::Added) {
            HH_LOG_INFO(kLog, L"output added: '{}' ({} bytes)", full, ev.size);
        } else {
            HH_LOG_DEBUG(kLog, L"output modified: '{}' ({} bytes)", full, ev.size);
        }
        sink_.post(EngineEvent{std::move(ev)});
        return;
    }
    }
}

/**
 * @brief Lists a freshly opened folder: records the snapshot, reports live
 *        sidecars and candidates written within catchUpMinutes.
 */
void FolderWatcher::initialScan(Watched& w) {
    auto listing = platform::listDirectory(w.folder);
    if (!listing) {
        HH_LOG_WARN(kLog, L"initial scan of '{}' failed: {}", w.folder, listing.error().toString());
        return;
    }

    // The snapshot is the full listing so later rescans diff correctly.
    w.snapshot.clear();
    const uint64_t nowUtc = platform::nowUtc();
    const uint64_t catchUpTicks = static_cast<uint64_t>(std::max(config_.catchUpMinutes, 0)) * kOneMinuteTicks;
    size_t sidecars = 0;
    size_t candidates = 0;

    for (const platform::DirEntry& e : listing.value()) {
        if (e.isDirectory || e.name.empty()) {
            continue;
        }
        w.snapshot[e.name] = e.size;

        const NameClass nc = classifyName(e.name, config_, false, w.folder, e.lastWriteUtc);
        if (nc.kind == NameClass::Kind::Sidecar) {
            SidecarEvent ev;
            ev.folder = w.folder;
            ev.stem = nc.sidecar.stem;
            ev.pid = nc.sidecar.pid;
            ev.tid = nc.sidecar.tid;
            ev.ext = nc.sidecar.ext;
            ev.size = e.size;
            ev.gone = false;
            HH_LOG_INFO(kLog, L"initial scan: sidecar '{}' ({} bytes)", e.name, e.size);
            sink_.post(EngineEvent{std::move(ev)});
            ++sidecars;
        } else if (nc.kind == NameClass::Kind::Candidate) {
            // Only recent files are worth a catch-up; old exports were handled long ago.
            const uint64_t age = (nowUtc > e.lastWriteUtc) ? (nowUtc - e.lastWriteUtc) : 0;
            if (age > catchUpTicks) {
                continue;
            }
            OutputEvent ev;
            ev.path = path::join(w.folder, e.name);
            ev.action = OutputEvent::Action::Added;
            ev.size = e.size;
            ev.fromInitialScan = true;
            HH_LOG_INFO(kLog, L"initial scan: candidate '{}' ({} bytes)", ev.path, e.size);
            sink_.post(EngineEvent{std::move(ev)});
            ++candidates;
        }
    }
    HH_LOG_INFO(kLog, L"initial scan of '{}': {} entries, {} sidecar(s), {} candidate(s)", w.folder,
                listing.value().size(), sidecars, candidates);
}

/**
 * @brief Lists the folder and diffs it against the snapshot (used after an
 *        overflow and after a folder comes back): new names are adds, missing
 *        names removes, size changes modifies.
 */
void FolderWatcher::rescanDiff(Watched& w) {
    auto listing = platform::listDirectory(w.folder);
    if (!listing) {
        HH_LOG_WARN(kLog, L"rescan of '{}' failed: {}", w.folder, listing.error().toString());
        return;
    }

    // Current state of the folder, files only.
    std::map<std::wstring, uint64_t> current;
    for (const platform::DirEntry& e : listing.value()) {
        if (!e.isDirectory && !e.name.empty()) {
            current[e.name] = e.size;
        }
    }

    // Work out the three difference sets before touching the snapshot.
    std::vector<std::wstring> removed;
    std::vector<std::wstring> added;
    std::vector<std::wstring> modified;
    for (const auto& [name, size] : w.snapshot) {
        if (current.find(name) == current.end()) {
            removed.push_back(name);
        }
    }
    for (const auto& [name, size] : current) {
        auto it = w.snapshot.find(name);
        if (it == w.snapshot.end()) {
            added.push_back(name);
        } else if (it->second != size) {
            modified.push_back(name);
        }
    }

    // Report through the normal classifier so the same rules apply.
    for (const std::wstring& name : removed) {
        classify(w, name, FILE_ACTION_REMOVED);
    }
    for (const std::wstring& name : added) {
        classify(w, name, FILE_ACTION_ADDED);
    }
    for (const std::wstring& name : modified) {
        classify(w, name, FILE_ACTION_MODIFIED);
    }

    // The listing is now the authoritative snapshot.
    w.snapshot = std::move(current);
    HH_LOG_INFO(kLog, L"rescan of '{}': +{} ~{} -{}", w.folder, added.size(), modified.size(), removed.size());
}

} // namespace hh
