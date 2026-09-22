// ---------------------------------------------------------------------------
// posix/SingleInstance.cpp - one HdrHint per user, argv forwarding (macOS).
//
//   ~/Library/Application Support/HdrHint/instance.lock   flock(LOCK_EX|LOCK_NB)
//   ~/Library/Application Support/HdrHint/instance.sock   Unix stream socket
//
// Protocol (identical payload to the Windows WM_COPYDATA path):
//   secondary -> primary : {"argv": ["--show", ...]}   then shutdown(SHUT_WR)
//   primary  -> secondary: "1" (accepted) or "0" (rejected)
//
// The lock is advisory but authoritative: only the process holding it may
// (re)create the socket, so a stale socket left by a crash is simply
// unlinked and replaced by the next primary. flock() locks die with the
// process, so a crash can never wedge the next launch.
// ---------------------------------------------------------------------------
#include "platform/SingleInstance.h"

#include "core/Logger.h"
#include "platform/KnownFolders.h"
#include "platform/Time.h"
#include "platform/Utf.h"
#include "platform/posix/PosixCommon.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hh::platform {

using posix::closeQuietly;

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"SingleInstance";

/// Largest argv document accepted from a secondary.
constexpr size_t kMaxPayloadBytes = 1u * 1024u * 1024u;

/// Connect retries while the primary finishes starting (same budget as Windows).
constexpr int kConnectRetries = 20;
constexpr int kConnectRetryDelayMs = 100;

/// How long either side waits for the other before giving up.
constexpr int kIoTimeoutMs = 5000;

/// The app-support folder as UTF-8 (created on demand).
std::string supportFolder() {
    return posix::toNative(appLocalDataFolder());
}

/// Fills a sockaddr_un; false when the path does not fit (104 bytes on macOS).
bool makeAddress(const std::string& path, sockaddr_un& addr) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

/// {"argv": [...]} as UTF-8.
std::string encodeArgs(const std::vector<std::wstring>& args) {
    nlohmann::json root = nlohmann::json::object();
    nlohmann::json list = nlohmann::json::array();
    for (const std::wstring& a : args) {
        list.push_back(toUtf8(a));
    }
    root["argv"] = std::move(list);
    return root.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// Parses {"argv": [...]}; false for anything else.
bool decodeArgs(const std::string& payload, std::vector<std::wstring>& out) {
    const nlohmann::json root = nlohmann::json::parse(payload, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return false;
    }
    const auto it = root.find("argv");
    if (it == root.end() || !it->is_array()) {
        return false;
    }
    out.clear();
    for (const auto& item : *it) {
        if (item.is_string()) {
            out.push_back(toWide(item.get<std::string>()));
        }
    }
    return true;
}

/// Waits for @p events on one descriptor; false on timeout or error.
bool waitFor(int fd, short events, int timeoutMs) {
    pollfd p{};
    p.fd = fd;
    p.events = events;
    for (;;) {
        const int r = ::poll(&p, 1, timeoutMs);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return r > 0 && (p.revents & (events | POLLHUP)) != 0;
    }
}

/// Reads until EOF (or the cap / timeout); false when nothing usable arrived.
bool readAll(int fd, std::string& out) {
    char buffer[4096];
    const uint64_t deadline = nowMonotonicMs() + static_cast<uint64_t>(kIoTimeoutMs);
    for (;;) {
        const uint64_t now = nowMonotonicMs();
        if (now >= deadline || !waitFor(fd, POLLIN, static_cast<int>(deadline - now))) {
            return !out.empty();
        }
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        if (n <= 0) {
            return !out.empty();
        }
        out.append(buffer, static_cast<size_t>(n));
        if (out.size() > kMaxPayloadBytes) {
            return false;
        }
    }
}

/// Writes every byte; false on error / timeout.
bool writeAll(int fd, const std::string& bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        if (!waitFor(fd, POLLOUT, kIoTimeoutMs)) {
            return false;
        }
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

/// Sockets must never raise SIGPIPE on a vanished peer.
void noSigPipe(int fd) {
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    static_cast<void>(fd);
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

SingleInstance::~SingleInstance() {
    if (source_ != nullptr) {
        auto source = static_cast<dispatch_source_t>(source_);
        ::dispatch_source_cancel(source);
        ::dispatch_release(source);
        source_ = nullptr;
    }
    closeQuietly(listenFd_);
    // Only the primary created the socket, so only it removes it.
    if (primary_ && !socketPath_.empty()) {
        ::unlink(socketPath_.c_str());
    }
    closeQuietly(lockFd_);
}

/**
 * @brief flock() the instance lock without blocking.
 */
bool SingleInstance::acquire() {
    if (primary_) {
        return true;
    }
    const std::string folder = supportFolder();
    if (folder.empty()) {
        HH_LOG_ERROR(kLog, L"no support folder; running without single-instance protection");
        primary_ = true;
        return true;
    }
    const std::string lockPath = folder + "/instance.lock";
    lockFd_ = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockFd_ < 0) {
        HH_LOG_WARN(kLog, L"cannot open {}: {}; running without single-instance protection", toWide(lockPath),
                    Error::fromErrno(errno, L"open").toString());
        primary_ = true;
        return true;
    }
    if (::flock(lockFd_, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        closeQuietly(lockFd_);
        if (err == EWOULDBLOCK) {
            HH_LOG_INFO(kLog, L"another instance holds the lock; this one is secondary");
            return false;
        }
        HH_LOG_WARN(kLog, L"flock failed ({}); running as primary", Error::fromErrno(err, L"flock").toString());
        primary_ = true;
        return true;
    }
    primary_ = true;
    HH_LOG_INFO(kLog, L"acquired the instance lock (pid {})", static_cast<int>(::getpid()));
    return true;
}

/**
 * @brief Listens on the instance socket; forwarded argv arrive on the main queue.
 */
bool SingleInstance::registerReceiver(std::function<void(const std::vector<std::wstring>&)> onArgs) {
    if (!primary_) {
        HH_LOG_WARN(kLog, L"registerReceiver() on a secondary instance");
        return false;
    }
    if (listenFd_ >= 0) {
        onArgs_ = std::move(onArgs);
        return true;
    }
    onArgs_ = std::move(onArgs);

    socketPath_ = supportFolder() + "/instance.sock";
    sockaddr_un addr{};
    if (!makeAddress(socketPath_, addr)) {
        HH_LOG_ERROR(kLog, L"socket path too long: {}", toWide(socketPath_));
        socketPath_.clear();
        return false;
    }
    // We hold the lock, so anything at that path is a leftover from a crash.
    ::unlink(socketPath_.c_str());

    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        HH_LOG_ERROR(kLog, L"socket: {}", Error::fromErrno(errno, L"socket").toString());
        return false;
    }
    ::fcntl(listenFd_, F_SETFD, FD_CLOEXEC);
    ::fcntl(listenFd_, F_SETFL, ::fcntl(listenFd_, F_GETFL, 0) | O_NONBLOCK);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listenFd_, 8) != 0) {
        HH_LOG_ERROR(kLog, L"bind/listen {}: {}", toWide(socketPath_), Error::fromErrno(errno, L"bind").toString());
        closeQuietly(listenFd_);
        return false;
    }
    ::chmod(socketPath_.c_str(), 0600);   // this user only, like the Windows DACL

    // A read source on the main queue: accept + parse + callback on the UI thread.
    dispatch_source_t source = ::dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(listenFd_), 0,
                                                        ::dispatch_get_main_queue());
    if (source == nullptr) {
        HH_LOG_ERROR(kLog, L"dispatch_source_create failed");
        closeQuietly(listenFd_);
        return false;
    }
    ::dispatch_set_context(source, this);
    ::dispatch_source_set_event_handler_f(source, &SingleInstance::onReadable);
    ::dispatch_resume(source);
    source_ = source;
    HH_LOG_INFO(kLog, L"receiving forwarded arguments on {}", toWide(socketPath_));
    return true;
}

