// ---------------------------------------------------------------------------
// EngineEvents.h - messages from worker threads to the Engine (UI thread).
// ---------------------------------------------------------------------------
#pragma once

#include "core/AmeLogParser.h"
#include "core/JobModel.h"
#include "platform/Win.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace hh {

/// A block finished in the AME log. seed=true means "history", not a live event.
struct LogItemEvent {
    AmeItemRecord record;
    bool seed = false;
    bool fromErrorLog = false;
    std::wstring logPath;
};

/// Queue Started/Stopped/Paused (deduplicated by the tailer).
struct LogQueueEvent {
    AmeQueueEvent event;
    bool seed = false;
};

/// Output folders learned from the log history (for auto-watching).
struct LogFoldersEvent {
    std::vector<std::wstring> folders;
};

/// The tailer found (or lost) log files.
struct LogStatusEvent {
    std::wstring primaryLogPath;   ///< empty when none found
    int candidateCount = 0;
};

/// An AME sidecar appeared / grew / vanished in a watched folder.
struct SidecarEvent {
    std::wstring folder;
    std::wstring stem;
    DWORD pid = 0;
    DWORD tid = 0;
    std::wstring ext;
    uint64_t size = 0;
    bool gone = false;
};

/// A candidate output file changed in a watched folder.
struct OutputEvent {
    enum class Action { Added, Modified, Removed };
    std::wstring path;
    Action action = Action::Added;
    uint64_t size = 0;
    bool fromInitialScan = false;
};

/// A watched folder became (un)available.
struct FolderAvailabilityEvent {
    std::wstring folder;
    bool available = true;
    std::wstring message;
};

/// Result of a readiness probe.
struct ProbeEvent {
    enum class Outcome { Writing, Ready, Missing, Failed };
    JobId jobId = 0;
    Outcome outcome = Outcome::Writing;
    std::wstring phase;          ///< "Finalizing", "Waiting for AME to release file", ...
    float progress = -1.0f;      ///< -1 = unknown
    uint64_t size = 0;
    std::wstring message;
    SourceStamp stamp;           ///< filled when Ready
    bool mp4Complete = false;
    double durationSec = 0.0;    ///< from moov when parsed
    uint64_t mdatPayload = 0;
};

/// Progress / completion of a mux.
struct MuxEvent {
    enum class Kind { Started, Progress, Finished };
    enum class Outcome { Done, Failed, Cancelled };
    JobId jobId = 0;
    Kind kind = Kind::Progress;
    Outcome outcome = Outcome::Done;
    float progress = 0.0f;
    std::wstring phase;          ///< "Identifying", "Muxing", "Verifying"
    std::wstring hintPath;
    std::wstring message;
    MuxRecord record;
    bool inbandHdr10 = false;
    TransferKind probedTransfer = TransferKind::Unknown;
};

/// One JSON line from a pipe client.
struct IpcMessageEvent {
    uint32_t connectionId = 0;
    std::string jsonLine;        ///< UTF-8
};

/// A pipe client connected / disconnected.
struct IpcClientEvent {
    uint32_t connectionId = 0;
    bool connected = true;
};

/// Free-form note from a worker for the UI log pane.
struct WorkerNoteEvent {
    std::wstring component;
    std::wstring text;
    bool isError = false;
};

using EngineEvent = std::variant<
    LogItemEvent, LogQueueEvent, LogFoldersEvent, LogStatusEvent,
    SidecarEvent, OutputEvent, FolderAvailabilityEvent,
    ProbeEvent, MuxEvent,
    IpcMessageEvent, IpcClientEvent,
    WorkerNoteEvent>;

/**
 * @brief Where workers post their events (thread-safe).
 */
class IEngineSink {
public:
    virtual ~IEngineSink() = default;
    virtual void post(EngineEvent&& event) = 0;
};

} // namespace hh
