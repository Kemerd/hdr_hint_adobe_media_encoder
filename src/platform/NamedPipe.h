// ---------------------------------------------------------------------------
// NamedPipe.h - one overlapped server-side pipe instance.
//
// The IpcServer keeps N of these; each cycles through
//   Connecting -> Reading (repeat) -> Disconnected -> Connecting
// with all waits multiplexed on event() by the server thread.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Handle.h"
#include "platform/Win.h"

#include <string>
#include <vector>

namespace hh::platform {

/// Builds a DACL that admits only the current user and SYSTEM.
class PipeSecurity {
public:
    PipeSecurity();
    ~PipeSecurity();
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;
    /// nullptr when the descriptor could not be built (server falls back to default).
    [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept { return ok_ ? &sa_ : nullptr; }
private:
    SECURITY_ATTRIBUTES sa_{};
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    bool ok_ = false;
};

class PipeInstance {
public:
    enum class State { Idle, Connecting, Connected, Reading, Closed };

    PipeInstance();
    ~PipeInstance();
    PipeInstance(const PipeInstance&) = delete;
    PipeInstance& operator=(const PipeInstance&) = delete;

    /// Creates the pipe (name like "\\\\.\\pipe\\HdrHint") and starts connecting.
    Result<void> create(std::wstring_view fullName, bool firstInstance, DWORD maxInstances,
                        SECURITY_ATTRIBUTES* security);

    /// Event to wait on (signalled on connect / read completion).
    [[nodiscard]] HANDLE event() const noexcept { return event_.get(); }
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] uint32_t id() const noexcept { return id_; }
    void setId(uint32_t id) noexcept { id_ = id; }

    /**
     * @brief Advances the state machine after event() fired.
     * @param received  appended with any bytes read
     * @return false when the client disconnected (instance is reset to Connecting)
     */
    bool onSignalled(std::string& received);

    /// Synchronous write of a full buffer (short timeout); false on failure.
    bool write(std::string_view bytes, DWORD timeoutMs = 2000);

    /// Drops the client and starts accepting a new one.
    void disconnectAndReconnect();
    /// Cancels I/O and closes the pipe.
    void close();

private:
    bool beginConnect();
    bool beginRead();

    UniqueHandle pipe_;
    UniqueHandle event_;
    OVERLAPPED overlapped_{};
    std::vector<char> buffer_;
    State state_ = State::Idle;
    bool ioPending_ = false;
    uint32_t id_ = 0;
    std::wstring name_;
    DWORD maxInstances_ = 1;
    SECURITY_ATTRIBUTES* security_ = nullptr;
};

} // namespace hh::platform
