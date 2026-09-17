// ---------------------------------------------------------------------------
// IpcServer.cpp - named-pipe server thread (\\.\pipe\HdrHint).
//
// One thread multiplexes every pipe instance with WaitForMultipleObjects:
//
//   [0] stop event      -> leave the loop
//   [1] wake event      -> the engine queued outbound lines; hand them out
//   [2..] pipe events   -> a client connected, sent bytes or went away
//
// Inbound bytes are split on '\n' and posted to the engine one line at a
// time as IpcMessageEvent; connects and disconnects become IpcClientEvent.
// The thread never touches engine state directly - everything goes through
// IEngineSink::post, and the engine only reaches us through send()/broadcast()
// which just queue under a mutex and poke the wake event.
// ---------------------------------------------------------------------------
#include "core/IpcServer.h"

#include "core/Logger.h"
#include "platform/Time.h"
#include "platform/Utf.h"

#include <algorithm>
#include <cstdint>
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

/// Component tag for the log.
constexpr const wchar_t* kLog = L"IpcServer";

/// Namespace prefix every local pipe name lives under.
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\";

/// A client that sends more than this without a newline is misbehaving.
constexpr size_t kMaxInboundBytes = 1024u * 1024u;

/// Per-client outbound queue cap; older lines are dropped beyond it.
constexpr size_t kMaxOutboundLines = 4096;

/// Cross-thread queue cap (engine -> server thread).
constexpr size_t kMaxPendingSends = 16384;

/// The wait slice: short enough that liveness checks run even when idle.
constexpr DWORD kWaitSliceMs = 1000;

/// stop + wake take two of the MAXIMUM_WAIT_OBJECTS slots.
constexpr int kMaxInstancesHard = MAXIMUM_WAIT_OBJECTS - 2;

/// Liveness used when the caller passes a negative value.
constexpr int kDefaultLivenessSeconds = 15;

/// Consecutive WaitForMultipleObjects failures before the thread gives up.
constexpr int kMaxWaitFailures = 5;

using PipeState = platform::PipeInstance::State;

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/**
 * @brief Binds the server to the sink that receives its events.
 */
IpcServer::IpcServer(IEngineSink& sink) : sink_(sink) {}

/**
 * @brief Stops the thread and closes every pipe.
 */
