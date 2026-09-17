// ---------------------------------------------------------------------------
// JobStore.h - in-memory job table with JSON persistence.
// Only ever touched from the UI thread.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "core/JobModel.h"
#include "platform/Win.h"

#include <map>
#include <string>
#include <vector>

namespace hh {

class JobStore {
public:
    /// Creates a job with a fresh id (generation 1 unless a terminal job with the key exists).
    Job& create(const std::wstring& key, const std::wstring& outputPath, JobSource source);

    [[nodiscard]] Job* find(JobId id) noexcept;
    [[nodiscard]] const Job* find(JobId id) const noexcept;
    /// The newest job with this key (any state), or nullptr.
    [[nodiscard]] Job* findByKey(std::wstring_view key) noexcept;
    /// The newest *active* (non-terminal) job with this key, or nullptr.
    [[nodiscard]] Job* findActiveByKey(std::wstring_view key) noexcept;
    /// Active job whose output stem matches (for sidecar events), or nullptr.
    [[nodiscard]] Job* findActiveByStem(std::wstring_view folder, std::wstring_view stem) noexcept;

    /// All jobs, newest first.
    [[nodiscard]] std::vector<Job*> all();
    [[nodiscard]] std::vector<const Job*> all() const;
    [[nodiscard]] size_t size() const noexcept { return jobs_.size(); }

    bool remove(JobId id);
    /// Drops terminal jobs beyond @p historyMax (oldest first).
    void trim(size_t historyMax);

    /// Marks changed (for debounced saves).
    void touch() noexcept { dirty_ = true; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    Result<void> load(const std::wstring& path);
    Result<void> save(const std::wstring& path);

    /// JSON (de)serialisation of a single job (UTF-8), exposed for IPC.
    static std::string toJson(const Job& job);
    static bool fromJson(std::string_view json, Job& job);

private:
    std::map<JobId, Job> jobs_;
    JobId nextId_ = 1;
    bool dirty_ = false;
};

} // namespace hh
