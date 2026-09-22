// ---------------------------------------------------------------------------
// AmeLogTailer.cpp - worker thread that follows AME's encoding logs.
//
// One thread owns everything: the candidate list, the directory watches,
// the per-file tail states and the parsers. The UI thread only ever calls
// start() / stop() / rescan(), which talk to the worker through two
// manual-reset events and one atomic flag, so nothing here needs a lock.
//
// Reading model
//   * The file is opened fresh for every read (share R/W/D) and closed
//     before the next wait, so AME can rotate or recreate it at any time.
//   * Bytes are appended to a per-file carry buffer, complete lines are fed
//     to the parser straight away, and only the unterminated tail stays in
//     the carry. AME writes "\r\n" *before* every line, which means the last
//     line of every append sits in the carry until the next append lands.
//   * The first pass over a file is a *seed* (history); everything parsed
//     afterwards is live. A file that was recreated after the app started
//     is seeded again, but blocks stamped after the app start are live.
//
// Persistence
//   Offsets and identities go to state.json so a restart does not replay
//   the whole history. The offset that is written is the start of the carry,
//   never past it, so the unterminated last line is re-read after a restart
//   instead of being lost.
// ---------------------------------------------------------------------------
#include "core/AmeLogTailer.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace hh {

namespace {

using json = nlohmann::json;

/// Component tag for every log line and worker note from this file.
constexpr const wchar_t* kLog = L"Tailer";

/// AME's file names, matched case-insensitively against change records.
constexpr std::wstring_view kMainLogName = L"AMEEncodingLog.txt";
constexpr std::wstring_view kErrorLogName = L"AMEEncodingErrorLog.txt";

/// What we ask ReadDirectoryChangesW to report for a log folder.
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;

/// Read granularity; large enough that a seed of a multi-MB log is a handful of calls.
constexpr size_t kChunkBytes = 64 * 1024;
/// Bytes sniffed at offset 0 to guess the encoding when there is no BOM.
constexpr size_t kProbeBytes = 64;
/// A carry that grows this large without a newline is flushed as one line.
constexpr size_t kMaxCarryBytes = 1024 * 1024;
/// state.json is rewritten at most this often while dirty.
constexpr uint64_t kStateSaveIntervalMs = 1000;
/// Worker notes about the same path are throttled to one per minute.
constexpr uint64_t kNoteIntervalMs = 60 * 1000;
/// Upper bound on the state file we are willing to parse.
constexpr uint64_t kMaxStateBytes = 4ull * 1024 * 1024;
/// Upper bound on persisted entries (stale overrides could pile up otherwise).
constexpr size_t kMaxStateEntries = 256;
/// waitAny() takes 64 handles: stop + wake + this many watches.
constexpr size_t kMaxWatches = platform::kMaxWaitHandles - 2;

/// Clamp ranges for the configuration.
constexpr int kMinPollMs = 250;
constexpr int kMaxPollMs = 60 * 1000;
constexpr int kMinRediscoverSeconds = 5;
constexpr int kMaxRediscoverSeconds = 3600;
constexpr int kMaxRecentFolders = 100;

/// The replacement character toWide() emits for invalid UTF-8.
constexpr wchar_t kReplacementChar = static_cast<wchar_t>(0xFFFD);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/**
 * @brief Brings the configuration into the ranges the loop can live with.
 */
[[nodiscard]] TailerConfig sanitize(const TailerConfig& in) {
    TailerConfig c = in;
    c.pollMs = std::clamp(c.pollMs, kMinPollMs, kMaxPollMs);
    c.rediscoverSeconds = std::clamp(c.rediscoverSeconds, kMinRediscoverSeconds, kMaxRediscoverSeconds);
    c.recentFoldersCount = std::clamp(c.recentFoldersCount, 0, kMaxRecentFolders);
    return c;
}

/**
 * @brief Map key for a log path: lower-cased so "c:\..." and "C:\..." meet.
 */
[[nodiscard]] std::wstring stateKeyFor(std::wstring_view logPath) {
    return platform::toLowerInvariant(logPath);
}

/**
 * @brief True for the two file names the tailer cares about.
 */
[[nodiscard]] bool isLogFileName(std::wstring_view name) {
    return platform::iequals(name, kMainLogName) || platform::iequals(name, kErrorLogName);
}

/**
 * @brief Errors that mean "not now" rather than "something is wrong".
 */
[[nodiscard]] bool isBenignError(DWORD err) noexcept {
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND || err == ERROR_SHARING_VIOLATION ||
           err == ERROR_LOCK_VIOLATION;
}

/**
 * @brief "Is the file on disk still the log we were tailing?"
 *
 * With stable file ids this is FileIdentity::sameFileAs. Without them (exFAT,
 * some network shares) that check also compares sizes, which would flag every
 * append as a recreation and replay the whole log; there the creation stamp
 * plus "not shorter than what we already consumed" has to do.
 */
[[nodiscard]] bool looksLikeSameLog(const platform::FileIdentity& stored, const platform::FileIdentity& live,
                                    uint64_t consumedOffset) noexcept {
    if (!stored.valid || !live.valid) {
        return false;
    }
    if (stored.sameFileAs(live)) {
        return true;
    }
    if (stored.hasFileId || live.hasFileId) {
        return false;
    }
    return stored.creationUtc != 0 && stored.creationUtc == live.creationUtc && live.size >= consumedOffset;
}

/**
 * @brief True when the path is in the list (ordinal, case-insensitive).
 */
[[nodiscard]] bool containsFolder(const std::vector<std::wstring>& list, std::wstring_view folder) {
    for (const std::wstring& f : list) {
        if (platform::iequals(f, folder)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Moves @p folder to the front of the recent list (newest first),
 *        dropping an older duplicate and trimming to @p cap entries.
 * @return true when the list changed
 */
bool rememberFolder(std::vector<std::wstring>& list, std::wstring folder, size_t cap) {
    if (cap == 0 || folder.empty()) {
        return false;
    }
    // Already the most recent entry: nothing to do.
    if (!list.empty() && platform::iequals(list.front(), folder)) {
        return false;
    }
    // Remove an older mention so the folder is listed once.
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (platform::iequals(*it, folder)) {
            list.erase(it);
            break;
        }
    }
    list.insert(list.begin(), std::move(folder));
    if (list.size() > cap) {
        list.resize(cap);
    }
    return true;
}

/**
 * @brief Posts a WorkerNoteEvent, throttled to one per path per minute.
 *
 * The throttle memory lives at file scope so it survives stop()/start()
 * cycles (the Engine builds a fresh tailer on every start).
 */
void postNote(IEngineSink& sink, std::wstring_view pathKey, std::wstring text, bool isError) {
    static std::mutex s_mutex;
    static std::map<std::wstring, uint64_t> s_lastNoteMs;

    const uint64_t now = platform::nowMonotonicMs();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const std::wstring key = platform::toLowerInvariant(pathKey);
        const auto it = s_lastNoteMs.find(key);
        if (it != s_lastNoteMs.end() && now >= it->second && (now - it->second) < kNoteIntervalMs) {
            return;
        }
        // Keep the map from growing without bound on a long session.
        if (s_lastNoteMs.size() > kMaxStateEntries) {
            s_lastNoteMs.clear();
        }
        s_lastNoteMs[key] = now;
    }

    WorkerNoteEvent ev;
    ev.component = kLog;
    ev.text = std::move(text);
    ev.isError = isError;
    sink.post(EngineEvent{std::move(ev)});
}

/**
 * @brief The user's ANSI code page (LOCALE_IDEFAULTANSICODEPAGE), cached.
 *
 * Used for log files that are neither UTF-16 nor valid UTF-8, which happens
 * with very old AME builds on non-English systems.
 */
[[nodiscard]] UINT userAnsiCodePage() {
#if defined(_WIN32)
    static const UINT s_codePage = [] {
        DWORD cp = 0;
        // LOCALE_RETURN_NUMBER writes a DWORD into the "string" buffer.
        const int n = ::GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                                        reinterpret_cast<LPWSTR>(&cp), sizeof(cp) / sizeof(wchar_t));
        if (n <= 0 || cp == 0) {
            cp = ::GetACP();
        }
        return static_cast<UINT>(cp);
    }();
    return s_codePage;
#else
    // macOS AME writes UTF-8 / UTF-16; CP_ACP is the Windows-1252 fallback
    // for logs copied over from a western Windows machine.
    return CP_ACP;
#endif
}

/**
 * @brief Decodes one narrow line: UTF-8 first, the ANSI code page when the
 *        UTF-8 attempt produced a replacement character.
 */
[[nodiscard]] std::wstring decodeNarrowLine(const uint8_t* data, size_t length) {
    if (data == nullptr || length == 0) {
        return {};
    }
    const std::string_view bytes(reinterpret_cast<const char*>(data), length);
    std::wstring wide = platform::toWide(bytes);
    if (wide.find(kReplacementChar) != std::wstring::npos) {
        wide = platform::fromCodePage(bytes, userAnsiCodePage());
    }
    return wide;
}

/**
 * @brief Feeds every complete line in a UTF-16LE carry to the parser.
 *
 * Complete means terminated by "\n"; a trailing "\r" is stripped. The bytes
 * after the last newline (plus an odd trailing byte) remain in the carry
 * untouched, which is exactly the UTF-16LE re-encoding of that tail.
 * @return number of lines fed
 */
size_t drainUtf16Lines(std::vector<uint8_t>& carry, AmeLogParser& parser, uint64_t fileClockUtc) {
    const size_t evenBytes = carry.size() & ~static_cast<size_t>(1);
    if (evenBytes < 2) {
        return 0;
    }

    // Reinterpret the even prefix as UTF-16 code units (the host is
    // little-endian). Line splitting happens on code units so the byte
    // arithmetic below holds whatever width wchar_t has on this platform;
    // each line is then decoded to wide text on its own.
    std::u16string text(evenBytes / sizeof(char16_t), u'\0');
    std::memcpy(text.data(), carry.data(), evenBytes);

    const size_t lastNl = text.rfind(u'\n');
    if (lastNl == std::u16string::npos) {
        // No newline at all: hold the bytes unless the carry has gone absurd.
        if (carry.size() > kMaxCarryBytes) {
            HH_LOG_WARN(kLog, L"{} bytes without a newline; flushing as one line", carry.size());
            parser.feedLine(platform::fromUtf16(text.data(), text.size()), fileClockUtc);
            carry.clear();
            return 1;
        }
        return 0;
    }

    // Feed every terminated line.
    size_t fed = 0;
    size_t start = 0;
    while (start <= lastNl) {
        size_t nl = text.find(u'\n', start);
        if (nl == std::u16string::npos || nl > lastNl) {
            nl = lastNl;
        }
        size_t len = nl - start;
        if (len > 0 && text[start + len - 1] == u'\r') {
            --len;
        }
        parser.feedLine(platform::fromUtf16(text.data() + start, len), fileClockUtc);
        ++fed;
        start = nl + 1;
    }

    // Keep the raw bytes of the unterminated tail (and the odd byte, if any).
    const size_t tailStart = (lastNl + 1) * sizeof(char16_t);
    if (tailStart >= carry.size()) {
        carry.clear();
    } else {
        carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(tailStart));
    }
    return fed;
}

