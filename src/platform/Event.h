// ---------------------------------------------------------------------------
// Event.h - waitable events and "wait for any of these" for worker threads.
//
// Every engine worker sleeps on a small set of things at once: its own stop
// and wake events, plus whatever I/O it owns (directory watches, pipe
// instances, a child's output pipe). This header is the one portable way to
// express that:
//
//     platform::Event stop, wake;
//     stop.create(true);                 // manual-reset
//     wake.create(false);                // auto-reset
//     const WaitHandle set[] = {stop.handle(), wake.handle(), watch.event()};
//     const DWORD r = platform::waitAny(set, 3, 250);
//     if (r == 0) { ...stop... } else if (r == platform::kWaitTimeout) { ... }
//
// Windows  WaitHandle is a kernel HANDLE; Event wraps CreateEventW and
//          waitAny is WaitForMultipleObjects (the lowest signalled index wins,
//          auto-reset events are consumed by the wait).
// POSIX    WaitHandle is a pollable file descriptor; Event is a non-blocking
//          pipe that holds exactly one byte while signalled, and waitAny is
//          poll(). The same rules apply: the lowest ready index is reported
//          and, when that descriptor belongs to an auto-reset Event, the
//          event is reset as part of the wait - exactly what Win32 does.
//          Sockets, pipes and the like report "readable" and are never
//          consumed by the wait; their owners read them.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace hh::platform {

// ===========================================================================
// Wait handles
// ===========================================================================

#if defined(_WIN32)
/// A kernel object that can be waited on (event, process, change handle...).
using WaitHandle = HANDLE;
/// The "no handle" value.
inline constexpr WaitHandle kInvalidWaitHandle = nullptr;
#else
/// A pollable file descriptor (event pipe, socket, child output pipe...).
using WaitHandle = int;
/// The "no descriptor" value.
inline constexpr WaitHandle kInvalidWaitHandle = -1;
#endif

/// waitAny() / Event::wait() result when nothing fired within the timeout.
inline constexpr DWORD kWaitTimeout = 0x00000102u;     // == WAIT_TIMEOUT
/// waitAny() / Event::wait() result when the wait itself failed (bad handle).
inline constexpr DWORD kWaitFailed = 0xFFFFFFFFu;      // == WAIT_FAILED
/// Largest number of handles one waitAny() accepts (MAXIMUM_WAIT_OBJECTS).
inline constexpr size_t kMaxWaitHandles = 64;

/**
 * @brief Blocks until one of @p handles is signalled or @p timeoutMs elapses.
 *
 * @param handles    array of @p count handles; invalid entries fail the wait
 * @param count      1..kMaxWaitHandles
 * @param timeoutMs  milliseconds, 0 = poll, INFINITE = forever
 * @return the zero-based index of the lowest signalled handle, kWaitTimeout,
 *         or kWaitFailed (count out of range, bad handle, OS error)
 */
DWORD waitAny(const WaitHandle* handles, size_t count, DWORD timeoutMs);

/**
 * @brief Zero-timeout check of a single handle.
 *
 * Consumes an auto-reset Event exactly like WaitForSingleObject(h, 0) does.
 * @return true when the handle was signalled
 */
bool isSignalled(WaitHandle handle);

// ===========================================================================
// Event
// ===========================================================================

/**
 * @brief A manual- or auto-reset event, movable, closed on destruction.
 *
 * All member functions are thread-safe with respect to each other: set() is
 * called from the UI thread while a worker sits in waitAny() on handle().
 */
class Event {
public:
    Event() noexcept;
    ~Event();
    Event(Event&& other) noexcept;
    Event& operator=(Event&& other) noexcept;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    /**
     * @brief Creates the underlying OS object (closing any previous one).
     * @param manualReset   true: stays signalled until reset(); false: one waiter consumes it
     * @param initialState  start signalled
     * @return false when the OS refused (the event stays invalid)
     */
    bool create(bool manualReset, bool initialState = false);

    /// Releases the OS object; the event becomes invalid.
    void close() noexcept;

    /// Signals the event. No effect on an invalid event.
    void set() noexcept;
    /// Clears the signalled state. No effect on an invalid event.
    void reset() noexcept;

    /**
     * @brief Waits for this event alone.
     * @return true when it was signalled within @p timeoutMs (auto-reset: consumed)
     */
    bool wait(DWORD timeoutMs) noexcept;

    /// Zero-timeout wait; consumes an auto-reset event.
    [[nodiscard]] bool isSet() noexcept { return wait(0); }

    /// The handle to put in a waitAny() set (kInvalidWaitHandle when invalid).
    [[nodiscard]] WaitHandle handle() const noexcept;

    /// True after a successful create() until close().
    [[nodiscard]] bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }

    /// Opaque per-platform state (defined in the platform's Event source).
    struct State;

private:
    std::unique_ptr<State> state_;
};

/**
 * @brief Convenience factory mirroring the old makeEvent() helper.
 * @return a valid Event, or an invalid one when the OS refused
 */
Event makeWaitableEvent(bool manualReset, bool initialState = false);

} // namespace hh::platform
