// ---------------------------------------------------------------------------
// AmeLogTailer.h - worker thread that follows AME's encoding logs.
// ---------------------------------------------------------------------------
#pragma once

#include "core/AmeLogLocator.h"
#include "core/AmeLogParser.h"
#include "core/EngineEvents.h"
#include "platform/DirectoryWatch.h"
#include "platform/FileIo.h"
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

struct TailerConfig {
    std::vector<std::wstring> logOverrides;
    int pollMs = 2000;
    bool preferDayFirstDates = false;
    int recentFoldersCount = 10;
    std::wstring statePath;          ///< %LOCALAPPDATA%\HdrHint\state.json (offsets)
    int rediscoverSeconds = 60;
};

/**
 * @brief Tails every discovered AMEEncodingLog.txt / AMEEncodingErrorLog.txt.
 *
 * On first sight of a file the whole content is parsed in *seed* mode (history
 * only); afterwards only appended bytes are parsed and reported live.
 */
class AmeLogTailer {
public:
    explicit AmeLogTailer(IEngineSink& sink);
    ~AmeLogTailer();
    AmeLogTailer(const AmeLogTailer&) = delete;
    AmeLogTailer& operator=(const AmeLogTailer&) = delete;

    void start(const TailerConfig& config);
    void stop();
    /// Asks the thread to re-run discovery now.
    void rescan();
    /// True while the thread runs.
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    /// Per-file tail state (persisted).
    struct TailState {
        uint64_t offset = 0;
        platform::FileIdentity identity;
        bool utf16 = true;
        bool seeded = false;          ///< first full parse done
        std::vector<uint8_t> carry;   ///< partial line / odd byte carried between reads
        std::unique_ptr<AmeLogParser> parser;
        uint64_t appStartUtc = 0;
    };

    void threadMain();
    void discover();
    void readFile(const LogCandidate& candidate, TailState& state, bool forceSeed);
    void loadState();
    void saveState();
    void emitFolders();

    IEngineSink& sink_;
    TailerConfig config_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    platform::UniqueHandle stopEvent_;
    platform::UniqueHandle wakeEvent_;
    std::atomic<bool> rescanRequested_{false};

    std::vector<LogCandidate> candidates_;
    std::vector<std::unique_ptr<platform::DirectoryWatch>> watches_;
    std::map<std::wstring, TailState> states_;        ///< keyed by lower-cased path
    std::vector<std::wstring> recentFolders_;         ///< newest first, capped
    uint64_t appStartUtc_ = 0;
    uint64_t lastDiscoverMs_ = 0;
    uint64_t lastStateSaveMs_ = 0;
    bool stateDirty_ = false;
};

} // namespace hh