/**
 * @brief Feeds every complete line in an ANSI/UTF-8 carry to the parser.
 *
 * Splitting on the raw 0x0A byte is safe for UTF-8 and for every ANSI code
 * page (DBCS trail bytes never fall below 0x40). The tail after the last
 * newline stays in the carry as raw bytes.
 * @return number of lines fed
 */
size_t drainNarrowLines(std::vector<uint8_t>& carry, AmeLogParser& parser, uint64_t fileClockUtc) {
    if (carry.empty()) {
        return 0;
    }

    // Locate the last newline; without one the whole buffer is a partial line.
    size_t lastNl = carry.size();
    for (size_t i = carry.size(); i > 0; --i) {
        if (carry[i - 1] == '\n') {
            lastNl = i - 1;
            break;
        }
    }
    if (lastNl == carry.size()) {
        if (carry.size() > kMaxCarryBytes) {
            HH_LOG_WARN(kLog, L"{} bytes without a newline; flushing as one line", carry.size());
            parser.feedLine(decodeNarrowLine(carry.data(), carry.size()), fileClockUtc);
            carry.clear();
            return 1;
        }
        return 0;
    }

    // Feed every terminated line.
    size_t fed = 0;
    size_t start = 0;
    while (start <= lastNl) {
        size_t nl = start;
        while (nl < lastNl && carry[nl] != '\n') {
            ++nl;
        }
        size_t len = nl - start;
        if (len > 0 && carry[start + len - 1] == '\r') {
            --len;
        }
        parser.feedLine(decodeNarrowLine(carry.data() + start, len), fileClockUtc);
        ++fed;
        start = nl + 1;
    }

    // Drop the consumed prefix; the partial tail remains.
    carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(lastNl + 1));
    return fed;
}

