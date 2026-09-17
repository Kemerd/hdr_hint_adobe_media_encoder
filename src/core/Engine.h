// ---------------------------------------------------------------------------
// Engine.h - the orchestrator. Owns the workers and the job state machine.
//
// Threading contract: every public method is called on the UI thread; worker
// events arrive through EngineEventQueue and are applied in onEventMessage().
// ---------------------------------------------------------------------------
#pragma once

#include "core/AmeLogTailer.h"
#include "core/EngineEvents.h"
#include "core/EventQueue.h"
#include "core/Expected.h"
#include "core/FileReadiness.h"
#include "core/FolderWatcher.h"
#include "core/HdrPresets.h"
#include "core/IpcServer.h"
#include "core/JobModel.h"
#include "core/JobStore.h"
#include "core/MkvmergeLocator.h"
#include "core/MuxWorker.h"
#include "core/Settings.h"
#include "platform/Win.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hh {

/// What the UI shows in the status dot / footer.
struct LinkState {
    bool logFound = false;
    std::wstring logPath;
    bool panelConnected = false;
    bool queueRunning = false;
    int watchFolders = 0;
    int activeJobs = 0;
    std::wstring mkvmergeVersion;   ///< "v82.0" or "not found"
    bool mkvmergeOk = false;
};

/// A toast the UI should show.
struct ToastRequest {
    enum class Tone { Info, Success, Warning, Error };
    Tone tone = Tone::Info;
    std::wstring text;
    JobId jobId = 0;               ///< optional: "Reveal" action target
    std::wstring actionLabel;      ///< optional button
};

/**
 * @brief The engine. Construct, wire callbacks, start(), pump onEventMessage().
 */
class Engine {
public:
    Engine(Settings& settings, PresetRegistry& presets);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Starts the workers. Events are kicked to (hwnd, message).
    Result<void> start(HWND eventTarget, UINT eventMessage);
    /// Stops the workers (cancels probes; a running mux is cancelled unless @p waitForMux).
    void stop(bool waitForMux);
    /// Drains and applies queued worker events. Call on the kick message.
    void onEventMessage();
    /// Periodic housekeeping (debounced saves, timeouts). Call ~1 Hz from a UI timer.
    void tick();

    // ---- queries ------------------------------------------------------------
    [[nodiscard]] std::vector<Job> jobs() const;                 ///< newest first (copies)
    [[nodiscard]] std::optional<Job> job(JobId id) const;
    [[nodiscard]] LinkState linkState() const;
    [[nodiscard]] const MkvmergeInfo& mkvmergeInfo() const noexcept { return mkvmerge_; }
    [[nodiscard]] std::vector<std::wstring> availableLuts() const;   ///< .cube files in the LUT folder + recents
    [[nodiscard]] const PresetRegistry& presets() const noexcept { return presets_; }
    [[nodiscard]] Settings& settings() noexcept { return settings_; }
    [[nodiscard]] bool muxRunning() const;
    [[nodiscard]] std::vector<std::wstring> watchFolders() const;

    // ---- commands -----------------------------------------------------------
    void runJob(JobId id);                       ///< Ready/Held/Failed/Skipped -> dispatch (re-probes when needed)
    void holdJob(JobId id);                      ///< Ready -> Held (or sets the hold flag while Encoding)
    void resumeJob(JobId id);                    ///< clears hold; dispatches when Ready
    void removeJob(JobId id);                    ///< cancels and deletes the row
    void retryJob(JobId id) { runJob(id); }
    void setJobOverrides(JobId id, const JobOverrides& overrides);
    void revealJob(JobId id, bool hintFile);
    void recycleJob(JobId id);                   ///< user-triggered Recycle Bin move
    void addManualFile(const std::wstring& path);///< enqueue a file by hand
    void setAutoProcess(bool on);
    void addWatchFolder(const std::wstring& folder);
    void removeWatchFolder(const std::wstring& folder);
    void reprobeMkvmerge();
    /// Call after the UI changed settings; re-applies what the workers need.
    void settingsChanged();
    void saveNow();

