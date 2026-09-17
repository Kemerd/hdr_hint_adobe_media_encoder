// ---------------------------------------------------------------------------
// MockViewModels.h - canned view-models for screenshots and UI tests.
//
// Every interface from ui/screens/ViewModels.h gets an implementation whose
// data lives in memory: one queue row per job state, realistic names, a full
// settings page and a linked-to-AME status. Setters mutate the canned data
// and fire onChanged exactly like the real view-models, so the screens can
// be exercised end to end without an engine, a log file or mkvmerge.
// ---------------------------------------------------------------------------
#pragma once

#include "core/JobModel.h"
#include "platform/Win.h"
#include "ui/screens/ViewModels.h"

#include <string>
#include <vector>

namespace hh::ui {

// ===========================================================================
// Queue
// ===========================================================================

/**
 * @brief In-memory queue with one job per state.
 */
class MockQueueViewModel final : public IQueueViewModel {
public:
    /// Starts with sampleJobs().
    MockQueueViewModel();
    ~MockQueueViewModel() override = default;

    [[nodiscard]] std::vector<JobView> jobs() const override;
    [[nodiscard]] std::vector<Choice> presetChoices(TransferKind t) const override;
    [[nodiscard]] std::vector<Choice> lutChoices() const override;
    void run(JobId id) override;
    void hold(JobId id) override;
    void resume(JobId id) override;
    void remove(JobId id) override;
    void reveal(JobId id, bool hintFile) override;
    void setPreset(JobId id, const std::wstring& presetId) override;
    void setLut(JobId id, const std::wstring& lutPath) override;
    void setAttachLut(JobId id, bool attach) override;
    void copyCommand(JobId id) override;
    [[nodiscard]] bool autoProcess() const override;
    void setAutoProcess(bool on) override;
    [[nodiscard]] std::wstring footerSummary() const override;
    void addFiles(const std::vector<std::wstring>& paths) override;

    // ---- harness helpers --------------------------------------------------

    /// Replaces every row (derived fields are recomputed) and fires onChanged.
    void setJobs(std::vector<JobView> rows);
    /// The rows as stored (for assertions).
    [[nodiscard]] const std::vector<JobView>& rows() const noexcept { return jobs_; }
    /// Last row id passed to reveal(), 0 when none (for assertions).
    [[nodiscard]] JobId lastRevealed() const noexcept { return lastRevealed_; }

    /// The canned queue: one row per JobState, PQ / HLG / SDR mixed.
    [[nodiscard]] static std::vector<JobView> sampleJobs();
    /**
     * @brief Builds one realistic row.
     * @param id        row id
     * @param state     job state (progress / reason are filled to match)
     * @param transfer  colour transfer (drives subtitle, preset and LUT)
     * @param name      file name ("Episode_48_Drilling_Flap_Holes.mp4")
     * @param folder    folder of the export (hint goes next to it)
     */
    [[nodiscard]] static JobView sampleJob(JobId id, JobState state, TransferKind transfer,
                                           const std::wstring& name, const std::wstring& folder);
    /// Recomputes the derived fields (labels, flags, command) from state/progress/plan.
    static void refreshRow(JobView& row);

private:
    /// Row lookup; null when unknown (logged).
    [[nodiscard]] JobView* find(JobId id, const wchar_t* who);
    void notify();

    std::vector<JobView> jobs_;
    bool autoProcess_ = true;
    JobId nextId_ = 1000;
    JobId lastRevealed_ = 0;
};

// ===========================================================================
// Settings
// ===========================================================================

/**
 * @brief In-memory settings page.
 */
class MockSettingsViewModel final : public ISettingsViewModel {
public:
    /// Starts with sampleView().
    MockSettingsViewModel();
    ~MockSettingsViewModel() override = default;

    [[nodiscard]] SettingsView view() const override;
    [[nodiscard]] std::vector<Choice> presetChoices(TransferKind t) const override;
    [[nodiscard]] std::vector<Choice> lutChoices() const override;
    void setMkvmergePath(const std::wstring& path) override;
    void browseMkvmerge() override;
    void setLutFolder(const std::wstring& path) override;
    void browseLutFolder() override;
    void setDefaultLut(TransferKind t, const std::wstring& path) override;
    void setDefaultPreset(TransferKind t, const std::wstring& presetId) override;
    void setSuffix(const std::wstring& suffix) override;
    void setAttachLutByDefault(bool on) override;
    void addWatchFolder() override;
    void removeWatchFolder(const std::wstring& folder) override;
    void setAutoProcess(bool on) override;
    void setRecycleOriginal(bool on) override;
    void setDockInsideAme(bool on) override;
    void setAlwaysOnTop(bool on) override;
    void setMinimizeToTray(bool on) override;
    void setStartMinimized(bool on) override;
    void setStartWithWindows(bool on) override;
    void setQuitWithAme(bool on) override;
    void setAppearance(int mode) override;
    void setAccent(int mode) override;
    void setReduceTransparency(bool on) override;
    void installPanel() override;
    void openLogFolder() override;
    void reprobeMkvmerge() override;

    // ---- harness helpers --------------------------------------------------

    /// Replaces the whole page and fires onChanged.
    void setView(SettingsView view);
    /// The canned page: mkvmerge found, LUTs configured, two watch folders.
    [[nodiscard]] static SettingsView sampleView();

private:
    /// Recomputes the status lines (mkvmerge, about) from the path fields.
    void refreshStatus();
    void notify();

    SettingsView view_;
    int addedFolders_ = 0;
};

// ===========================================================================
// Link
// ===========================================================================

/**
 * @brief In-memory AME link state (panel linked, log found).
 */
class MockLinkViewModel final : public ILinkViewModel {
public:
    /// Starts linked: panel connected, log found, queue idle, floating.
    MockLinkViewModel();
    ~MockLinkViewModel() override = default;

    [[nodiscard]] LinkView link() const override;
    void toggleDock() override;

    /// Replaces the state (an empty tooltip is derived from the flags) and fires onChanged.
    void setLink(LinkView link);

private:
    void notify();

    LinkView link_;
};

// ===========================================================================
// Guide
// ===========================================================================

/**
 * @brief Guide tab over the real markdown (resource / file / built-in) with a
 *        clipboard that is observable from tests.
 */
class MockGuideViewModel final : public IGuideViewModel {
public:
    /// @param settings optional settings source for a richer summary (may be null)
    explicit MockGuideViewModel(const ISettingsViewModel* settings = nullptr);
    ~MockGuideViewModel() override = default;

    [[nodiscard]] std::wstring markdown() const override;
    [[nodiscard]] std::wstring summaryText() const override;
    void copySummary() override;
    /// Records the request; never launches anything from the harness.
    void openGuideFile() override;

    /// Text of the last copySummary() (for assertions).
    [[nodiscard]] const std::wstring& lastCopied() const noexcept { return lastCopied_; }
    /// How many times openGuideFile() was called.
    [[nodiscard]] int openRequests() const noexcept { return openRequests_; }

private:
    const ISettingsViewModel* settings_ = nullptr;
    mutable std::wstring markdownCache_;
    std::wstring lastCopied_;
    int openRequests_ = 0;
};

// ===========================================================================
// Scripted scenarios
// ===========================================================================

/**
 * @brief Puts the mock queue into a scripted state for screenshots.
 * @param vm        the queue
 * @param scenario  0 = full queue (every state), 1 = empty queue, 2 = one done job;
 *                  anything else falls back to 0
 */
void mockApplyState(MockQueueViewModel& vm, int scenario);

} // namespace hh::ui