/**
 * @brief Human label for a parser result (log lines only).
 */
[[nodiscard]] const wchar_t* resultName(AmeItemRecord::Result r) noexcept {
    switch (r) {
    case AmeItemRecord::Result::Success:    return L"success";
    case AmeItemRecord::Result::Failed:     return L"failed";
    case AmeItemRecord::Result::Incomplete: return L"incomplete";
    case AmeItemRecord::Result::Unknown:    break;
    }
    return L"unknown";
}

// ---------------------------------------------------------------------------
// JSON helpers (never throw; missing or mistyped keys leave the output alone)
// ---------------------------------------------------------------------------

/**
 * @brief Reads an unsigned integer member; false when absent or not a number.
 */
bool getU64(const json& obj, const char* key, uint64_t& out) {
    if (key == nullptr || !obj.is_object()) {
        return false;
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return false;
    }
    if (it->is_number_unsigned()) {
        out = it->get<uint64_t>();
        return true;
    }
    // A signed value that is not negative is accepted as well.
    if (it->is_number_integer()) {
        const int64_t v = it->get<int64_t>();
        if (v < 0) {
            return false;
        }
        out = static_cast<uint64_t>(v);
        return true;
    }
    return false;
}

/**
 * @brief Reads a boolean member; false when absent or not a boolean.
 */
bool getBool(const json& obj, const char* key, bool& out) {
    if (key == nullptr || !obj.is_object()) {
        return false;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) {
        return false;
    }
    out = it->get<bool>();
    return true;
}

} // namespace

// ===========================================================================
// Construction / lifecycle
// ===========================================================================

/**
 * @brief Creates an idle tailer; nothing runs until start().
 */
AmeLogTailer::AmeLogTailer(IEngineSink& sink)
    : sink_(sink) {
}

/**
 * @brief Stops the worker (if running) and persists the offsets.
 */
AmeLogTailer::~AmeLogTailer() {
    stop();
}

/**
 * @brief Loads the persisted offsets and launches the worker thread.
 *
 * A second start() while running is ignored; the Engine builds a fresh
 * tailer per session so this only guards against a programming slip.
 */
void AmeLogTailer::start(const TailerConfig& config) {
    if (running_.load() || thread_.joinable()) {
        HH_LOG_WARN(kLog, L"start() called while already running; ignored");
        return;
    }

    // Fresh session state.
    config_ = sanitize(config);
    appStartUtc_ = platform::nowUtc();
    candidates_.clear();
    watches_.clear();
    states_.clear();
    recentFolders_.clear();
    lastDiscoverMs_ = 0;
    lastStateSaveMs_ = 0;
    stateDirty_ = false;
    rescanRequested_.store(false);

    // Offsets from the previous session (validated against the files).
    loadState();

    // Both events are manual-reset: the worker resets wake after handling it.
    stopEvent_ = platform::makeWaitableEvent(true);
    wakeEvent_ = platform::makeWaitableEvent(true);
    if (!stopEvent_ || !wakeEvent_) {
        HH_LOG_ERROR(kLog, L"CreateEventW failed: {}", Error::fromLastError(L"events").toString());
        stopEvent_.close();
        wakeEvent_.close();
        return;
    }

    // Launch. std::thread can throw when the OS refuses a thread; that must
    // not escape into the UI thread.
    running_.store(true);
    try {
        thread_ = std::thread([this] { threadMain(); });
    } catch (const std::system_error& e) {
        running_.store(false);
        HH_LOG_ERROR(kLog, L"cannot start the tailer thread: {}", platform::toWide(e.what()));
        return;
    }
    HH_LOG_INFO(kLog, L"started (poll {} ms, rediscover {} s, {} override(s), state '{}')", config_.pollMs,
                config_.rediscoverSeconds, config_.logOverrides.size(), config_.statePath);
}

