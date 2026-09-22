// ---------------------------------------------------------------------------
// MuxWorker.h - the one thread that runs mkvmerge jobs sequentially.
// ---------------------------------------------------------------------------
#pragma once

#include "core/EngineEvents.h"
#include "core/MkvmergeRunner.h"
#include "platform/Event.h"
#include "platform/Win.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace hh {

struct MuxRequest {
    JobId jobId = 0;
    MuxPlan plan;
    std::wstring ffprobePath;     ///< optional: probe the input for transfer / in-band SEI
    bool probeInput = true;
};

/**
 * @brief identify -> (probe) -> mux -> verify -> finalize, one job at a time.
 */
class MuxWorker {
public:
    explicit MuxWorker(IEngineSink& sink);
    ~MuxWorker();
    MuxWorker(const MuxWorker&) = delete;
    MuxWorker& operator=(const MuxWorker&) = delete;

    void start();
    void stop();

    void enqueue(MuxRequest request);
    /// Cancels a queued or running job.
    void cancel(JobId jobId);
    /// True while a job is running.
    [[nodiscard]] bool busy() const noexcept { return busy_.load(); }
    /// Id of the running job (0 = none).
    [[nodiscard]] JobId currentJob() const noexcept { return current_.load(); }

private:
    void threadMain();
    void process(MuxRequest& request);

    IEngineSink& sink_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> busy_{false};
    std::atomic<JobId> current_{0};
    platform::Event stopEvent_;
    platform::Event wakeEvent_;
    platform::Event cancelEvent_;      ///< manual-reset; set to abort the current job
    std::mutex mutex_;
    std::deque<MuxRequest> queue_;
};

} // namespace hh