IpcServer::~IpcServer() {
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Creates the pipe instances and starts the server thread.
 *
 * The first instance is created with FILE_FLAG_FIRST_PIPE_INSTANCE, so an
 * ERROR_ACCESS_DENIED there means another process (a second HdrHint) already
 * owns the name and we must not fight over it.
 */
Result<void> IpcServer::start(const std::wstring& pipeName, int maxInstances, int livenessSeconds) {
    if (running_.load() || thread_.joinable()) {
        return Error::text(L"IpcServer::start: already running");
    }

    // Build the full pipe name; accept an already-qualified name as well.
    const std::wstring_view shortName = platform::trim(pipeName);
    if (shortName.empty()) {
        return Error::text(L"IpcServer::start: empty pipe name");
    }
    if (platform::istartsWith(shortName, kPipePrefix)) {
        fullName_ = std::wstring(shortName);
    } else {
        fullName_ = std::wstring(kPipePrefix) + std::wstring(shortName);
    }
    // The part after the prefix must be a single component.
    const std::wstring_view leaf = std::wstring_view(fullName_).substr(std::wstring_view(kPipePrefix).size());
    if (leaf.empty() || leaf.find_first_of(L"\\/") != std::wstring_view::npos) {
        return Error::text(L"IpcServer::start: invalid pipe name '" + fullName_ + L"'");
    }

    // Clamp the knobs to something the wait loop can actually handle.
    maxInstances_ = std::clamp(maxInstances, 1, kMaxInstancesHard);
    livenessSeconds_ = (livenessSeconds < 0) ? kDefaultLivenessSeconds : livenessSeconds;

    // Everything below undoes itself on failure.
    auto abortStart = [this]() {
        for (auto& c : clients_) {
            if (c && c->pipe) { c->pipe->close(); }
        }
        clients_.clear();
        security_.reset();
        stopEvent_.reset();
        wakeEvent_.reset();
        connected_.store(0);
    };

    // Control events: stop is manual-reset (stays signalled), wake auto-reset.
    stopEvent_ = platform::makeEvent(true);
    wakeEvent_ = platform::makeEvent(false);
    if (!stopEvent_ || !wakeEvent_) {
        const Error err = Error::fromLastError(L"IpcServer: CreateEvent");
        abortStart();
        return err;
    }

    // DACL that admits only this user and SYSTEM; the pipe falls back to the
    // default descriptor when it cannot be built.
    security_ = std::make_unique<platform::PipeSecurity>();
    if (!security_->attributes()) {
        HH_LOG_WARN(kLog, L"pipe security descriptor unavailable, using the default DACL");
    }

    // Create the instances. Ids are 1..N and double as connection ids.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingSends_.clear();
    }
    clients_.clear();
    connected_.store(0);

    for (int i = 1; i <= maxInstances_; ++i) {
        auto client = std::make_unique<Client>();
        client->pipe = std::make_unique<platform::PipeInstance>();
        client->pipe->setId(static_cast<uint32_t>(i));

        const bool first = (i == 1);
        const auto created = client->pipe->create(fullName_, first, static_cast<DWORD>(maxInstances_),
                                                  security_->attributes());
        if (!created) {
            // The first instance decides ownership of the name.
            if (first) {
                abortStart();
                if (created.error().win32 == ERROR_ACCESS_DENIED) {
                    HH_LOG_ERROR(kLog, L"pipe '{}' is owned by another process", fullName_);
                    return Error::fromWin32(ERROR_ACCESS_DENIED,
                                            L"IpcServer: pipe '" + fullName_ + L"' is owned by another process");
                }
                HH_LOG_ERROR(kLog, L"cannot create pipe '{}': {}", fullName_, created.error().toString());
                return created.error();
            }
            // Later instances are best-effort: fewer concurrent panels, not a failure.
            HH_LOG_WARN(kLog, L"pipe instance {} failed ({}), continuing with {} instance(s)",
                        i, created.error().toString(), clients_.size());
            break;
        }
        clients_.push_back(std::move(client));
    }

    if (clients_.empty()) {
        abortStart();
        return Error::text(L"IpcServer: no pipe instance could be created");
    }

    // Spin up the thread. std::thread has no non-throwing constructor, so the
    // one exception it can raise (resource exhaustion) is caught right here.
    running_.store(true);
    try {
        thread_ = std::thread(&IpcServer::threadMain, this);
    } catch (const std::system_error& e) {
        running_.store(false);
        abortStart();
        return Error::text(L"IpcServer: thread creation failed: " + platform::toWide(e.what()));
    }

    HH_LOG_INFO(kLog, L"listening on '{}' ({} instance(s), liveness {} s)",
                fullName_, clients_.size(), livenessSeconds_);
    return Result<void>::success();
}

/**
 * @brief Signals the thread, joins it and closes every pipe. Safe to call twice.
 */
void IpcServer::stop() {
    const bool wasRunning = running_.load() || thread_.joinable();

    // Ask the loop to leave.
    if (stopEvent_ && !::SetEvent(stopEvent_.get())) {
        HH_LOG_WARN(kLog, L"SetEvent(stop) failed: {}", Error::fromLastError(L"SetEvent").toString());
    }

    // Wait for it. Joining from the server thread itself would deadlock, so
    // that (programming error) case detaches instead and is logged loudly.
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            HH_LOG_ERROR(kLog, L"stop() called from the server thread; detaching");
            thread_.detach();
        } else {
            thread_.join();
        }
    }
    running_.store(false);

    // Tell the engine about clients that were still attached, then drop the pipes.
    for (auto& c : clients_) {
        if (!c) { continue; }
        if (c->connected && c->pipe) {
            sink_.post(IpcClientEvent{c->pipe->id(), false});
        }
        c->connected = false;
        if (c->pipe) { c->pipe->close(); }
    }
    clients_.clear();
    connected_.store(0);

    // Nothing queued can be delivered any more.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingSends_.clear();
    }
    stopEvent_.reset();
    wakeEvent_.reset();
    security_.reset();

    if (wasRunning) {
        HH_LOG_INFO(kLog, L"stopped");
    }
}