/**
 * @brief Signals the worker, joins it, saves the offsets and drops the watches.
 *
 * Safe to call when nothing is running (and from the destructor).
 */
void AmeLogTailer::stop() {
    const bool hadThread = thread_.joinable();
    running_.store(false);
    if (stopEvent_) {
        stopEvent_.set();
    }

    // Join unless we are (incorrectly) being called on the worker itself.
    if (hadThread) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            HH_LOG_ERROR(kLog, L"stop() called from the tailer thread; detaching");
            thread_.detach();
        } else {
            thread_.join();
        }
    }

    // The worker is gone, so the state map is ours to persist.
    if (hadThread && stateDirty_) {
        saveState();
    }

    // Watches are normally closed by the worker; this is the safety net.
    for (auto& w : watches_) {
        if (w) {
            w->stop();
        }
    }
    watches_.clear();
    stopEvent_.close();
    wakeEvent_.close();
    if (hadThread) {
        HH_LOG_INFO(kLog, L"stopped");
    }
}

/**
 * @brief Asks the worker to re-run discovery on its next wake-up.
 */
void AmeLogTailer::rescan() {
    rescanRequested_.store(true);
    if (wakeEvent_) {
        wakeEvent_.set();
    }
}

// ===========================================================================
// Worker thread
// ===========================================================================

/**
 * @brief The worker loop: wait on stop / wake / directory events, poll the
 *        candidates when something moved (or the poll interval elapsed),
 *        rediscover periodically and persist the offsets.
 */
void AmeLogTailer::threadMain() {
    HH_LOG_INFO(kLog, L"tailer thread started");

    // ---- First discovery and the initial status -----------------------------
    discover();

    // The status event is posted once up front and whenever the primary log
    // changes afterwards; both are tracked here, on this thread only.
    std::wstring lastPrimary;
    bool statusPosted = false;
    auto publishStatus = [this, &lastPrimary, &statusPosted](bool force) {
        const LogCandidate* primary = newestMainLog(candidates_);
        const std::wstring current = (primary != nullptr) ? primary->path : std::wstring();
        if (!force && statusPosted && platform::iequals(current, lastPrimary)) {
            return;
        }
        LogStatusEvent ev;
        ev.primaryLogPath = current;
        ev.candidateCount = static_cast<int>(std::min<size_t>(candidates_.size(), 1000));
        sink_.post(EngineEvent{std::move(ev)});
        lastPrimary = current;
        statusPosted = true;
    };
    publishStatus(true);

    // Compares every candidate against its tail state and reads what changed.
    auto pollCandidates = [this]() {
        for (LogCandidate& cand : candidates_) {
            if (cand.path.empty()) {
                continue;
            }
            const std::wstring key = stateKeyFor(cand.path);
            auto it = states_.find(key);
            const bool isNew = (it == states_.end());

            // Cheap stat before anything is opened.
            const auto size = platform::fileSize(cand.path);
            if (!size) {
                // Missing is normal for an override that is yet to be created.
                if (cand.exists) {
                    HH_LOG_INFO(kLog, L"log '{}' is no longer readable: {}", cand.path, size.error().toString());
                }
                cand.exists = false;
                if (!isBenignError(size.error().win32)) {
                    postNote(sink_, key, L"Cannot read " + cand.path + L": " + size.error().toString(), true);
                }
                continue;
            }
            const auto lastWrite = platform::lastWriteUtc(cand.path);
            const uint64_t lw = lastWrite ? lastWrite.value() : 0;
            if (!cand.exists) {
                HH_LOG_INFO(kLog, L"log '{}' appeared", cand.path);
            }
            cand.exists = true;
            if (lw != 0) {
                cand.lastWriteUtc = lw;
            }

            // First sight of this file: fresh state, full seed pass.
            if (isNew) {
                TailState fresh;
                fresh.parser = std::make_unique<AmeLogParser>(config_.preferDayFirstDates);
                fresh.appStartUtc = appStartUtc_;
                it = states_.emplace(key, std::move(fresh)).first;
            }
            TailState& state = it->second;
            if (!state.parser) {
                state.parser = std::make_unique<AmeLogParser>(config_.preferDayFirstDates);
            }

            // Read when the file moved or the state has never completed a read.
            const bool changed = isNew || !state.identity.valid || size.value() != state.identity.size ||
                                 (lw != 0 && lw != state.identity.lastWriteUtc);
            if (!changed) {
                continue;
            }
            readFile(cand, state, isNew);
        }
    };

    // ---- Main loop -----------------------------------------------------------
    std::vector<platform::WaitHandle> handles;
    std::vector<platform::DirectoryWatch*> byHandle;
    handles.reserve(platform::kMaxWaitHandles);
    byHandle.reserve(platform::kMaxWaitHandles);
    const DWORD pollTimeout = static_cast<DWORD>(config_.pollMs);
    const uint64_t rediscoverMs = static_cast<uint64_t>(config_.rediscoverSeconds) * 1000ull;

    while (running_.load()) {
        // Wait list: stop, wake, then one event per live watch.
        handles.clear();
        byHandle.clear();
        handles.push_back(stopEvent_.handle());
        handles.push_back(wakeEvent_.handle());
        for (const auto& w : watches_) {
            if (!w || !w->active()) {
                continue;
            }
            const platform::WaitHandle ev = w->event();
            if (ev == platform::kInvalidWaitHandle || handles.size() >= platform::kMaxWaitHandles) {
                continue;
            }
            handles.push_back(ev);
            byHandle.push_back(w.get());
        }

        // Sleep until something fires or the poll interval elapses.
        const DWORD r = platform::waitAny(handles.data(), handles.size(), pollTimeout);
        if (r == 0) {
            break;
        }
        if (r == platform::kWaitFailed) {
            // Only reachable with a bad handle; never spin on it.
            HH_LOG_ERROR(kLog, L"wait failed: {}", Error::fromLastError(L"wait").toString());
            if (stopEvent_.wait(1000)) {
                break;
            }
            continue;
        }
        if (!running_.load()) {
            break;
        }

        // Decide whether this wake-up warrants a look at the files.
        bool pollNow = (r == platform::kWaitTimeout);
        if (r == 1) {
            wakeEvent_.reset();
            pollNow = true;
        }
        if (r != platform::kWaitTimeout) {
            // waitAny() reports the lowest signalled handle only,
            // so every watch is polled with a zero timeout.
            for (platform::DirectoryWatch* w : byHandle) {
                if (!w || !w->active()) {
                    continue;
                }
                if (!platform::isSignalled(w->event())) {
                    continue;
                }
                bool overflowed = false;
                const std::vector<platform::DirectoryChange> changes = w->drain(overflowed);
                if (overflowed) {
                    pollNow = true;
                    continue;
                }
                for (const platform::DirectoryChange& c : changes) {
                    if (isLogFileName(path::fileName(c.name))) {
                        HH_LOG_DEBUG(kLog, L"change {:#x} on '{}' in '{}'", c.action, c.name, w->directory());
                        pollNow = true;
                        break;
                    }
                }
            }
        }

        // Periodic / requested rediscovery.
        const uint64_t nowMs = platform::nowMonotonicMs();
        const bool rescanWanted = rescanRequested_.exchange(false);
        if (rescanWanted || nowMs < lastDiscoverMs_ || (nowMs - lastDiscoverMs_) >= rediscoverMs) {
            if (rescanWanted) {
                HH_LOG_INFO(kLog, L"rescan requested");
            }
            discover();
            pollNow = true;
        }

        // Read whatever moved, then tell the engine if the primary log changed.
        if (pollNow) {
            pollCandidates();
            publishStatus(false);
        }

        // Persist at most once per second while dirty.
        if (stateDirty_ && (nowMs < lastStateSaveMs_ || (nowMs - lastStateSaveMs_) >= kStateSaveIntervalMs)) {
            saveState();
        }
    }

    // Close every watch on this thread so the overlapped buffers drain safely.
    for (auto& w : watches_) {
        if (w) {
            w->stop();
        }
    }
    watches_.clear();
    HH_LOG_INFO(kLog, L"tailer thread stopped");
}

