// ---------------------------------------------------------------------------
// Event.cpp - Windows implementation of platform::Event / waitAny.
//
// A thin veneer over CreateEventW and WaitForMultipleObjects: the portable
// API was shaped after these two calls, so nothing here changes behaviour.
// ---------------------------------------------------------------------------
#include "platform/Event.h"

#include "platform/Handle.h"

#include <utility>

namespace hh::platform {

// ===========================================================================
// Event state
// ===========================================================================

/**
 * @brief The owned kernel event.
 */
struct Event::State {
    UniqueHandle handle;   ///< the CreateEventW handle (closed with the state)
};

Event::Event() noexcept = default;
Event::~Event() = default;
Event::Event(Event&& other) noexcept = default;
Event& Event::operator=(Event&& other) noexcept = default;

/**
 * @brief Creates an unnamed event with the default security descriptor.
 */
bool Event::create(bool manualReset, bool initialState) {
    // Replacing an existing event closes it first, exactly like reassigning a UniqueHandle.
    close();
    UniqueHandle h(::CreateEventW(nullptr, manualReset ? TRUE : FALSE, initialState ? TRUE : FALSE, nullptr));
    if (!h) {
        return false;
    }
    auto state = std::make_unique<State>();
    state->handle = std::move(h);
    state_ = std::move(state);
    return true;
}

void Event::close() noexcept {
    state_.reset();
}

void Event::set() noexcept {
    if (state_ && state_->handle) {
        ::SetEvent(state_->handle.get());
    }
}

void Event::reset() noexcept {
    if (state_ && state_->handle) {
        ::ResetEvent(state_->handle.get());
    }
}

bool Event::wait(DWORD timeoutMs) noexcept {
    if (!state_ || !state_->handle) {
        return false;
    }
    return ::WaitForSingleObject(state_->handle.get(), timeoutMs) == WAIT_OBJECT_0;
}

WaitHandle Event::handle() const noexcept {
    return (state_ && state_->handle) ? state_->handle.get() : kInvalidWaitHandle;
}

bool Event::valid() const noexcept {
    return state_ && state_->handle.valid();
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
 * @brief WaitForMultipleObjects with the result folded into an index.
 *
 * Abandoned-mutex results are reported as their index too: callers only ever
 * wait on events, pipes and change handles, none of which can be abandoned,
 * and treating the rare abandoned mutex as "signalled" is what every caller
 * would do anyway.
 */
DWORD waitAny(const WaitHandle* handles, size_t count, DWORD timeoutMs) {
    // Guard the inputs before handing them to the kernel.
    if (handles == nullptr || count == 0 || count > kMaxWaitHandles) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return kWaitFailed;
    }
    const DWORD r = ::WaitForMultipleObjects(static_cast<DWORD>(count), handles, FALSE, timeoutMs);
    if (r == WAIT_TIMEOUT) {
        return kWaitTimeout;
    }
    if (r == WAIT_FAILED) {
        return kWaitFailed;
    }
    // Signalled: WAIT_OBJECT_0 + i.
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count) {
        return r - WAIT_OBJECT_0;
    }
    // Abandoned: WAIT_ABANDONED_0 + i.
    if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + count) {
        return r - WAIT_ABANDONED_0;
    }
    return kWaitFailed;
}

bool isSignalled(WaitHandle handle) {
    if (handle == kInvalidWaitHandle || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    return ::WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

} // namespace hh::platform
