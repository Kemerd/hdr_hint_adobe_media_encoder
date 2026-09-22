// ---------------------------------------------------------------------------
// FileReadiness.h - decides when an AME output is complete and safe to read.
// ---------------------------------------------------------------------------
#pragma once

#include "core/EngineEvents.h"
#include "platform/Event.h"
#include "platform/Win.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace hh {

struct ReadinessConfig {
    int probeIntervalMs = 1500;
    int maxIntervalMs = 5000;
    int stableSeconds = 2;
    uint64_t minOutputBytes = 1024;
    bool checkStructure = true;
    bool requireConfirmation = true;      ///< wait for log/CEP confirmation
    int confirmTimeoutS = 20;             ///< ... but not longer than this after first Ready
    int encodingTimeoutHours = 12;
    int missingGraceS = 30;
};

/**
 * @brief Scheduled probes: exists -> deny-write open -> stable size ->
 *        no sidecars -> MP4 box chain -> confirmation (or timeout).
 */
class FileReadiness {
public:
    explicit FileReadiness(IEngineSink& sink);
    ~FileReadiness();
    FileReadiness(const FileReadiness&) = delete;
    FileReadiness& operator=(const FileReadiness&) = delete;

    void start(const ReadinessConfig& config);
    void stop();

    /**
     * @brief Starts probing a job's output.
     * @param requireConfirmation false for manual/one-shot jobs (no log expected)
     */
    void schedule(JobId jobId, const std::wstring& path, bool requireConfirmation);
    /// Log/CEP reported success: the probe may report Ready without waiting.
    void confirm(JobId jobId);
    /// Log reported failure: the probe stops and reports Failed with the reason.
    void fail(JobId jobId, const std::wstring& reason);
    void cancel(JobId jobId);
    /// Sidecar bytes seen for the job (used to estimate finalize progress).
    void setSidecarBytes(JobId jobId, uint64_t bytes);

private:
    struct Probe {
        std::wstring path;
        bool requireConfirmation = true;
        bool confirmed = false;
        bool failed = false;
        std::wstring failReason;
        uint64_t startMs = 0;
        uint64_t dueMs = 0;
        uint64_t intervalMs = 0;
        uint64_t lastSize = 0;
        uint64_t lastSizeChangeMs = 0;
        uint64_t firstReadyMs = 0;
        uint64_t lastSeenMs = 0;
        uint64_t sidecarBytes = 0;
        int attempts = 0;
        std::wstring lastPhase;
    };

    void threadMain();
    void runProbe(JobId id, Probe& p, uint64_t nowMs);

    IEngineSink& sink_;
    ReadinessConfig config_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    platform::Event stopEvent_;
    platform::Event wakeEvent_;
    std::mutex mutex_;
    std::map<JobId, Probe> probes_;
};

} // namespace hh