/**
 * @brief Re-runs discovery and syncs the directory watches to the result.
 *
 * Watches are diffed rather than rebuilt so a rediscovery every minute does
 * not churn kernel handles; dead watches (volume gone) are retried here.
 */
void AmeLogTailer::discover() {
    lastDiscoverMs_ = platform::nowMonotonicMs();
    std::vector<LogCandidate> found = discoverAmeLogs(config_.logOverrides);

    // ---- Log the outcome when it differs from last time ----------------------
    bool changed = (found.size() != candidates_.size());
    if (!changed) {
        for (size_t i = 0; i < found.size(); ++i) {
            if (!platform::iequals(found[i].path, candidates_[i].path) || found[i].exists != candidates_[i].exists) {
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        HH_LOG_INFO(kLog, L"discovered {} log candidate(s)", found.size());
        for (const LogCandidate& c : found) {
            HH_LOG_INFO(kLog, L"  {} [{}{}{}]", c.path, c.isErrorLog ? L"error log" : L"main log",
                        c.version.empty() ? L"" : L", v", c.version.empty() ? L"" : c.version.c_str());
            if (!c.exists) {
                HH_LOG_INFO(kLog, L"    (missing; watching for creation)");
            }
        }
        if (found.empty()) {
            HH_LOG_WARN(kLog, L"no AME encoding log found; is Adobe Media Encoder installed?");
        }
    } else {
        HH_LOG_DEBUG(kLog, L"rediscovery: {} candidate(s), unchanged", found.size());
    }
    candidates_ = std::move(found);

    // ---- Distinct directories to watch --------------------------------------
    std::vector<std::wstring> dirs;
    for (const LogCandidate& c : candidates_) {
        if (c.directory.empty() || containsFolder(dirs, c.directory)) {
            continue;
        }
        dirs.push_back(c.directory);
    }

    // Drop watches that died or whose directory is no longer interesting.
    for (auto it = watches_.begin(); it != watches_.end();) {
        platform::DirectoryWatch* w = it->get();
        if (w == nullptr || !w->active() || !containsFolder(dirs, w->directory())) {
            if (w != nullptr) {
                HH_LOG_DEBUG(kLog, L"dropping watch on '{}'", w->directory());
                w->stop();
            }
            it = watches_.erase(it);
        } else {
            ++it;
        }
    }

    // Add watches for directories that do not have one yet.
    for (const std::wstring& dir : dirs) {
        bool have = false;
        for (const auto& w : watches_) {
            if (w && platform::iequals(w->directory(), dir)) {
                have = true;
                break;
            }
        }
        if (have) {
            continue;
        }
        if (watches_.size() >= kMaxWatches) {
            HH_LOG_WARN(kLog, L"watch limit ({}) reached; '{}' is polled only", kMaxWatches, dir);
            continue;
        }
        // An override folder that does not exist yet is polled until it does.
        if (!platform::isDirectory(dir)) {
            HH_LOG_DEBUG(kLog, L"'{}' does not exist yet; polling only", dir);
            continue;
        }
        auto watch = std::make_unique<platform::DirectoryWatch>();
        const auto started = watch->start(dir, false, kNotifyFilter);
        if (!started) {
            HH_LOG_WARN(kLog, L"cannot watch '{}' ({}); polling only", dir, started.error().toString());
            continue;
        }
        HH_LOG_DEBUG(kLog, L"watching '{}'", dir);
        watches_.push_back(std::move(watch));
    }
}

/**
 * @brief Reads everything appended to @p candidate since the last read,
 *        feeds it to the parser and posts the resulting events.
 * @param forceSeed restart from byte 0 in seed mode regardless of the state
 */
void AmeLogTailer::readFile(const LogCandidate& candidate, TailState& state, bool forceSeed) {
    if (candidate.path.empty()) {
        return;
    }
    if (!state.parser) {
        state.parser = std::make_unique<AmeLogParser>(config_.preferDayFirstDates);
    }
    const std::wstring key = stateKeyFor(candidate.path);

    // ---- Optional full restart ----------------------------------------------
    if (forceSeed && (state.offset != 0 || state.seeded)) {
        HH_LOG_DEBUG(kLog, L"forced re-seed of '{}'", candidate.path);
        state.offset = 0;
        state.carry.clear();
        state.parser->reset();
        state.seeded = false;
        stateDirty_ = true;
    }

    // ---- Open per read; never held across a wait ----------------------------
    platform::FileReader file;
    if (auto opened = file.open(candidate.path, true); !opened) {
        const DWORD err = opened.error().win32;
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
            // AME holds it exclusively for a moment while writing; next round.
            HH_LOG_DEBUG(kLog, L"'{}' is locked; retrying next round", candidate.path);
        } else if (isBenignError(err)) {
            HH_LOG_DEBUG(kLog, L"'{}' vanished before it could be opened", candidate.path);
        } else {
            HH_LOG_WARN(kLog, L"cannot open '{}': {}", candidate.path, opened.error().toString());
            postNote(sink_, key, L"Cannot open " + candidate.path + L": " + opened.error().toString(), true);
        }
        return;
    }

    // ---- Identity: same file as last time? ----------------------------------
    const auto identResult = platform::identity(candidate.path);
    if (!identResult) {
        HH_LOG_WARN(kLog, L"identity('{}') failed: {}", candidate.path, identResult.error().toString());
        if (!isBenignError(identResult.error().win32)) {
            postNote(sink_, key, L"Cannot inspect " + candidate.path + L": " + identResult.error().toString(), true);
        }
        return;
    }
    const platform::FileIdentity ident = identResult.value();

    // A different file id, or a file shorter than what we already consumed,
    // means AME (or the user) replaced the log: start over in seed mode.
    bool recreated = false;
    if (state.identity.valid) {
        if (!looksLikeSameLog(state.identity, ident, state.offset) || ident.size < state.offset) {
            HH_LOG_INFO(kLog, L"'{}' was recreated or truncated ({} -> {} bytes); re-seeding", candidate.path,
                        state.identity.size, ident.size);
            recreated = true;
            state.offset = 0;
            state.carry.clear();
            state.parser->reset();
            state.seeded = false;
            stateDirty_ = true;
        }
    } else if (!state.seeded && ident.creationUtc != 0 && ident.creationUtc >= state.appStartUtc) {
        // A log born after we started is as fresh as a recreated one: blocks
        // stamped after the app start are genuinely new exports.
        recreated = true;
    }

    // ---- Encoding detection at the top of the file ---------------------------
    if (state.offset == 0) {
        uint8_t head[kProbeBytes] = {};
        size_t got = 0;
        DWORD err = 0;
        if (!file.readAt(0, head, sizeof(head), got, err)) {
            HH_LOG_WARN(kLog, L"cannot read the head of '{}': {} ({})", candidate.path, win32ErrorText(err), err);
            postNote(sink_, key, L"Cannot read " + candidate.path + L": " + win32ErrorText(err), true);
            return;
        }
        if (got < 2) {
            // Nothing to decide on yet (AME writes BOM + first line in one go).
            HH_LOG_DEBUG(kLog, L"'{}' has {} byte(s); waiting for content", candidate.path, got);
            state.identity = ident;
            return;
        }
        state.carry.clear();
        if (head[0] == 0xFF && head[1] == 0xFE) {
            state.utf16 = true;
            state.offset = 2;
        } else if (std::memchr(head, 0, got) != nullptr) {
            // UTF-16 without a BOM: ASCII text shows up as "X\0Y\0".
            state.utf16 = true;
            state.offset = 0;
        } else {
            state.utf16 = false;
            state.offset = 0;
        }
        stateDirty_ = true;
        HH_LOG_DEBUG(kLog, L"'{}' encoding: {}", candidate.path, state.utf16 ? L"UTF-16LE" : L"ANSI/UTF-8");
    }

    // ---- Read from the offset to EOF in chunks --------------------------------
    std::vector<uint8_t> chunk(kChunkBytes);
    uint64_t bytesRead = 0;
    size_t linesFed = 0;
    for (;;) {
        size_t got = 0;
        DWORD err = 0;
        if (!file.readAt(state.offset, chunk.data(), chunk.size(), got, err)) {
            if (err != ERROR_HANDLE_EOF) {
                HH_LOG_WARN(kLog, L"read of '{}' failed at {}: {} ({})", candidate.path, state.offset,
                            win32ErrorText(err), err);
                postNote(sink_, key, L"Read error on " + candidate.path + L": " + win32ErrorText(err), true);
            }
            break;
        }
        if (got == 0) {
            break;   // EOF
        }

        // Append and feed the complete lines straight away so memory stays
        // bounded by one chunk plus the partial tail.
        state.carry.insert(state.carry.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
        state.offset += got;
        bytesRead += got;
        if (state.utf16) {
            linesFed += drainUtf16Lines(state.carry, *state.parser, ident.lastWriteUtc);
        } else {
            linesFed += drainNarrowLines(state.carry, *state.parser, ident.lastWriteUtc);
        }

        // A stop request must not wait for a multi-MB seed to finish; the
        // state stays consistent because every consumed byte is accounted for.
        if (stopEvent_.isSet()) {
            HH_LOG_DEBUG(kLog, L"stop requested mid-read of '{}'", candidate.path);
            break;
        }
    }
    file.close();

    // Remember what we saw; a race with an append is corrected by the offset.
    state.identity = ident;
    if (state.offset > state.identity.size) {
        state.identity.size = state.offset;
    }
    stateDirty_ = true;
    HH_LOG_DEBUG(kLog, L"read {} byte(s), {} line(s) from '{}' (offset {}, carry {})", bytesRead, linesFed,
                 candidate.path, state.offset, state.carry.size());

    // ---- Harvest and post ------------------------------------------------------
    std::vector<AmeItemRecord> items = state.parser->takeItems();
    std::vector<AmeQueueEvent> queue = state.parser->takeQueueEvents();
    const bool seedPass = !state.seeded;
    const size_t cap = static_cast<size_t>(config_.recentFoldersCount);
    size_t seededCount = 0;
    size_t liveCount = 0;
    bool foldersChanged = false;

    for (AmeItemRecord& rec : items) {
        // History unless the file was (re)created and the block is newer than us.
        bool seed = seedPass;
        if (seed && recreated && rec.statusUtc != 0 && rec.statusUtc >= state.appStartUtc) {
            seed = false;
        }
        if (seed) {
            ++seededCount;
        } else {
            ++liveCount;
            HH_LOG_INFO(kLog, L"live item ({}): '{}' [{}]{}", resultName(rec.result), rec.outputPath, rec.statusText,
                        candidate.isErrorLog ? L" (error log)" : L"");
        }

        // Successful exports teach us where renders land.
        if (rec.result == AmeItemRecord::Result::Success && !rec.outputPath.empty()) {
            const std::wstring folder = path::parent(rec.outputPath);
            if (rememberFolder(recentFolders_, folder, cap)) {
                foldersChanged = true;
            }
        }

        LogItemEvent ev;
        ev.record = std::move(rec);
        ev.seed = seed;
        ev.fromErrorLog = candidate.isErrorLog;
        ev.logPath = candidate.path;
        sink_.post(EngineEvent{std::move(ev)});
    }

    for (const AmeQueueEvent& q : queue) {
        bool seed = seedPass;
        if (seed && recreated && q.utc != 0 && q.utc >= state.appStartUtc) {
            seed = false;
        }
        LogQueueEvent ev;
        ev.event = q;
        ev.seed = seed;
        sink_.post(EngineEvent{std::move(ev)});
    }

    // ---- Seed bookkeeping --------------------------------------------------------
    if (seedPass) {
        state.seeded = true;
        stateDirty_ = true;
        HH_LOG_INFO(kLog, L"seeded '{}': {} historical item(s), {} live, {} queue event(s)", candidate.path,
                    seededCount, liveCount, queue.size());
        // The folder list is announced once the main log's history is in.
        if (!candidate.isErrorLog) {
            emitFolders();
        }
    } else if (foldersChanged && !candidate.isErrorLog) {
        // A live export into a folder we had not seen: let the watcher know.
        emitFolders();
    }
}

// ===========================================================================
// Persistence
// ===========================================================================

/**
 * @brief Loads state.json and keeps every entry whose stored identity still
 *        matches the file on disk; anything else is re-seeded from scratch.
 */
void AmeLogTailer::loadState() {
    states_.clear();
    if (config_.statePath.empty()) {
        HH_LOG_DEBUG(kLog, L"no state path configured; every log will be seeded");
        return;
    }
    if (!platform::isFile(config_.statePath)) {
        HH_LOG_DEBUG(kLog, L"no state file at '{}' (first run)", config_.statePath);
        return;
    }

    // ---- Read and parse ------------------------------------------------------
    const auto bytes = platform::readAll(config_.statePath, kMaxStateBytes);
    if (!bytes) {
        HH_LOG_WARN(kLog, L"cannot read '{}': {}", config_.statePath, bytes.error().toString());
        return;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    if (platform::trim(text).empty()) {
        HH_LOG_DEBUG(kLog, L"state file '{}' is empty", config_.statePath);
        return;
    }
    const json root = json::parse(text.begin(), text.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        HH_LOG_WARN(kLog, L"state file '{}' is not valid JSON; ignoring it", config_.statePath);
        return;
    }
    const auto tailers = root.find("tailers");
    if (tailers == root.end() || !tailers->is_object()) {
        HH_LOG_DEBUG(kLog, L"state file has no 'tailers' object");
        return;
    }

    // ---- Validate each entry against the live file -----------------------------
    size_t loaded = 0;
    size_t discarded = 0;
    for (const auto& [rawKey, entry] : tailers->items()) {
        if (loaded + discarded >= kMaxStateEntries) {
            HH_LOG_WARN(kLog, L"more than {} state entries; ignoring the rest", kMaxStateEntries);
            break;
        }
        if (rawKey.empty() || !entry.is_object()) {
            ++discarded;
            continue;
        }
        const std::wstring logPath = platform::toWide(rawKey);
        const std::wstring key = stateKeyFor(logPath);

        // Stored identity and offsets (missing members keep their defaults).
        platform::FileIdentity stored;
        uint64_t offset = 0;
        uint64_t volumeSerial = 0;
        bool utf16 = true;
        bool seeded = false;
        getU64(entry, "offset", offset);
        getU64(entry, "size", stored.size);
        getU64(entry, "lastWrite", stored.lastWriteUtc);
        getU64(entry, "creation", stored.creationUtc);
        getU64(entry, "fileIdLow", stored.fileIdLow);
        getU64(entry, "fileIdHigh", stored.fileIdHigh);
        getU64(entry, "volumeSerial", volumeSerial);
        getBool(entry, "hasFileId", stored.hasFileId);
        getBool(entry, "utf16", utf16);
        getBool(entry, "seeded", seeded);
        stored.volumeSerial = static_cast<uint32_t>(volumeSerial);
        stored.valid = true;

        // The file must still be the one we left behind.
        const auto live = platform::identity(logPath);
        if (!live || !looksLikeSameLog(stored, live.value(), offset) || live.value().size < offset) {
            HH_LOG_DEBUG(kLog, L"state for '{}' discarded (file changed or missing)", logPath);
            ++discarded;
            continue;
        }

        // A seed that never finished is restarted from the top.
        if (!seeded) {
            offset = 0;
            stored = platform::FileIdentity{};
        }

        TailState st;
        st.offset = offset;
        st.identity = stored;
        st.utf16 = utf16;
        st.seeded = seeded;
        st.parser = std::make_unique<AmeLogParser>(config_.preferDayFirstDates);
        st.appStartUtc = appStartUtc_;
        states_.emplace(key, std::move(st));
        ++loaded;
        HH_LOG_DEBUG(kLog, L"state for '{}': offset {}, {}", logPath, offset, seeded ? L"seeded" : L"unseeded");
    }
    HH_LOG_INFO(kLog, L"loaded tail state: {} entry(ies) kept, {} discarded", loaded, discarded);
}

/**
 * @brief Writes state.json atomically. The persisted offset is the start of
 *        the carry so an unterminated last line is re-read after a restart.
 */
void AmeLogTailer::saveState() {
    lastStateSaveMs_ = platform::nowMonotonicMs();
    if (config_.statePath.empty()) {
        stateDirty_ = false;
        return;
    }

    // ---- Build the document ------------------------------------------------------
    json tailers = json::object();
    for (const auto& [key, st] : states_) {
        // Nothing worth remembering until a read completed.
        if (key.empty() || !st.identity.valid) {
            continue;
        }
        const uint64_t carry = static_cast<uint64_t>(st.carry.size());
        const uint64_t offset = (st.offset >= carry) ? (st.offset - carry) : 0;

        json e = json::object();
        e["offset"] = offset;
        e["size"] = st.identity.size;
        e["lastWrite"] = st.identity.lastWriteUtc;
        e["creation"] = st.identity.creationUtc;
        e["fileIdLow"] = st.identity.fileIdLow;
        e["fileIdHigh"] = st.identity.fileIdHigh;
        e["volumeSerial"] = static_cast<uint64_t>(st.identity.volumeSerial);
        e["hasFileId"] = st.identity.hasFileId;
        e["utf16"] = st.utf16;
        e["seeded"] = st.seeded;
        tailers[platform::toUtf8(key)] = std::move(e);
    }
    json root = json::object();
    root["tailers"] = std::move(tailers);
    const std::string text = root.dump(2, ' ', false, json::error_handler_t::replace);

    // ---- Write atomically (the folder may not exist on a first run) ----------------
    const std::wstring dir = path::parent(config_.statePath);
    if (!dir.empty() && !platform::isDirectory(dir)) {
        const auto made = platform::createDirectories(dir);
        if (!made) {
            HH_LOG_WARN(kLog, L"cannot create '{}': {}", dir, made.error().toString());
        }
    }
    const auto written = platform::writeAllAtomic(config_.statePath, text);
    if (!written) {
        // Stay dirty so the next tick retries; the throttle above limits the rate.
        HH_LOG_WARN(kLog, L"cannot write '{}': {}", config_.statePath, written.error().toString());
        postNote(sink_, config_.statePath, L"Cannot save tail state: " + written.error().toString(), true);
        return;
    }
    stateDirty_ = false;
    HH_LOG_DEBUG(kLog, L"saved tail state ({} entries) to '{}'", root["tailers"].size(), config_.statePath);
}

/**
 * @brief Tells the engine which output folders the log history mentions.
 */
void AmeLogTailer::emitFolders() {
    if (recentFolders_.empty()) {
        return;
    }
    LogFoldersEvent ev;
    ev.folders = recentFolders_;
    HH_LOG_INFO(kLog, L"announcing {} recent output folder(s)", ev.folders.size());
    sink_.post(EngineEvent{std::move(ev)});
}

} // namespace hh