// ---------------------------------------------------------------------------
// Engine -> server (any thread)
// ---------------------------------------------------------------------------

/**
 * @brief Queues a line for one client (0 = every connected client).
 */
void IpcServer::send(uint32_t connectionId, std::string line) {
    if (line.empty()) { return; }
    if (!running_.load()) {
        HH_LOG_DEBUG(kLog, L"send: server not running, dropping line for client {}", connectionId);
        return;
    }

    // Queue under the mutex; the server thread swaps the vector out wholesale.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingSends_.size() >= kMaxPendingSends) {
            HH_LOG_WARN(kLog, L"send queue full ({}), dropping the oldest line", pendingSends_.size());
            pendingSends_.erase(pendingSends_.begin());
        }
        pendingSends_.emplace_back(connectionId, std::move(line));
    }

    // Poke the loop.
    if (wakeEvent_ && !::SetEvent(wakeEvent_.get())) {
        HH_LOG_WARN(kLog, L"SetEvent(wake) failed: {}", Error::fromLastError(L"SetEvent").toString());
    }
}

/**
 * @brief Queues a line for every connected client.
 */
void IpcServer::broadcast(std::string line) {
    send(0, std::move(line));
}

// ---------------------------------------------------------------------------
// Server thread
// ---------------------------------------------------------------------------

/**
 * @brief The wait loop. Runs until the stop event fires.
 */
