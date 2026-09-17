// ---------------------------------------------------------------------------
// MuxWorker.cpp - the one thread that runs mkvmerge jobs sequentially.
//
// Pipeline per job: identify -> (ffprobe) -> mux -> verify -> finalize.
// Every outcome is reported back through IEngineSink::post as a MuxEvent;
// the worker never touches UI or store state directly.
// ---------------------------------------------------------------------------
#include "core/MuxWorker.h"

#include "core/Logger.h"
#include "core/MediaProbe.h"
#include "core/Mp4Boxes.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace hh {

namespace {

/// Component tag used for every log line in this file.
constexpr const wchar_t* kLog = L"MuxWorker";

/// Back-off when WaitForMultipleObjects itself fails (should never happen).
constexpr DWORD kWaitFailureBackoffMs = 250;

/**
 * @brief Removes the partial output unless the plan asks to keep it.
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

} // namespace

// ---------------------------------------------------------------------------
// construction / lifetime
// ---------------------------------------------------------------------------

MuxWorker::MuxWorker(IEngineSink& sink)
    : sink_(sink)
    , stopEvent_(platform::makeEvent(true, false))
    , wakeEvent_(platform::makeEvent(false, false))
    , cancelEvent_(platform::makeEvent(true, false)) {
    // Event creation only fails under extreme resource pressure; log it so a
    // silent "worker never wakes" has a trail.
    if (!stopEvent_ || !wakeEvent_ || !cancelEvent_) {
        HH_LOG_ERROR(kLog, L"could not create worker events ({})", Error::fromLastError(L"CreateEventW").toString());
    }
}

MuxWorker::~MuxWorker() {
    stop();
}

void MuxWorker::start() {
    // Idempotent: a second start() while running is a no-op.
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        HH_LOG_DEBUG(kLog, L"start() called while already running");
        return;
    }
    if (thread_.joinable()) {
        // A previous thread that was never joined (stop() not called): join it
        // now so we never own two threads.
        thread_.join();
    }
    if (stopEvent_) {
        ::ResetEvent(stopEvent_.get());
    }
    if (cancelEvent_) {
        ::ResetEvent(cancelEvent_.get());
    }
    thread_ = std::thread(&MuxWorker::threadMain, this);
    HH_LOG_INFO(kLog, L"started");
}

void MuxWorker::stop() {
    // Signal stop and cancel so both the idle wait and a running mux return.
    const bool wasRunning = running_.exchange(false);
    if (stopEvent_) {
        ::SetEvent(stopEvent_.get());
    }
    if (cancelEvent_) {
        ::SetEvent(cancelEvent_.get());
    }
    if (wakeEvent_) {
        ::SetEvent(wakeEvent_.get());
    }

    // Join unless we are being called from the worker thread itself (which
    // would deadlock); that case cannot happen with the current call sites
    // but costs nothing to guard.
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            HH_LOG_ERROR(kLog, L"stop() called from the worker thread; detaching");
            thread_.detach();
        } else {
            thread_.join();
        }
    }
    if (wasRunning) {
        HH_LOG_INFO(kLog, L"stopped");
    }
}

// ---------------------------------------------------------------------------
// queue management
// ---------------------------------------------------------------------------

void MuxWorker::enqueue(MuxRequest request) {
    if (request.jobId == 0) {
        HH_LOG_WARN(kLog, L"enqueue() with job id 0 ignored");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(request));
        HH_LOG_DEBUG(kLog, L"queued job {} ({} waiting)", queue_.back().jobId, queue_.size());
    }
    if (!running_.load()) {
        HH_LOG_WARN(kLog, L"enqueue() while the worker is not running; job waits for start()");
    }
    if (wakeEvent_) {
        ::SetEvent(wakeEvent_.get());
    }
}

void MuxWorker::cancel(JobId jobId) {
    if (jobId == 0) {
        return;
    }

    // The "is it running or queued?" decision is made under the queue lock so
    // it cannot race the worker popping the same job.
    std::optional<MuxRequest> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_.load() == jobId) {
            if (cancelEvent_) {
                ::SetEvent(cancelEvent_.get());
            }
            HH_LOG_INFO(kLog, L"cancel requested for running job {}", jobId);
            return;
        }
        const auto it = std::find_if(queue_.begin(), queue_.end(),
                                     [jobId](const MuxRequest& r) { return r.jobId == jobId; });
        if (it != queue_.end()) {
            removed = std::move(*it);
            queue_.erase(it);
        }
    }

    // Queued job: report it as cancelled right away, outside the lock.
    if (removed.has_value()) {
        HH_LOG_INFO(kLog, L"removed queued job {}", jobId);
        MuxEvent ev;
        ev.jobId = jobId;
        ev.kind = MuxEvent::Kind::Finished;
        ev.outcome = MuxEvent::Outcome::Cancelled;
        ev.message = L"Cancelled before start";
        sink_.post(EngineEvent{std::move(ev)});
    } else {
        HH_LOG_DEBUG(kLog, L"cancel({}) matched neither the running nor a queued job", jobId);
    }
}

// ---------------------------------------------------------------------------
// thread
// ---------------------------------------------------------------------------

void MuxWorker::threadMain() {
    HH_LOG_DEBUG(kLog, L"worker thread running");
    HANDLE waits[2] = {stopEvent_.get(), wakeEvent_.get()};

    for (;;) {
        // Drain the queue one job at a time.
        for (;;) {
            if (stopEvent_ && ::WaitForSingleObject(stopEvent_.get(), 0) == WAIT_OBJECT_0) {
                break;
            }

            // Pop under the lock and publish current_ in the same critical
            // section so cancel() sees a consistent picture. The manual-reset
            // cancel event is cleared here too, before current_ becomes
            // visible: a cancel() that lands after the lock is released then
            // sets the event and is never wiped out by a late reset.
            MuxRequest request;
            bool have = false;
            bool stopping = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty()) {
                    if (cancelEvent_) {
                        ::ResetEvent(cancelEvent_.get());
                    }
                    // stop() signals stop before cancel, so re-checking stop
                    // after the reset guarantees a shutdown that raced the
                    // reset is still observed: either stop is visible here
                    // or its cancel signal lands after the reset.
                    if (stopEvent_ && ::WaitForSingleObject(stopEvent_.get(), 0) == WAIT_OBJECT_0) {
                        stopping = true;
                    } else {
                        request = std::move(queue_.front());
                        queue_.pop_front();
                        current_.store(request.jobId);
                        busy_.store(true);
                        have = true;
                    }
                }
            }
            if (stopping || !have) {
                break;
            }

            // process() reports through the sink; nothing may escape the thread.
            try {
                process(request);
            } catch (...) {
                HH_LOG_ERROR(kLog, L"unexpected exception while processing job {}", request.jobId);
                MuxEvent ev;
                ev.jobId = request.jobId;
                ev.kind = MuxEvent::Kind::Finished;
                ev.outcome = MuxEvent::Outcome::Failed;
                ev.message = L"Internal error while muxing";
                sink_.post(EngineEvent{std::move(ev)});
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                current_.store(0);
                busy_.store(false);
            }
        }

        // Stop requested: report whatever is still queued as cancelled.
        if (!stopEvent_ || ::WaitForSingleObject(stopEvent_.get(), 0) == WAIT_OBJECT_0) {
            std::deque<MuxRequest> leftovers;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                leftovers.swap(queue_);
            }
            for (const auto& request : leftovers) {
                MuxEvent ev;
                ev.jobId = request.jobId;
                ev.kind = MuxEvent::Kind::Finished;
                ev.outcome = MuxEvent::Outcome::Cancelled;
                ev.message = L"Worker stopped";
                sink_.post(EngineEvent{std::move(ev)});
            }
            break;
        }

        // Idle: wait for stop or new work.
        const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
            continue;   // stop: the top of the loop handles the leftovers
        }
        if (w == WAIT_OBJECT_0 + 1) {
            continue;   // wake: drain again
        }
        // WAIT_FAILED (or an abandoned handle): never spin hot.
        HH_LOG_ERROR(kLog, L"WaitForMultipleObjects failed: {}", Error::fromLastError(L"WaitForMultipleObjects").toString());
        ::Sleep(kWaitFailureBackoffMs);
        if (!running_.load()) {
            break;
        }
    }
    HH_LOG_DEBUG(kLog, L"worker thread exiting");
}

// ---------------------------------------------------------------------------
// one job
// ---------------------------------------------------------------------------

void MuxWorker::process(MuxRequest& request) {
    const JobId jobId = request.jobId;
    MuxPlan& plan = request.plan;
    const uint64_t startedMs = platform::nowMonotonicMs();

    // Everything learned about the source travels with every Finished event
    // so the engine can record it whether the mux succeeded or not.
    TransferKind probedTransfer = TransferKind::Unknown;
    bool inbandHdr10 = false;
    MuxRecord record;

    // Small helpers so every post() carries the job id and the probe results.
    auto postProgress = [&](const wchar_t* phase, float progress) {
        MuxEvent ev;
        ev.jobId = jobId;
        ev.kind = MuxEvent::Kind::Progress;
        ev.phase = phase;
        ev.progress = progress;
        ev.probedTransfer = probedTransfer;
        ev.inbandHdr10 = inbandHdr10;
        sink_.post(EngineEvent{std::move(ev)});
    };
    auto postFinished = [&](MuxEvent::Outcome outcome, std::wstring message, std::wstring hintPath) {
        MuxEvent ev;
        ev.jobId = jobId;
        ev.kind = MuxEvent::Kind::Finished;
        ev.outcome = outcome;
        ev.progress = outcome == MuxEvent::Outcome::Done ? 1.0f : 0.0f;
        ev.message = std::move(message);
        ev.hintPath = std::move(hintPath);
        ev.record = record;
        ev.probedTransfer = probedTransfer;
        ev.inbandHdr10 = inbandHdr10;
        sink_.post(EngineEvent{std::move(ev)});
    };
    auto fail = [&](std::wstring message) {
        HH_LOG_ERROR(kLog, L"job {} failed: {}", jobId, message);
        discardPartial(plan);
        postFinished(MuxEvent::Outcome::Failed, std::move(message), {});
    };
    auto cancelled = [&]() -> bool {
        return cancelEvent_ && ::WaitForSingleObject(cancelEvent_.get(), 0) == WAIT_OBJECT_0;
    };
    auto finishCancelled = [&]() {
        HH_LOG_INFO(kLog, L"job {} cancelled", jobId);
        discardPartial(plan);
        postFinished(MuxEvent::Outcome::Cancelled, L"Cancelled", {});
    };

    // ---- Started ------------------------------------------------------------
    HH_LOG_INFO(kLog, L"job {} starting: '{}' -> '{}'", jobId, plan.inputPath, plan.hintPath);
    {
        MuxEvent ev;
        ev.jobId = jobId;
        ev.kind = MuxEvent::Kind::Started;
        ev.phase = L"Identifying";
        ev.progress = 0.0f;
        sink_.post(EngineEvent{std::move(ev)});
    }

    // ---- Plan sanity --------------------------------------------------------
    if (platform::trim(plan.mkvmergePath).empty()) {
        fail(L"mkvmerge path is not set");
        return;
    }
    if (!platform::isFile(plan.mkvmergePath)) {
        fail(std::format(L"mkvmerge not found at '{}'", plan.mkvmergePath));
        return;
    }
    if (platform::trim(plan.inputPath).empty()) {
        fail(L"Input path is empty");
        return;
    }
    if (!platform::isFile(plan.inputPath)) {
        fail(std::format(L"Input file missing: '{}'", plan.inputPath));
        return;
    }
    if (platform::trim(plan.hintPath).empty()) {
        fail(L"Output path is empty");
        return;
    }
    if (platform::trim(plan.partialPath).empty()) {
        // The engine normally fills this; derive it so a partial name is
        // always used and the final name never holds a half-written file.
        plan.partialPath = path::partialPathFor(plan.hintPath);
        HH_LOG_DEBUG(kLog, L"job {}: derived partial path '{}'", jobId, plan.partialPath);
    }
    if (plan.attachLut && !plan.lutPath.empty() && !platform::isFile(plan.lutPath)) {
        fail(std::format(L"LUT file missing: '{}'", plan.lutPath));
        return;
    }
    if (cancelled()) {
        finishCancelled();
        return;
    }

    // ---- Identify -----------------------------------------------------------
    auto identResult = MkvmergeRunner::identify(plan.mkvmergePath, plan.inputPath);
    if (!identResult) {
        fail(identResult.error().message);
        return;
    }
    const Identification source = identResult.value();
    if (!source.errors.empty()) {
        fail(std::format(L"mkvmerge: {}", source.errors.front()));
        return;
    }
    if (!source.recognized || !source.supported) {
        fail(std::format(L"Input container is not supported by mkvmerge ('{}')", source.containerType));
        return;
    }
    if (source.videoTrackId < 0) {
        fail(L"No video track");
        return;
    }
    plan.trackId = source.videoTrackId;
    HH_LOG_INFO(kLog, L"job {}: video track {} ({}, {}), {} audio track(s)", jobId,
                source.videoTrackId, source.videoCodec, source.pixelDimensions, source.audioTrackCount);
    if (cancelled()) {
        finishCancelled();
        return;
    }

    // ---- Optional ffprobe ---------------------------------------------------
    if (request.probeInput && !platform::trim(request.ffprobePath).empty()) {
        auto media = probeMedia(request.ffprobePath, plan.inputPath);
        if (media.has_value()) {
            probedTransfer = media->transfer;
            inbandHdr10 = media->hasMasteringDisplay || media->hasContentLightLevel;
            HH_LOG_INFO(kLog, L"job {}: ffprobe transfer '{}' ({}), in-band HDR10 {}", jobId,
                        media->colorTransfer, toString(probedTransfer), inbandHdr10 ? L"yes" : L"no");
        } else {
            HH_LOG_WARN(kLog, L"job {}: ffprobe gave no result; continuing without it", jobId);
        }
        if (cancelled()) {
            finishCancelled();
            return;
        }
    }

    // ---- MP4 layout (duration / mdat size for the soft checks) ------------
    std::optional<Mp4Layout> layout;
    {
        auto layoutResult = inspectMp4(plan.inputPath);
        if (layoutResult) {
            layout = layoutResult.value();
            HH_LOG_DEBUG(kLog, L"job {}: source layout complete={} duration={:.3f}s mdat={} bytes", jobId,
                         layout->complete, layout->durationSec, layout->mdatPayload);
        } else {
            HH_LOG_WARN(kLog, L"job {}: could not inspect source layout: {}", jobId, layoutResult.error().toString());
        }
    }

    // ---- Stale partial from a previous attempt -----------------------------
    if (platform::exists(plan.partialPath)) {
        auto del = platform::deleteFile(plan.partialPath);
        if (!del) {
            HH_LOG_WARN(kLog, L"job {}: could not remove stale partial '{}': {}", jobId, plan.partialPath, del.error().toString());
        } else {
            HH_LOG_INFO(kLog, L"job {}: removed stale partial '{}'", jobId, plan.partialPath);
        }
    }
    if (cancelled()) {
        finishCancelled();
        return;
    }

    // ---- Mux ----------------------------------------------------------------
    postProgress(L"Muxing", 0.0f);
    const MuxRunResult runResult = MkvmergeRunner::run(
        plan, cancelEvent_.get(),
        [&](float progress) { postProgress(L"Muxing", progress); },
        record);

    switch (runResult.status) {
    case MuxRunResult::Status::Cancelled:
        finishCancelled();
        return;
    case MuxRunResult::Status::Failed:
    case MuxRunResult::Status::Crashed:
    case MuxRunResult::Status::TimedOut:
        fail(runResult.message.empty() ? std::wstring(L"mkvmerge failed") : runResult.message);
        return;
    case MuxRunResult::Status::Done:
    case MuxRunResult::Status::DoneWithWarnings:
        break;
    }

    // ---- Verify -------------------------------------------------------------
    postProgress(L"Verifying", 1.0f);
    Identification outputIdent;
    auto verified = MkvmergeRunner::verify(plan, source, layout.has_value() ? &*layout : nullptr, outputIdent, record);
    if (!verified) {
        fail(std::format(L"Verification failed: {}", verified.error().message));
        return;
    }

    // ---- Finalize -----------------------------------------------------------
    auto finalPath = MkvmergeRunner::finalizeOutput(plan);
    if (!finalPath) {
        fail(finalPath.error().message);
        return;
    }

    // ---- Done ---------------------------------------------------------------
    std::wstring message;
    if (!record.warnings.empty()) {
        message = std::format(L"Completed with {} mkvmerge warnings", record.warnings.size());
    }
    HH_LOG_INFO(kLog, L"job {} done in {} ms: '{}' ({} bytes){}", jobId,
                platform::nowMonotonicMs() - startedMs, finalPath.value(), record.outputSize,
                message.empty() ? L"" : L" - " + message);
    postFinished(MuxEvent::Outcome::Done, std::move(message), finalPath.value());
}

} // namespace hh
