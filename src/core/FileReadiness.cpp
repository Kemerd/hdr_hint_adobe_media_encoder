// ---------------------------------------------------------------------------
// FileReadiness.cpp - decides when an AME output is complete and safe to read.
//
// One worker thread owns a map of probes. Every probe is a chain of cheap
// checks that must all pass on the same run:
//
//   exists -> deny-write open -> size stable -> no sidecars -> box chain
//          -> log/CEP confirmation (or its timeout) -> Ready
//
// The map is guarded by a mutex because the UI thread schedules, confirms,
// fails and cancels probes at any time. The worker copies a probe out, runs
// the checks without holding the lock (the checks touch the disk), and then
// merges the result back, so a slow network volume never stalls the UI.
// ---------------------------------------------------------------------------
#include "core/FileReadiness.h"

#include "core/Logger.h"
#include "core/Mp4Boxes.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Process.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace hh {

namespace {

/// Component tag used for every log line in this file.
constexpr const wchar_t* kLog = L"Readiness";

/// Marker in Probe::dueMs: the probe reported a terminal outcome and must be dropped.
constexpr uint64_t kDone = 0;

/// After this much time in "AME holds the file" the probe interval starts doubling.
constexpr uint64_t kBackoffAfterMs = 2ull * 60 * 1000;
/// Access-denied (or other open errors) become a failure after this long.
constexpr uint64_t kOpenErrorFailAfterMs = 10ull * 60 * 1000;
/// Longest the worker sleeps between wake-ups even when nothing is due.
constexpr uint64_t kMaxSleepMs = 30000;
/// Milliseconds in one hour (encoding timeout is configured in hours).
constexpr uint64_t kMsPerHour = 3600ull * 1000;
/// FILETIME ticks (100 ns) in one hour: the sidecar trust window.
constexpr uint64_t kOneHourTicks = 3600ull * 10000000ull;

/// Phase labels shown by the UI while a probe is not yet Ready.
constexpr const wchar_t* kPhaseWaitingForFile = L"Waiting for file";
constexpr const wchar_t* kPhaseWaitingRelease = L"Waiting for AME to release file";
constexpr const wchar_t* kPhaseWaitingData = L"Waiting for data";
constexpr const wchar_t* kPhaseFinalizing = L"Finalizing";
constexpr const wchar_t* kPhaseSidecars = L"Waiting for sidecars to clear";
constexpr const wchar_t* kPhaseWaitingLog = L"Waiting for AME log";
constexpr const wchar_t* kPhaseOpenRetry = L"Cannot open file, retrying";

/**
 * @brief Clamps the user-provided configuration into a sane range.
 *
 * The values come from an INI file, so anything (zero, negative, absurd) has
 * to be tolerated without producing a busy loop or an overflow later on.
 */
ReadinessConfig sanitize(const ReadinessConfig& in) {
    ReadinessConfig c = in;
    c.probeIntervalMs = std::clamp(c.probeIntervalMs, 100, 60000);
    c.maxIntervalMs = std::clamp(c.maxIntervalMs, c.probeIntervalMs, 600000);
    c.stableSeconds = std::clamp(c.stableSeconds, 0, 3600);
    c.confirmTimeoutS = std::clamp(c.confirmTimeoutS, 0, 86400);
    c.encodingTimeoutHours = std::clamp(c.encodingTimeoutHours, 1, 24 * 30);
    c.missingGraceS = std::clamp(c.missingGraceS, 0, 86400);
    return c;
}

/**
 * @brief Copies a FileIdentity into the job model's SourceStamp (same fields).
 */
SourceStamp toStamp(const platform::FileIdentity& id) noexcept {
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

/**
 * @brief Finalize progress estimate: output size over the sidecar bytes AME
 *        produced during the encode. -1 when either side is unknown.
 */
float finalizeProgress(uint64_t size, uint64_t sidecarBytes) noexcept {
    if (size == 0 || sidecarBytes == 0) {
        return -1.0f;
    }
    const double ratio = static_cast<double>(size) / static_cast<double>(sidecarBytes);
    return static_cast<float>(std::clamp(ratio, 0.0, 1.0));
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
 * @brief True when a sidecar-looking file is worth trusting as a sidecar:
 *        its pid is a running AME, or it was written within the last hour.
 *
 * Without this rule a neighbour called "Show.2024.06.m4v" would block the
 * readiness of "Show.mp4" for the whole encoding timeout.
 */
bool trustedSidecar(const path::Sidecar& sc, uint64_t lastWriteUtc, uint64_t nowUtc) {
    if (pidBelongsToAme(sc.pid)) {
        return true;
    }
    if (lastWriteUtc == 0 || nowUtc < lastWriteUtc) {
        return false;
    }
    return (nowUtc - lastWriteUtc) <= kOneHourTicks;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Binds the prober to the sink that receives its ProbeEvents.
 */
FileReadiness::FileReadiness(IEngineSink& sink) : sink_(sink) {}

/**
 * @brief Stops the worker (if still running) before the members go away.
 */
FileReadiness::~FileReadiness() {
    stop();
}

/**
 * @brief Starts the probe thread. A second call while running is ignored.
 */
void FileReadiness::start(const ReadinessConfig& config) {
    // The configuration is read by the worker without a lock, so it may only
    // change while no thread is running.
    if (running_.load()) {
        HH_LOG_WARN(kLog, L"start() called while already running; ignored");
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

    // Launch; std::thread throws on resource exhaustion, which we turn into a log line.
    running_.store(true);
    try {
        thread_ = std::thread([this] { threadMain(); });
    } catch (const std::system_error& e) {
        running_.store(false);
        HH_LOG_ERROR(kLog, L"could not start probe thread: {}", platform::toWide(e.what()));
        return;
    }
    HH_LOG_INFO(kLog, L"started (interval {} ms, max {} ms, stable {} s, structure {}, confirm timeout {} s)",
                config_.probeIntervalMs, config_.maxIntervalMs, config_.stableSeconds, config_.checkStructure,
                config_.confirmTimeoutS);
}

/**
 * @brief Signals the worker, joins it and drops every probe.
 */
void FileReadiness::stop() {
    // Flip the flag first so a loop iteration in flight exits promptly.
    running_.store(false);
    if (stopEvent_) {
        stopEvent_.set();
    }
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            // Never join ourselves; the thread body already checks running_.
            HH_LOG_ERROR(kLog, L"stop() called from the probe thread; detaching");
            thread_.detach();
        } else {
            thread_.join();
        }
    }

    // Nothing runs any more: clear the schedule and the events.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        probes_.clear();
    }
    stopEvent_.close();
    wakeEvent_.close();
}

// ---------------------------------------------------------------------------
// Commands from the UI thread
// ---------------------------------------------------------------------------

/**
 * @brief Starts probing a job's output; a probe already registered for the
 *        job is replaced (a new generation of the same export).
 */
void FileReadiness::schedule(JobId jobId, const std::wstring& path, bool requireConfirmation) {
    if (path.empty()) {
        HH_LOG_WARN(kLog, L"schedule(job {}): empty path ignored", jobId);
        return;
    }

    // Normalise once so every later Win32 call sees an absolute path.
    std::wstring full = platform::fullPath(path);
    if (full.empty()) {
        full = path;
    }

    // First probe runs immediately; the interval is the configured base rate.
    const uint64_t now = platform::nowMonotonicMs();
    Probe p;
    p.path = full;
    p.requireConfirmation = requireConfirmation;
    p.startMs = now;
    p.dueMs = now;
    p.intervalMs = static_cast<uint64_t>(std::max(config_.probeIntervalMs, 100));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        probes_[jobId] = std::move(p);
    }
    if (wakeEvent_) {
        wakeEvent_.set();
    }
    HH_LOG_INFO(kLog, L"job {}: probing '{}' (confirmation {})", jobId, full,
                requireConfirmation ? L"required" : L"not required");
}

/**
 * @brief Log/CEP reported success: the next probe may report Ready without
 *        waiting for the confirmation window.
 */
void FileReadiness::confirm(JobId jobId) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = probes_.find(jobId);
        if (it != probes_.end()) {
            it->second.confirmed = true;
            if (it->second.dueMs != kDone) {
                it->second.dueMs = platform::nowMonotonicMs();
            }
            found = true;
        }
    }
    if (!found) {
        HH_LOG_DEBUG(kLog, L"confirm(job {}): no probe registered", jobId);
        return;
    }
    if (wakeEvent_) {
        wakeEvent_.set();
    }
    HH_LOG_INFO(kLog, L"job {}: confirmed by log/CEP", jobId);
}