void IpcServer::threadMain() {
    HH_LOG_INFO(kLog, L"server thread started ({} instance(s))", clients_.size());

    // ---- helpers -----------------------------------------------------------

    // Drops a client: tells the engine (when it was connected), clears the
    // buffers and, unless the pipe already re-armed itself, starts accepting
    // the next panel on that instance.
    auto dropClient = [this](Client& c, const wchar_t* why, bool reconnect) {
        const uint32_t id = c.pipe ? c.pipe->id() : 0u;
        if (c.connected) {
            c.connected = false;
            if (connected_.load() > 0) { connected_.fetch_sub(1); }
            HH_LOG_INFO(kLog, L"client {} disconnected ({})", id, why ? why : L"");
            sink_.post(IpcClientEvent{id, false});
        }
        c.inbound.clear();
        c.outbound.clear();
        c.lastActivityMs = 0;
        if (reconnect && c.pipe) { c.pipe->disconnectAndReconnect(); }
    };

    // A pipe event fired: advance that instance and act on what it produced.
    auto onClientSignalled = [this, &dropClient](Client& c) {
        if (!c.pipe) { return; }
        const uint32_t id = c.pipe->id();
        const bool wasConnected = c.connected;

        // The instance either read bytes, completed a connect, or lost the client.
        std::string received;
        if (!c.pipe->onSignalled(received)) {
            dropClient(c, L"pipe closed", false);
            return;
        }

        // First time we see it connected: announce it and arm liveness.
        const PipeState st = c.pipe->state();
        const bool nowConnected = (st == PipeState::Connected || st == PipeState::Reading);
        if (!wasConnected && nowConnected) {
            c.connected = true;
            c.lastActivityMs = platform::nowMonotonicMs();
            connected_.fetch_add(1);
            HH_LOG_INFO(kLog, L"client {} connected", id);
            sink_.post(IpcClientEvent{id, true});
        }
        if (received.empty()) { return; }

        // Bytes before a connect completed would be a state-machine bug upstream.
        if (!c.connected) {
            HH_LOG_WARN(kLog, L"client {}: {} byte(s) received while not connected, ignoring", id, received.size());
            return;
        }

        // A line that never ends is an attack or a broken client either way.
        if (c.inbound.size() + received.size() > kMaxInboundBytes) {
            HH_LOG_WARN(kLog, L"client {}: inbound buffer exceeded {} bytes, dropping connection",
                        id, kMaxInboundBytes);
            dropClient(c, L"inbound overflow", true);
            return;
        }

        c.inbound.append(received);
        c.lastActivityMs = platform::nowMonotonicMs();
        splitLines(c);
    };

    // The engine queued lines: route them to the right client(s) and write.
    auto drainSends = [this]() {
        std::vector<std::pair<uint32_t, std::string>> batch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            batch.swap(pendingSends_);
        }

        for (auto& item : batch) {
            const uint32_t target = item.first;
            std::string& text = item.second;
            if (text.empty()) { continue; }

            bool queued = false;
            for (auto& c : clients_) {
                if (!c || !c->pipe) { continue; }
                if (target != 0 && c->pipe->id() != target) { continue; }

                // Only connected clients get anything; a line for a specific
                // client that has gone is simply dropped.
                if (!c->connected) {
                    if (target != 0) { break; }
                    continue;
                }
                if (c->outbound.size() >= kMaxOutboundLines) {
                    HH_LOG_WARN(kLog, L"client {}: outbound queue full, dropping the oldest line", c->pipe->id());
                    c->outbound.pop_front();
                }
                c->outbound.push_back(text);
                queued = true;
                if (target != 0) { break; }
            }
            if (!queued && target != 0) {
                HH_LOG_DEBUG(kLog, L"no connected client {} for a queued line, dropped", target);
            }
        }

        // Write everything out now.
        for (auto& c : clients_) {
            if (c && !c->outbound.empty()) { flushOutbound(*c); }
        }
    };

    // A connected panel that has been silent too long is presumed dead
    // (AME crashed, the panel was closed without a goodbye, ...).
    auto checkLiveness = [this, &dropClient]() {
        if (livenessSeconds_ <= 0) { return; }
        const uint64_t now = platform::nowMonotonicMs();
        const uint64_t limit = static_cast<uint64_t>(livenessSeconds_) * 1000ull;
        for (auto& c : clients_) {
            if (!c || !c->connected || !c->pipe) { continue; }
            const uint64_t silent = (now >= c->lastActivityMs) ? (now - c->lastActivityMs) : 0ull;
            if (silent > limit) {
                HH_LOG_WARN(kLog, L"client {} silent for {} ms, dropping", c->pipe->id(), silent);
                dropClient(*c, L"liveness timeout", true);
            }
        }
    };

    // ---- the loop ----------------------------------------------------------

    std::vector<HANDLE> handles;
    std::vector<Client*> waitClients;
    handles.reserve(2 + clients_.size());
    waitClients.reserve(clients_.size());
    int waitFailures = 0;

    for (;;) {
        // Rebuild the wait set every pass; it is tiny and instances can close.
        handles.clear();
        waitClients.clear();
        handles.push_back(stopEvent_.get());
        handles.push_back(wakeEvent_.get());
        for (auto& c : clients_) {
            if (c && c->pipe && c->pipe->event()) {
                handles.push_back(c->pipe->event());
                waitClients.push_back(c.get());
            }
        }

        const DWORD count = static_cast<DWORD>(handles.size());
        const DWORD r = ::WaitForMultipleObjects(count, handles.data(), FALSE, kWaitSliceMs);

        // A failing wait usually means a handle went bad; do not spin forever.
        if (r == WAIT_FAILED) {
            ++waitFailures;
            HH_LOG_ERROR(kLog, L"WaitForMultipleObjects failed: {}",
                         Error::fromLastError(L"WaitForMultipleObjects").toString());
            if (waitFailures >= kMaxWaitFailures) {
                HH_LOG_ERROR(kLog, L"giving up after {} consecutive wait failures", waitFailures);
                break;
            }
            ::Sleep(100);
            continue;
        }
        waitFailures = 0;

        // Dispatch on what fired.
        if (r == WAIT_OBJECT_0) {
            break;                                   // stop requested
        } else if (r == WAIT_OBJECT_0 + 1) {
            drainSends();                            // engine queued lines
        } else if (r >= WAIT_OBJECT_0 + 2 && r < WAIT_OBJECT_0 + count) {
            const size_t index = static_cast<size_t>(r - WAIT_OBJECT_0 - 2);
            if (index < waitClients.size() && waitClients[index]) {
                onClientSignalled(*waitClients[index]);
            }
        } else if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + count) {
            // Only mutexes can be abandoned; events cannot. Log and carry on.
            HH_LOG_WARN(kLog, L"unexpected abandoned wait (index {})", r - WAIT_ABANDONED_0);
        }
        // WAIT_TIMEOUT: nothing happened; the liveness sweep below still runs.

        checkLiveness();
    }

    running_.store(false);
    HH_LOG_INFO(kLog, L"server thread exiting");
}