    // ---- IPC (panel) --------------------------------------------------------
    void ipcSend(uint32_t connectionId, const std::string& jsonLine);
    void ipcBroadcast(const std::string& jsonLine);
    /// True when at least one panel is connected.
    [[nodiscard]] bool panelConnected() const noexcept { return panelConnections_ > 0; }

    // ---- callbacks (UI thread) ---------------------------------------------
    std::function<void()> onJobsChanged;
    std::function<void(const ToastRequest&)> onToast;
    std::function<void()> onLinkStateChanged;
    /// Panel messages the engine does not own (hello, panelBounds, command, lifecycle).
    std::function<void(uint32_t connectionId, const std::string& kind, const std::string& jsonLine)> onPanelMessage;
    std::function<void(uint32_t connectionId, bool connected)> onPanelConnection;

    // ---- headless -----------------------------------------------------------
    /**
     * @brief Processes one file synchronously (used by --process). No workers needed.
     * @param presetId  empty = by transfer (probe/identify) with defaults
     * @param lutPath   empty = default for the transfer; "none" = no attachment
     */
    Result<std::wstring> processOneShot(const std::wstring& path, const std::wstring& presetId,
                                        const std::wstring& lutPath, const std::function<void(float)>& onProgress);

    /// Resolves the effective plan for a job (defaults + overrides). Public for the UI preview.
    [[nodiscard]] EffectivePlan resolvePlan(const Job& job) const;

private:
    // event application
    void apply(LogItemEvent& e);
    void apply(LogQueueEvent& e);
    void apply(LogFoldersEvent& e);
    void apply(LogStatusEvent& e);
    void apply(SidecarEvent& e);
    void apply(OutputEvent& e);
    void apply(FolderAvailabilityEvent& e);
    void apply(ProbeEvent& e);
    void apply(MuxEvent& e);
    void apply(IpcMessageEvent& e);
    void apply(IpcClientEvent& e);
    void apply(WorkerNoteEvent& e);
    void applyAmeMessage(uint32_t conn, const std::string& type, const std::string& jsonLine);

    // state machine helpers
    Job& ensureJob(const std::wstring& outputPath, JobSource source, bool& created);
    void transition(Job& job, JobState state, std::wstring reason = {});
    void onReady(Job& job);
    void dispatch(Job& job);
    /// True when the trigger that discovered @p job may process it unattended.
    [[nodiscard]] bool autoProcessAllowed(const Job& job) const;
    void finishDone(Job& job, MuxEvent& e);
    void maybeRecycle(Job& job);
    void inferTransfer(Job& job, TransferKind kind, const wchar_t* source);
    void registerFolder(const std::wstring& folder);
    void refreshLuts();
    void rememberLut(const std::wstring& lutPath);
    void cepStartedPathsEraseFor(JobId id);
    void notifyJobs();
    void notifyLink();
    void toast(ToastRequest::Tone tone, std::wstring text, JobId jobId = 0, std::wstring action = {});
    MuxPlan makeMuxPlan(const Job& job, const EffectivePlan& plan) const;
    Result<void> validatePlan(const Job& job, EffectivePlan& plan) const;
    void applySettingsToWorkers();
    void updateRegistry();
    void catchUp(const AmeItemRecord& rec);

    Settings& settings_;
    PresetRegistry& presets_;
    EngineEventQueue queue_;
    JobStore store_;
    MkvmergeInfo mkvmerge_;
    std::wstring ffprobePath_;

    std::unique_ptr<AmeLogTailer> tailer_;
    std::unique_ptr<FolderWatcher> watcher_;
    std::unique_ptr<FileReadiness> readiness_;
    std::unique_ptr<MuxWorker> mux_;
    std::unique_ptr<IpcServer> ipc_;

    LinkState link_;
    int panelConnections_ = 0;
    bool started_ = false;
    uint64_t lastSaveMs_ = 0;
    std::wstring jobsPath_;
    std::vector<std::wstring> luts_;
    std::vector<std::wstring> knownFolders_;
    std::map<std::string, std::wstring> cepStartedPaths_;   ///< "group,item" -> outputFilePath (for batchItemStatus pairing)
};

} // namespace hh
