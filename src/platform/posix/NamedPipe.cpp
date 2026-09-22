// ---------------------------------------------------------------------------
// posix/NamedPipe.cpp - the IpcServer's endpoint on a Unix-domain socket.
//
// Mapping of the Windows model
//   CreateNamedPipe(FIRST_PIPE_INSTANCE)  -> bind() + listen() on the socket path
//   later CreateNamedPipe instances       -> share that listening socket
//   ConnectNamedPipe (overlapped)         -> wait for "listening socket readable",
//                                            then accept() (EAGAIN = another
//                                            instance won the race: keep waiting)
//   ReadFile (overlapped)                 -> wait for "client readable", recv()
//   DisconnectNamedPipe                   -> close the client, back to Connecting
//
// Ownership of the name: before binding, the first instance tries to connect
// to an existing socket file. A live answer means another HdrHint owns the
// endpoint (ERROR_ACCESS_DENIED, exactly what FILE_FLAG_FIRST_PIPE_INSTANCE
// reports); a refused connection means a stale file from a crash, which is
// removed and replaced.
// ---------------------------------------------------------------------------
#include "platform/NamedPipe.h"

#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/KnownFolders.h"
#include "platform/Time.h"
#include "platform/Utf.h"
#include "platform/posix/PosixCommon.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace hh::platform {

using posix::closeQuietly;
using posix::toNative;

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"NamedPipe";

/// recv() size per call.
constexpr size_t kReadChunk = 16u * 1024u;

/// Reads harvested per signal before yielding (mirrors the Windows spin cap).
constexpr int kMaxReadsPerSignal = 16;

/// Marks a descriptor non-blocking + close-on-exec and disables SIGPIPE.
void configureSocket(int fd) noexcept {
    ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

/// sockaddr_un for a path; false when it does not fit.
bool makeAddress(const std::string& path, sockaddr_un& addr) noexcept {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

/// True when something is accepting connections on @p path right now.
bool endpointIsLive(const sockaddr_un& addr) noexcept {
    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) {
        return false;
    }
    const bool live = ::connect(probe, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(probe);
    return live;
}

} // namespace

// ===========================================================================
// Shared listener
// ===========================================================================

/**
 * @brief One listening socket, shared by every instance of an endpoint.
 */
struct PipeInstance::Listener {
    int fd = -1;
    std::string path;

    ~Listener() {
        closeQuietly(fd);
        if (!path.empty()) {
            ::unlink(path.c_str());   // we created it, we remove it
        }
    }
};

namespace {

/// Endpoint path -> its listener while any instance still uses it.
std::mutex& listenerMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, std::weak_ptr<PipeInstance::Listener>>& listeners() {
    static std::map<std::string, std::weak_ptr<PipeInstance::Listener>> map;
    return map;
}

} // namespace

// ===========================================================================
// Naming and security
// ===========================================================================

std::wstring ipcEndpointName(std::wstring_view shortName) {
    // A path is already an endpoint.
    if (!shortName.empty() && shortName.front() == L'/') {
        return std::wstring(shortName);
    }
    // Windows-style "\\.\pipe\X" (from an old config) keeps only its leaf.
    std::wstring_view leaf = shortName;
    const size_t cut = leaf.find_last_of(L"\\/");
    if (cut != std::wstring_view::npos) {
        leaf = leaf.substr(cut + 1);
    }
    if (leaf.empty()) {
        leaf = L"HdrHint";
    }
    return appLocalDataFolder() + L"/" + std::wstring(leaf) + L".sock";
}

PipeSecurity::PipeSecurity() {
    // Owner read/write only - the POSIX spelling of "current user + SYSTEM".
    sa_.mode = 0600;
    ok_ = true;
}

PipeSecurity::~PipeSecurity() = default;

// ===========================================================================
// PipeInstance
// ===========================================================================

PipeInstance::PipeInstance() = default;

PipeInstance::~PipeInstance() {
    close();
}

/**
 * @brief First instance binds the socket; later ones join its listener.
 */
Result<void> PipeInstance::create(std::wstring_view fullName, bool firstInstance, DWORD maxInstances,
                                  PipeSecurityAttributes* security) {
    close();
    name_ = std::wstring(fullName);
    const std::string path = toNative(fullName);
    sockaddr_un addr{};
    if (!makeAddress(path, addr)) {
        return Error::fromWin32(ERROR_FILENAME_EXCED_RANGE, L"IPC socket path unusable: " + name_);
    }

    std::lock_guard<std::mutex> lock(listenerMutex());
    if (firstInstance) {
        // ---- ownership check ----------------------------------------------
        struct stat st {};
        if (::lstat(path.c_str(), &st) == 0) {
            if (endpointIsLive(addr)) {
                return Error::fromWin32(ERROR_ACCESS_DENIED, L"IPC socket '" + name_ + L"' is owned by another process");
            }
            ::unlink(path.c_str());   // stale leftover from a crash
        }
        // The folder normally exists already (settings live there).
        const size_t slash = name_.find_last_of(L'/');
        if (slash != std::wstring::npos && slash > 0) {
            createDirectories(name_.substr(0, slash));
        }

        // ---- bind + listen --------------------------------------------------
        auto listener = std::make_shared<Listener>();
        listener->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener->fd < 0) {
            return Error::fromErrno(errno, L"socket");
        }
        configureSocket(listener->fd);
        if (::bind(listener->fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            return Error::fromErrno(errno, L"bind " + name_);
        }
        listener->path = path;   // from here on the destructor unlinks it
        ::chmod(path.c_str(), security ? static_cast<mode_t>(security->mode) : static_cast<mode_t>(0600));
        const int backlog = static_cast<int>(std::clamp<DWORD>(maxInstances, 1, 64));
        if (::listen(listener->fd, backlog) != 0) {
            return Error::fromErrno(errno, L"listen " + name_);
        }
        listeners()[path] = listener;
        listener_ = std::move(listener);
    } else {
        // ---- join the first instance's listener -------------------------------
        const auto it = listeners().find(path);
        listener_ = (it != listeners().end()) ? it->second.lock() : nullptr;
        if (!listener_) {
            return Error::fromWin32(ERROR_INVALID_HANDLE, L"IPC socket '" + name_ + L"' has no listener");
        }
    }

    state_ = State::Connecting;
    HH_LOG_DEBUG(kLog, L"instance {}: listening on {}", id_, name_);
    return {};
}

WaitHandle PipeInstance::event() const noexcept {
    if (state_ == State::Connecting) {
        return listener_ ? listener_->fd : kInvalidWaitHandle;
    }
    if (state_ == State::Connected || state_ == State::Reading) {
        return client_;
    }
    return kInvalidWaitHandle;
}

/**
 * @brief accept() while connecting, recv() while connected.
 */
bool PipeInstance::onSignalled(std::string& received) {
    if (state_ == State::Idle || state_ == State::Closed) {
        return false;
    }

    // ---- connect phase ------------------------------------------------------
    if (state_ == State::Connecting) {
        if (!listener_ || listener_->fd < 0) {
            return false;
        }
        const int fd = ::accept(listener_->fd, nullptr, nullptr);
        if (fd < 0) {
            // Another instance accepted this client first, or a spurious wake-up.
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED) {
                return true;
            }
            HH_LOG_WARN(kLog, L"instance {}: accept failed: {}", id_, Error::fromErrno(errno, L"accept").toString());
            return true;
        }
        configureSocket(fd);
        client_ = fd;
        state_ = State::Connected;
        HH_LOG_INFO(kLog, L"instance {}: client connected", id_);
    }

    // ---- read phase -------------------------------------------------------------
    char buffer[kReadChunk];
    for (int reads = 0; reads < kMaxReadsPerSignal; ++reads) {
        const ssize_t n = ::recv(client_, buffer, sizeof(buffer), 0);
        if (n > 0) {
            received.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;   // drained; wait for more
        }
        // 0 = orderly shutdown; anything else = the client is gone.
        if (n == 0) {
            HH_LOG_INFO(kLog, L"instance {}: client disconnected", id_);
        } else {
            HH_LOG_WARN(kLog, L"instance {}: read failed: {}", id_, Error::fromErrno(errno, L"recv").toString());
        }
        disconnectAndReconnect();
        return false;
    }
    return true;   // spin cap reached; the socket stays readable and brings us back
}

