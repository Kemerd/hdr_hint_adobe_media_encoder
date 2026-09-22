// ---------------------------------------------------------------------------
// IpcServer.h - named-pipe server thread (\\.\pipe\HdrHint).
// ---------------------------------------------------------------------------
#pragma once

#include "core/EngineEvents.h"
#include "platform/Event.h"
#include "platform/NamedPipe.h"
#include "platform/Win.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hh {

/**
 * @brief Accepts panel connections, splits lines, posts IpcMessageEvent /
 *        IpcClientEvent, and writes outbound lines queued by the engine.
 */
class IpcServer {
public:
    explicit IpcServer(IEngineSink& sink);
    ~IpcServer();
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// @param pipeName short name ("HdrHint") -> "\\\\.\\pipe\\HdrHint"
    Result<void> start(const std::wstring& pipeName, int maxInstances, int livenessSeconds = 15);
    void stop();

    /// Queues a line (with or without trailing '\n') to one client.
    void send(uint32_t connectionId, std::string line);
    /// Queues a line to every connected client.
    void broadcast(std::string line);
    [[nodiscard]] int connectedCount() const noexcept { return connected_.load(); }
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    struct Client {
        std::unique_ptr<platform::PipeInstance> pipe;
        std::string inbound;                 ///< partial line buffer
        std::deque<std::string> outbound;
        uint64_t lastActivityMs = 0;
        bool connected = false;
    };

    void threadMain();
    void flushOutbound(Client& c);
    void splitLines(Client& c);

    IEngineSink& sink_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> connected_{0};
    platform::Event stopEvent_;
    platform::Event wakeEvent_;
    std::mutex mutex_;
    std::vector<std::pair<uint32_t, std::string>> pendingSends_;   ///< (0 = broadcast)
    std::vector<std::unique_ptr<Client>> clients_;
    std::wstring fullName_;
    int maxInstances_ = 4;
    int livenessSeconds_ = 15;
    std::unique_ptr<platform::PipeSecurity> security_;
};

} // namespace hh