/**
 * @brief Accepts every pending client and hands its argv to onArgs_.
 */
void SingleInstance::onReadable(void* context) {
    auto* self = static_cast<SingleInstance*>(context);
    if (self == nullptr || self->listenFd_ < 0) {
        return;
    }
    for (;;) {
        int client = ::accept(self->listenFd_, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;   // EAGAIN: no more clients queued
        }
        ::fcntl(client, F_SETFD, FD_CLOEXEC);
        noSigPipe(client);

        std::string payload;
        std::vector<std::wstring> args;
        const bool ok = readAll(client, payload) && decodeArgs(payload, args);
        writeAll(client, ok ? "1" : "0");
        ::close(client);

        if (!ok) {
            HH_LOG_WARN(kLog, L"ignoring a forwarding client with a payload that is not {{\"argv\":[...]}}");
            continue;
        }
        HH_LOG_INFO(kLog, L"received {} forwarded argument(s)", args.size());
        if (self->onArgs_) {
            self->onArgs_(args);
        }
    }
}

/**
 * @brief Connects to the primary (retrying while it starts) and sends argv.
 */
bool SingleInstance::forwardToPrimary(const std::vector<std::wstring>& args) {
    const std::string path = supportFolder() + "/instance.sock";
    sockaddr_un addr{};
    if (!makeAddress(path, addr)) {
        HH_LOG_ERROR(kLog, L"socket path too long: {}", toWide(path));
        return false;
    }

    int fd = -1;
    for (int attempt = 0; attempt < kConnectRetries; ++attempt) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        noSigPipe(fd);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(kConnectRetryDelayMs));
    }
    if (fd < 0) {
        HH_LOG_WARN(kLog, L"no primary listening on {} after {} attempts", toWide(path), kConnectRetries);
        return false;
    }

    const std::string payload = encodeArgs(args);
    bool ok = !payload.empty() && payload.size() <= kMaxPayloadBytes && writeAll(fd, payload);
    ::shutdown(fd, SHUT_WR);   // EOF tells the primary the document is complete
    std::string answer;
    ok = ok && readAll(fd, answer) && answer == "1";
    ::close(fd);
    if (ok) {
        HH_LOG_INFO(kLog, L"forwarded {} argument(s) to the primary", args.size());
    } else {
        HH_LOG_WARN(kLog, L"the primary did not accept the forwarded argv");
    }
    return ok;
}

} // namespace hh::platform