/**
 * @brief Blocking-with-timeout send of the whole buffer.
 */
bool PipeInstance::write(std::string_view bytes, DWORD timeoutMs) {
    if (client_ < 0 || (state_ != State::Connected && state_ != State::Reading)) {
        HH_LOG_DEBUG(kLog, L"instance {}: write while not connected", id_);
        return false;
    }
    const uint64_t deadline = nowMonotonicMs() + timeoutMs;
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::send(client_, bytes.data() + done, bytes.size() - done, 0);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The client's buffer is full: wait (bounded) for room.
            const uint64_t now = nowMonotonicMs();
            if (now >= deadline) {
                HH_LOG_WARN(kLog, L"instance {}: write timed out after {} ms", id_, timeoutMs);
                return false;
            }
            pollfd p{};
            p.fd = client_;
            p.events = POLLOUT;
            ::poll(&p, 1, static_cast<int>(std::min<uint64_t>(deadline - now, 1000)));
            continue;
        }
        HH_LOG_WARN(kLog, L"instance {}: write failed: {}", id_, Error::fromErrno(errno, L"send").toString());
        return false;
    }
    return true;
}

void PipeInstance::disconnectAndReconnect() {
    closeQuietly(client_);
    state_ = listener_ ? State::Connecting : State::Closed;
}

void PipeInstance::close() {
    closeQuietly(client_);
    if (listener_) {
        std::lock_guard<std::mutex> lock(listenerMutex());
        const std::string path = listener_->path;
        listener_.reset();   // the last instance's reset closes + unlinks
        const auto it = listeners().find(path);
        if (it != listeners().end() && it->second.expired()) {
            listeners().erase(it);
        }
    }
    state_ = State::Closed;
}

} // namespace hh::platform
