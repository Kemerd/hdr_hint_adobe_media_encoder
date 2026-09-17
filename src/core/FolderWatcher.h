// ---------------------------------------------------------------------------
// FolderWatcher.h - worker thread watching render folders for AME activity.
// ---------------------------------------------------------------------------
#pragma once

#include "core/EngineEvents.h"
#include "platform/DirectoryWatch.h"
#include "platform/Handle.h"
#include "platform/Win.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hh {

struct WatcherConfig {
    std::vector<std::wstring> extensions{L".mp4", L".mov", L".m4v"};
    std::wstring suffix = L"_REC709_HINT";
    int debounceMs = 500;
    bool sidecarDetection = true;
    int catchUpMinutes = 15;          ///< initial scan: files newer than this are reported
};

/**
 * @brief Watches a set of folders (non-recursive) and reports sidecars and
 *        candidate outputs. Folder list changes are applied on the thread.
 */
class FolderWatcher {
public:
    explicit FolderWatcher(IEngineSink& sink);
    ~FolderWatcher();
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    void start(const WatcherConfig& config);
    void stop();

    /// Replaces the whole folder set (duplicates and non-directories are ignored).
    void setFolders(const std::vector<std::wstring>& folders);
    void addFolder(const std::wstring& folder);
    void removeFolder(const std::wstring& folder);
    /// Current folder set (normalised).
    [[nodiscard]] std::vector<std::wstring> folders() const;
    /// Updates the suffix/extensions without restarting.
    void updateConfig(const WatcherConfig& config);

private:
    struct Watched {
        std::wstring folder;
        std::unique_ptr<platform::DirectoryWatch> watch;
        bool available = false;
        uint64_t nextRetryMs = 0;
        std::map<std::wstring, uint64_t> pending;      ///< name -> due time (debounce)
        std::map<std::wstring, DWORD> pendingAction;   ///< name -> last action
        std::map<std::wstring, uint64_t> snapshot;     ///< name -> size (for rescans)
    };

    void threadMain();
    void applyPendingCommands();
    void openWatch(Watched& w, bool initialScan);
    void processChanges(Watched& w);
    void flushDue(Watched& w, uint64_t nowMs);
    void classify(Watched& w, const std::wstring& name, DWORD action);
    void initialScan(Watched& w);
    void rescanDiff(Watched& w);

    IEngineSink& sink_;
    WatcherConfig config_;
    mutable std::mutex mutex_;                 ///< guards commands_/configPending_
    std::vector<std::pair<int, std::wstring>> commands_;   ///< (0=set-reset,1=add,2=remove, folder)
    std::vector<std::wstring> desiredFolders_;
    bool configPending_ = false;
    WatcherConfig pendingConfig_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    platform::UniqueHandle stopEvent_;
    platform::UniqueHandle wakeEvent_;
    std::vector<std::unique_ptr<Watched>> watched_;
};

} // namespace hh
