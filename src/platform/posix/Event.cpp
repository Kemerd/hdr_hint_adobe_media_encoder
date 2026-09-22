// ---------------------------------------------------------------------------
// posix/Event.cpp - platform::Event and waitAny on top of pipes and poll().
//
// Model
//   Every Event owns a non-blocking pipe. While the event is signalled the
//   pipe holds exactly one byte, so "readable" == "signalled" and any number
//   of events, sockets and child pipes can be waited on together with one
//   poll() call - the POSIX spelling of WaitForMultipleObjects.
//
// Auto-reset
//   Win32 consumes an auto-reset event as part of the wait that reports it.
//   poll() cannot do that, so auto-reset events register their read
//   descriptor in a small table; waitAny() looks the winning descriptor up
//   and resets that event before returning. If another thread consumed it
//   first, the wake-up was spurious and the wait simply continues.
// ---------------------------------------------------------------------------
#include "platform/Event.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace hh::platform {

// ===========================================================================
// Event state
// ===========================================================================

/**
 * @brief The pipe pair plus the signalled flag, guarded by one mutex.
 *
 * The invariant "pipe holds one byte <=> signalled" is only ever changed with
 * the mutex held, so set() and reset() racing from two threads can never
 * leave a lost wake-up or a stale byte behind.
 */
struct Event::State {
    std::mutex mutex;          ///< guards signalled + the pipe contents
    int readFd = -1;           ///< the end poll() watches
    int writeFd = -1;          ///< the end set() writes to
    bool manualReset = true;   ///< false = consumed by the wait that reports it
    bool signalled = false;    ///< current state

    ~State();

    /// Makes the event signalled (idempotent). Caller holds the mutex.
    void signalLocked() noexcept;
    /// Makes the event non-signalled (idempotent). Caller holds the mutex.
    void clearLocked() noexcept;
};

namespace {

// ---------------------------------------------------------------------------
// Auto-reset registry: read descriptor -> owning state
// ---------------------------------------------------------------------------

/// Guards the registry. Lock order: registry mutex first, then a state's mutex.
std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}

/// Descriptors of every live auto-reset event.
std::unordered_map<int, Event::State*>& registry() {
    static std::unordered_map<int, Event::State*> map;
    return map;
}

/**
 * @brief Marks a descriptor non-blocking and close-on-exec.
 *
 * Both flags matter: a blocking read would hang the drain in reset(), and an
 * inherited descriptor would keep a child process holding our event.
 */
bool configureDescriptor(int fd) noexcept {
    if (fd < 0) {
        return false;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    const int fdFlags = ::fcntl(fd, F_GETFD, 0);
    if (fdFlags < 0 || ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) < 0) {
        return false;
    }
    return true;
}

/**
 * @brief Reads everything currently buffered in a non-blocking pipe.
 */
void drainPipe(int fd) noexcept {
    if (fd < 0) {
        return;
    }
    char sink[64];
    for (;;) {
        const ssize_t n = ::read(fd, sink, sizeof(sink));
        if (n > 0) {
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;   // 0 (EOF) or EAGAIN: nothing left
    }
}

/**
 * @brief Consumes an auto-reset event reported ready by poll().
 *
 * @return true when the descriptor is not an auto-reset event (nothing to
 *         consume) or when this call consumed the signal; false when another
 *         waiter got there first and the wake-up must be ignored
 */
bool consumeIfAutoReset(int fd) noexcept {
    std::lock_guard<std::mutex> registryLock(registryMutex());
    const auto it = registry().find(fd);
    if (it == registry().end() || it->second == nullptr) {
        return true;   // a socket, a pipe, a manual-reset event: never consumed by the wait
    }
    Event::State* state = it->second;
    std::lock_guard<std::mutex> stateLock(state->mutex);
    if (!state->signalled) {
        return false;  // raced with another waiter (or a reset()); keep waiting
    }
    state->clearLocked();
    return true;
}

/// Milliseconds left of a timeout, or -1 for "forever" (poll's convention).
int remainingMs(DWORD timeoutMs, std::chrono::steady_clock::time_point start) noexcept {
    if (timeoutMs == INFINITE) {
        return -1;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (elapsed < 0) {
        return static_cast<int>(std::min<DWORD>(timeoutMs, static_cast<DWORD>(INT_MAX)));
    }
    const uint64_t used = static_cast<uint64_t>(elapsed);
    if (used >= timeoutMs) {
        return 0;
    }
    return static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(timeoutMs) - used, static_cast<uint64_t>(INT_MAX)));
}

} // namespace

// ---------------------------------------------------------------------------
// State members
// ---------------------------------------------------------------------------

Event::State::~State() {
    // Leave the registry first so no waiter can reach a half-destroyed state.
    if (!manualReset && readFd >= 0) {
        std::lock_guard<std::mutex> lock(registryMutex());
        const auto it = registry().find(readFd);
        if (it != registry().end() && it->second == this) {
            registry().erase(it);
        }
    }
    if (readFd >= 0) {
        ::close(readFd);
        readFd = -1;
    }
    if (writeFd >= 0) {
        ::close(writeFd);
        writeFd = -1;
    }
}