/**
 * @brief Log reported failure: the worker posts Failed with the reason on its
 *        next pass and drops the probe.
 */
void FileReadiness::fail(JobId jobId, const std::wstring& reason) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = probes_.find(jobId);
        if (it != probes_.end()) {
            it->second.failed = true;
            it->second.failReason = reason;
            if (it->second.dueMs != kDone) {
                it->second.dueMs = platform::nowMonotonicMs();
            }
            found = true;
        }
    }
    if (!found) {
        HH_LOG_DEBUG(kLog, L"fail(job {}): no probe registered", jobId);
        return;
    }
    if (wakeEvent_) {
        wakeEvent_.set();
    }
    HH_LOG_INFO(kLog, L"job {}: failure reported: {}", jobId, reason);
}

/**
 * @brief Drops the probe without posting anything.
 */
void FileReadiness::cancel(JobId jobId) {
    size_t erased = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        erased = probes_.erase(jobId);
    }
    if (erased != 0) {
        HH_LOG_INFO(kLog, L"job {}: probe cancelled", jobId);
    }
}

/**
 * @brief Records the sidecar bytes seen for the job (finalize progress hint).
 */
void FileReadiness::setSidecarBytes(JobId jobId, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = probes_.find(jobId);
    if (it != probes_.end()) {
        it->second.sidecarBytes = bytes;
    }
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

/**
 * @brief Waits for the earliest due probe (or a wake/stop) and runs whatever
 *        is due, one probe at a time, without holding the lock during I/O.
 */
void FileReadiness::threadMain() {
    HH_LOG_INFO(kLog, L"probe thread started");

    while (running_.load()) {
        // Work out how long we can sleep: until the earliest due probe.
        DWORD waitMs = INFINITE;
        {
            const uint64_t now = platform::nowMonotonicMs();
            std::lock_guard<std::mutex> lock(mutex_);
            uint64_t earliest = UINT64_MAX;
            for (const auto& [id, p] : probes_) {
                if (p.dueMs != kDone) {
                    earliest = std::min(earliest, p.dueMs);
                }
            }
            if (earliest != UINT64_MAX) {
                waitMs = (earliest <= now) ? 0u : static_cast<DWORD>(std::min<uint64_t>(earliest - now, kMaxSleepMs));
            }
        }

        // Sleep on {stop, wake}; a timeout simply means something is due.
        const platform::WaitHandle handles[2] = {stopEvent_.handle(), wakeEvent_.handle()};
        const DWORD r = platform::waitAny(handles, 2, waitMs);
        if (r == 0) {
            break;
        }
        if (r == platform::kWaitFailed) {
            // Should never happen with valid events; avoid a hot loop anyway.
            HH_LOG_ERROR(kLog, L"wait failed: {}", Error::fromLastError(L"wait").toString());
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        if (!running_.load()) {
            break;
        }

        // Snapshot the ids that are due right now.
        std::vector<JobId> due;
        {
            const uint64_t now = platform::nowMonotonicMs();
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, p] : probes_) {
                if (p.dueMs != kDone && p.dueMs <= now) {
                    due.push_back(id);
                }
            }
        }

        // Run each due probe on a private copy, then merge the result back.
        for (const JobId id : due) {
            if (!running_.load()) {
                break;
            }

            Probe copy;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = probes_.find(id);
                if (it == probes_.end()) {
                    continue;   // cancelled while we were busy with another probe
                }
                copy = it->second;
            }

            runProbe(id, copy, platform::nowMonotonicMs());

            // Merge: the UI thread may have confirmed / failed / re-scheduled meanwhile.
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = probes_.find(id);
            if (it == probes_.end()) {
                continue;   // cancelled during the probe: drop the result
            }
            Probe& live = it->second;
            if (live.path != copy.path || live.startMs != copy.startMs) {
                continue;   // re-scheduled during the probe: the new probe starts fresh
            }
            if (copy.dueMs == kDone) {
                // Terminal outcome posted. A fail() that landed during the run
                // is still honoured so the engine hears the log's verdict.
                if (live.failed && !copy.failed) {
                    ProbeEvent ev;
                    ev.jobId = id;
                    ev.outcome = ProbeEvent::Outcome::Failed;
                    ev.message = live.failReason.empty() ? L"Encode failed" : live.failReason;
                    ev.size = copy.lastSize;
                    sink_.post(EngineEvent{std::move(ev)});
                }
                probes_.erase(it);
                continue;
            }

            // Copy back what the probe computed; keep the flags set concurrently.
            live.dueMs = copy.dueMs;
            live.intervalMs = copy.intervalMs;
            live.lastSize = copy.lastSize;
            live.lastSizeChangeMs = copy.lastSizeChangeMs;
            live.firstReadyMs = copy.firstReadyMs;
            live.lastSeenMs = copy.lastSeenMs;
            live.attempts = copy.attempts;
            live.lastPhase = copy.lastPhase;
            if ((live.confirmed && !copy.confirmed) || (live.failed && !copy.failed)) {
                live.dueMs = platform::nowMonotonicMs();   // react to the new verdict right away
            }
        }
    }

    HH_LOG_INFO(kLog, L"probe thread stopped");
}