/**
 * @brief Writes every queued line to the client; a failed write drops it.
 */
void IpcServer::flushOutbound(Client& c) {
    if (!c.pipe) {
        c.outbound.clear();
        return;
    }
    // Lines for a client that is not connected have nowhere to go.
    if (!c.connected) {
        if (!c.outbound.empty()) {
            HH_LOG_DEBUG(kLog, L"client {}: discarding {} line(s), not connected", c.pipe->id(), c.outbound.size());
        }
        c.outbound.clear();
        return;
    }

    const uint32_t id = c.pipe->id();
    while (!c.outbound.empty()) {
        std::string text = std::move(c.outbound.front());
        c.outbound.pop_front();
        if (text.empty()) { continue; }

        // The protocol is one JSON object per line.
        if (text.back() != '\n') { text.push_back('\n'); }

        // A write that fails or times out means the panel is gone or wedged;
        // either way the connection is not worth keeping.
        if (!c.pipe->write(text)) {
            HH_LOG_WARN(kLog, L"client {}: write failed, dropping connection", id);
            c.connected = false;
            if (connected_.load() > 0) { connected_.fetch_sub(1); }
            c.inbound.clear();
            c.outbound.clear();
            c.lastActivityMs = 0;
            c.pipe->disconnectAndReconnect();
            sink_.post(IpcClientEvent{id, false});
            return;
        }
    }
}

/**
 * @brief Posts every complete line in the inbound buffer and keeps the tail.
 */
void IpcServer::splitLines(Client& c) {
    if (!c.pipe || c.inbound.empty()) { return; }
    const uint32_t id = c.pipe->id();

    size_t start = 0;
    while (start < c.inbound.size()) {
        const size_t nl = c.inbound.find('\n', start);
        if (nl == std::string::npos) { break; }

        // One line without its terminator; tolerate CRLF and blank keep-alives.
        std::string_view lineView(c.inbound.data() + start, nl - start);
        while (!lineView.empty() && (lineView.back() == '\r' || lineView.back() == ' ' || lineView.back() == '\t')) {
            lineView.remove_suffix(1);
        }
        while (!lineView.empty() && (lineView.front() == ' ' || lineView.front() == '\t' || lineView.front() == '\r')) {
            lineView.remove_prefix(1);
        }
        if (!lineView.empty()) {
            sink_.post(IpcMessageEvent{id, std::string(lineView)});
        }
        start = nl + 1;
    }

    // Keep only the unterminated remainder.
    if (start >= c.inbound.size()) {
        c.inbound.clear();
    } else if (start > 0) {
        c.inbound.erase(0, start);
    }
}

} // namespace hh