void Event::State::signalLocked() noexcept {
    if (signalled || writeFd < 0) {
        return;
    }
    // One byte is the whole protocol; a full pipe is impossible at one byte.
    const char byte = 1;
    ssize_t n = -1;
    do {
        n = ::write(writeFd, &byte, 1);
    } while (n < 0 && errno == EINTR);
    signalled = (n == 1);
}

void Event::State::clearLocked() noexcept {
    if (!signalled) {
        return;
    }
    drainPipe(readFd);
    signalled = false;
}

// ===========================================================================
// Event
// ===========================================================================

Event::Event() noexcept = default;
Event::~Event() = default;
Event::Event(Event&& other) noexcept = default;
Event& Event::operator=(Event&& other) noexcept = default;

/**
 * @brief Creates the pipe pair and registers auto-reset events.
 */
bool Event::create(bool manualReset, bool initialState) {
    close();

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return false;
    }
    auto state = std::make_unique<State>();
    state->readFd = fds[0];
    state->writeFd = fds[1];
    state->manualReset = manualReset;

    // Both ends must be non-blocking and close-on-exec before anyone sees them.
    if (!configureDescriptor(state->readFd) || !configureDescriptor(state->writeFd)) {
        return false;   // the state's destructor closes both descriptors
    }

    // Auto-reset events are consumed by waitAny(); it finds them through the registry.
    if (!manualReset) {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry()[state->readFd] = state.get();
    }

    if (initialState) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->signalLocked();
    }
    state_ = std::move(state);
    return true;
}

void Event::close() noexcept {
    state_.reset();
}

void Event::set() noexcept {
    if (!state_) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->signalLocked();
}

void Event::reset() noexcept {
    if (!state_) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->clearLocked();
}

bool Event::wait(DWORD timeoutMs) noexcept {
    if (!state_ || state_->readFd < 0) {
        return false;
    }
    const WaitHandle h = state_->readFd;
    return waitAny(&h, 1, timeoutMs) == 0;
}

WaitHandle Event::handle() const noexcept {
    return state_ ? state_->readFd : kInvalidWaitHandle;
}

bool Event::valid() const noexcept {
    return state_ && state_->readFd >= 0 && state_->writeFd >= 0;
}

Event makeWaitableEvent(bool manualReset, bool initialState) {
    Event e;
    e.create(manualReset, initialState);
    return e;
}

// ===========================================================================
// Waiting
// ===========================================================================

/**
 * @brief poll() over every descriptor; lowest ready index wins.
 *
 * POLLHUP / POLLERR count as "ready": the owner's next read reports the
 * disconnect or error, which is how a closed pipe or socket surfaces.
 * POLLNVAL (a descriptor that is not open) fails the wait, matching
 * WaitForMultipleObjects' ERROR_INVALID_HANDLE.
 */
DWORD waitAny(const WaitHandle* handles, size_t count, DWORD timeoutMs) {
    // Guard the inputs before touching the kernel.
    if (handles == nullptr || count == 0 || count > kMaxWaitHandles) {
        errno = EINVAL;
        return kWaitFailed;
    }
    pollfd fds[kMaxWaitHandles] = {};
    for (size_t i = 0; i < count; ++i) {
        if (handles[i] < 0) {
            errno = EBADF;
            return kWaitFailed;
        }
        fds[i].fd = handles[i];
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }

    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const int timeout = remainingMs(timeoutMs, start);
        const int n = ::poll(fds, static_cast<nfds_t>(count), timeout);
        if (n < 0) {
            if (errno == EINTR) {
                continue;   // a signal arrived; the deadline still holds
            }
            return kWaitFailed;
        }
        if (n == 0) {
            return kWaitTimeout;
        }

        // Report the lowest ready descriptor, like WaitForMultipleObjects.
        for (size_t i = 0; i < count; ++i) {
            const short re = fds[i].revents;
            if (re == 0) {
                continue;
            }
            if ((re & POLLNVAL) != 0) {
                errno = EBADF;
                return kWaitFailed;
            }
            if (consumeIfAutoReset(fds[i].fd)) {
                return static_cast<DWORD>(i);
            }
            // Lost the race for an auto-reset event: ignore that descriptor
            // for this round and look at the others.
        }

        // Everything that fired was consumed elsewhere; wait again with
        // whatever time is left (a zero timeout ends the loop above).
        if (timeoutMs != INFINITE && remainingMs(timeoutMs, start) == 0) {
            return kWaitTimeout;
        }
        for (size_t i = 0; i < count; ++i) {
            fds[i].revents = 0;
        }
    }
}

bool isSignalled(WaitHandle handle) {
    if (handle < 0) {
        return false;
    }
    return waitAny(&handle, 1, 0) == 0;
}

} // namespace hh::platform