/**
 * @brief Runs the readiness chain once for one probe.
 *
 * Every early return either reschedules the probe (p.dueMs in the future)
 * or marks it finished (p.dueMs == kDone) after posting a terminal event.
 * The function never blocks longer than one CreateFile plus a handful of
 * header reads, so a probe on a network volume stays bounded.
 */
void FileReadiness::runProbe(JobId id, Probe& p, uint64_t nowMs) {
    ++p.attempts;

    // Small helpers so each step reads as a single line.
    auto postFailed = [&](std::wstring reason, uint64_t size) {
        ProbeEvent ev;
        ev.jobId = id;
        ev.outcome = ProbeEvent::Outcome::Failed;
        ev.message = std::move(reason);
        ev.size = size;
        HH_LOG_WARN(kLog, L"job {}: failed: {}", id, ev.message);
        sink_.post(EngineEvent{std::move(ev)});
        p.dueMs = kDone;
    };
    auto postWriting = [&](const wchar_t* phase, uint64_t size, float progress, std::wstring message) {
        ProbeEvent ev;
        ev.jobId = id;
        ev.outcome = ProbeEvent::Outcome::Writing;
        ev.phase = phase;
        ev.progress = progress;
        ev.size = size;
        ev.message = std::move(message);
        if (p.lastPhase != phase) {
            HH_LOG_INFO(kLog, L"job {}: {} ({} bytes){}{}", id, phase, size, ev.message.empty() ? L"" : L" - ",
                        ev.message);
            p.lastPhase = phase;
        } else {
            HH_LOG_TRACE(kLog, L"job {}: {} ({} bytes, attempt {})", id, phase, size, p.attempts);
        }
        sink_.post(EngineEvent{std::move(ev)});
    };
    auto reschedule = [&](bool backoff) {
        const uint64_t base = static_cast<uint64_t>(config_.probeIntervalMs);
        const uint64_t cap = static_cast<uint64_t>(config_.maxIntervalMs);
        if (p.intervalMs == 0) {
            p.intervalMs = base;
        }
        if (backoff) {
            // AME can hold a large file for minutes: slow down after two of them.
            if (nowMs >= p.startMs && nowMs - p.startMs > kBackoffAfterMs) {
                p.intervalMs = std::min(p.intervalMs * 2, cap);
            }
        } else {
            p.intervalMs = base;
        }
        p.dueMs = nowMs + std::max<uint64_t>(p.intervalMs, 1);
    };

    // Step 0: verdicts delivered from outside (log failure) and the hard timeout.
    if (p.failed) {
        postFailed(p.failReason.empty() ? L"Encode failed" : p.failReason, p.lastSize);
        return;
    }
    const uint64_t timeoutMs = static_cast<uint64_t>(config_.encodingTimeoutHours) * kMsPerHour;
    if (p.startMs != 0 && nowMs >= p.startMs && nowMs - p.startMs > timeoutMs) {
        postFailed(L"Timed out waiting for AME", p.lastSize);
        return;
    }

    // Step 1: existence. A missing file is tolerated for the grace period
    // (AME may delete and recreate the output when a queue item restarts).
    const auto attributes = platform::fileAttributes(p.path);
    if (!attributes) {
        const DWORD err = attributes.error().win32;
        const bool notFound = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
        const uint64_t graceMs = static_cast<uint64_t>(config_.missingGraceS) * 1000ull;
        if (p.lastSeenMs != 0 && nowMs >= p.lastSeenMs && nowMs - p.lastSeenMs > graceMs) {
            postFailed(notFound ? std::wstring(L"Output file disappeared")
                                : std::format(L"Output file unavailable: {}", win32ErrorText(err)),
                       p.lastSize);
            return;
        }
        // Whatever we measured before no longer applies to a file that is gone.
        p.lastSize = 0;
        p.lastSizeChangeMs = 0;
        p.firstReadyMs = 0;
        postWriting(kPhaseWaitingForFile, 0, -1.0f, notFound ? std::wstring() : win32ErrorText(err));
        reschedule(false);
        return;
    }
    const platform::FileAttributes& fad = attributes.value();
    if (fad.isDirectory) {
        postFailed(L"Output path is a directory", 0);
        return;
    }
    const uint64_t size = fad.size;
    p.lastSeenMs = nowMs;

    // Step 2: deny-write open. Only a writer (AME's exclusive handle) trips
    // this; readers such as Explorer, Defender or our own mkvmerge do not.
    DWORD openErr = 0;
    const platform::OpenProbe open = platform::probeDenyWrite(p.path, &openErr);
    switch (open) {
    case platform::OpenProbe::Writing:
        p.firstReadyMs = 0;
        postWriting(kPhaseWaitingRelease, size, finalizeProgress(size, p.sidecarBytes), {});
        reschedule(true);
        return;
    case platform::OpenProbe::Missing:
        // Raced with a delete between the attribute query and the open.
        p.firstReadyMs = 0;
        postWriting(kPhaseWaitingForFile, 0, -1.0f, {});
        reschedule(false);
        return;
    case platform::OpenProbe::Denied:
    case platform::OpenProbe::Error: {
        const std::wstring text = win32ErrorText(openErr);
        if (nowMs >= p.startMs && nowMs - p.startMs > kOpenErrorFailAfterMs) {
            postFailed(std::format(L"Cannot open output file: {}", text), size);
            return;
        }
        p.firstReadyMs = 0;
        postWriting(kPhaseOpenRetry, size, -1.0f, text);
        reschedule(false);
        return;
    }
    case platform::OpenProbe::Ready:
        break;
    }

    // Step 3: the size must be large enough and unchanged for stableSeconds
    // across at least two probes (the first observation only records it).
    if (p.lastSizeChangeMs == 0 || size != p.lastSize) {
        p.lastSize = size;
        p.lastSizeChangeMs = nowMs;
        p.firstReadyMs = 0;
        postWriting(size < config_.minOutputBytes ? kPhaseWaitingData : kPhaseFinalizing, size,
                    finalizeProgress(size, p.sidecarBytes), {});
        reschedule(false);
        return;
    }
    if (size < config_.minOutputBytes) {
        p.firstReadyMs = 0;
        postWriting(kPhaseWaitingData, size, -1.0f,
                    std::format(L"{} bytes, need at least {}", size, config_.minOutputBytes));
        reschedule(false);
        return;
    }
    const uint64_t stableMs = static_cast<uint64_t>(config_.stableSeconds) * 1000ull;
    if (nowMs < p.lastSizeChangeMs || nowMs - p.lastSizeChangeMs < stableMs) {
        p.firstReadyMs = 0;
        postWriting(kPhaseFinalizing, size, finalizeProgress(size, p.sidecarBytes), {});
        reschedule(false);
        return;
    }

    // Step 4: no sidecars with the same stem may remain in the folder. AME
    // deletes them right after the mux, so their presence means "not done".
    {
        const std::wstring parent = path::parent(p.path);
        const std::wstring stem = path::stem(p.path);
        auto listing = platform::listDirectory(parent);
        if (!listing) {
            // A listing failure is not evidence either way; the other checks decide.
            HH_LOG_DEBUG(kLog, L"job {}: cannot list '{}': {}", id, parent, listing.error().toString());
        } else {
            const uint64_t nowUtc = platform::nowUtc();
            for (const platform::DirEntry& e : listing.value()) {
                if (e.isDirectory) {
                    continue;
                }
                const auto sc = path::parseSidecar(e.name);
                if (!sc || !platform::iequals(sc->stem, stem)) {
                    continue;
                }
                if (!trustedSidecar(*sc, e.lastWriteUtc, nowUtc)) {
                    HH_LOG_DEBUG(kLog, L"job {}: ignoring stale sidecar-looking neighbour '{}'", id, e.name);
                    continue;
                }
                p.firstReadyMs = 0;
                postWriting(kPhaseSidecars, size, finalizeProgress(size, p.sidecarBytes), e.name);
                reschedule(false);
                return;
            }
        }
    }

    // Step 5: the box chain must land exactly on EOF with moov and mdat present.
    bool mp4Complete = false;
    double durationSec = 0.0;
    uint64_t mdatPayload = 0;
    if (config_.checkStructure) {
        auto layout = inspectMp4(p.path);
        if (!layout) {
            p.firstReadyMs = 0;
            postWriting(kPhaseFinalizing, size, finalizeProgress(size, p.sidecarBytes), layout.error().toString());
            reschedule(false);
            return;
        }
        const Mp4Layout& l = layout.value();
        if (!l.complete) {
            p.firstReadyMs = 0;
            postWriting(kPhaseFinalizing, size, finalizeProgress(size, p.sidecarBytes), l.error);
            reschedule(false);
            return;
        }
        mp4Complete = true;
        durationSec = l.durationSec;
        mdatPayload = l.mdatPayload;
    }

    // Step 6: confirmation. Everything on disk says "done"; give the log or
    // the CEP bridge a bounded window to agree before we go ahead anyway.
    if (p.firstReadyMs == 0) {
        p.firstReadyMs = nowMs;
        HH_LOG_INFO(kLog, L"job {}: all file checks passed ({} bytes, duration {:.2f} s)", id, size, durationSec);
    }
    const uint64_t confirmMs = static_cast<uint64_t>(config_.confirmTimeoutS) * 1000ull;
    if (p.requireConfirmation && !p.confirmed && nowMs >= p.firstReadyMs && nowMs - p.firstReadyMs < confirmMs) {
        postWriting(kPhaseWaitingLog, size, -1.0f, {});
        reschedule(false);
        return;
    }
    if (p.requireConfirmation && !p.confirmed) {
        HH_LOG_INFO(kLog, L"job {}: no log confirmation after {} s; proceeding on file evidence", id,
                    config_.confirmTimeoutS);
    }

    // Ready: stamp the file so a later re-export of the same path is detectable.
    ProbeEvent ev;
    ev.jobId = id;
    ev.outcome = ProbeEvent::Outcome::Ready;
    ev.phase = L"Ready";
    ev.progress = 1.0f;
    ev.size = size;
    ev.mp4Complete = mp4Complete;
    ev.durationSec = durationSec;
    ev.mdatPayload = mdatPayload;
    auto ident = platform::identity(p.path);
    if (ident) {
        ev.stamp = toStamp(ident.value());
    } else {
        HH_LOG_WARN(kLog, L"job {}: could not read file identity: {}", id, ident.error().toString());
        ev.stamp.size = size;
        ev.stamp.lastWriteUtc = fad.lastWriteUtc;
        ev.stamp.creationUtc = fad.creationUtc;
        ev.stamp.valid = false;
    }
    HH_LOG_INFO(kLog, L"job {}: ready '{}' ({} bytes, {} attempts)", id, p.path, size, p.attempts);
    sink_.post(EngineEvent{std::move(ev)});
    p.dueMs = kDone;
}

} // namespace hh
