// ---------------------------------------------------------------------------
// NamedPipe.h - one server-side IPC endpoint instance.
//
// The IpcServer keeps N of these; each cycles through
//   Connecting -> Reading (repeat) -> Disconnected -> Connecting
// with all waits multiplexed on event() by the server thread.
//
// Windows: an overlapped named-pipe instance (\\.\pipe\HdrHint).
// macOS:   a Unix-domain stream socket. The first instance binds and listens;
//          every instance waits on that listening socket while Connecting
//          and on its accepted connection afterwards, so the IpcServer's wait
//          loop is identical on both platforms. Node's net.connect({path})
//          talks to either one.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Event.h"
#include "platform/Win.h"

#if defined(_WIN32)
#include "platform/Handle.h"
#endif

#include <memory>
#include <string>
#include <vector>

namespace hh::platform {

#if defined(_WIN32)
/// Security attributes handed to CreateNamedPipeW.
using PipeSecurityAttributes = SECURITY_ATTRIBUTES;
#else
/// POSIX: the socket file is chmod'ed to this mode (owner only by default).
struct PipeSecurityAttributes {
    unsigned mode = 0600;
};
#endif

/**
 * @brief Full endpoint name for a short one ("HdrHint").
 *
 * Windows: "\\.\pipe\HdrHint". macOS: a socket path in the user's
 * application-support folder (".../HdrHint/HdrHint.sock"), which is private
 * to the user and short enough for sockaddr_un.
 */
std::wstring ipcEndpointName(std::wstring_view shortName);

/// Builds a DACL (Windows) / file mode (POSIX) that admits only the current user.
class PipeSecurity {
public:
    PipeSecurity();
    ~PipeSecurity();
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;
    /// nullptr when the descriptor could not be built (server falls back to default).
    [[nodiscard]] PipeSecurityAttributes* attributes() noexcept { return ok_ ? &sa_ : nullptr; }
private:
    PipeSecurityAttributes sa_{};
#if defined(_WIN32)
    PSECURITY_DESCRIPTOR sd_ = nullptr;
#endif
    bool ok_ = false;
};

class PipeInstance {
public:
    enum class State { Idle, Connecting, Connected, Reading, Closed };

    PipeInstance();
    ~PipeInstance();
    PipeInstance(const PipeInstance&) = delete;
    PipeInstance& operator=(const PipeInstance&) = delete;

    /// Creates the endpoint (name from ipcEndpointName) and starts connecting.
    Result<void> create(std::wstring_view fullName, bool firstInstance, DWORD maxInstances,
                        PipeSecurityAttributes* security);

    /// Handle to wait on (signalled on connect / readable data).
    [[nodiscard]] WaitHandle event() const noexcept;
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
    /// Cancels I/O and closes the endpoint.
    void close();

#if !defined(_WIN32)
    /// The listening socket shared by every instance of one endpoint (opaque).
    struct Listener;
#endif

private:
#if defined(_WIN32)
    bool beginConnect();
    bool beginRead();

    UniqueHandle pipe_;
    UniqueHandle event_;
    OVERLAPPED overlapped_{};
    std::vector<char> buffer_;
    bool ioPending_ = false;
    DWORD maxInstances_ = 1;
    PipeSecurityAttributes* security_ = nullptr;
#else
    std::shared_ptr<Listener> listener_;
    int client_ = -1;                    ///< accepted connection (Connected / Reading)
#endif
    State state_ = State::Idle;
    uint32_t id_ = 0;
    std::wstring name_;
};

} // namespace hh::platform
