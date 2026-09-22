// ---------------------------------------------------------------------------
// EventQueue.cpp - thread-safe queue with a "kick" to the UI thread.
//
// Kick coalescing: kickPending_ is set by the first pusher and cleared by the
// drainer *before* it swaps the queue out. A push that lands between the
// clear and the swap therefore posts another kick, so nothing ever sits in
// the queue without a pending message.
// ---------------------------------------------------------------------------
#include "core/EventQueue.h"

#include <utility>

namespace hh {

/**
 * @brief Sets (or changes) the window and message used for kicks.
 *
 * Events posted before a target existed are still queued; if there are any,
 * a kick is issued right away so the new window drains them.
 */
#if defined(_WIN32)
void EngineEventQueue::setTarget(HWND hwnd, UINT message) {
    hwnd_.store(hwnd, std::memory_order_release);
    message_.store(message, std::memory_order_release);

    // Anything waiting from before the window existed needs a kick now.
    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = !queue_.empty();
    }
    if (pending) {
        kick();
    }
}
#endif

/**
 * @brief Sets (or changes) the function used for kicks.
 *
 * The function is held by shared_ptr so a worker that already copied it can
 * finish its call while the UI thread swaps in a new one.
 */
void EngineEventQueue::setTarget(EngineKick kick) {
    {
        std::lock_guard<std::mutex> lock(kickMutex_);
        kickFn_ = kick ? std::make_shared<const EngineKick>(std::move(kick)) : nullptr;
    }

    // Anything waiting from before the target existed needs a kick now.
    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = !queue_.empty();
    }
    if (pending) {
        kickPending_.store(false, std::memory_order_release);
        kick();
    }
}

/**
 * @brief Appends an event (any thread) and makes sure a kick is in flight.
 */
void EngineEventQueue::post(EngineEvent&& event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(event));
    }
    kick();
}

/**
 * @brief Takes everything queued. Clears the kick flag first (see the class
 *        comment) so a concurrent push always produces a fresh kick.
 */
std::deque<EngineEvent> EngineEventQueue::drain() {
    kickPending_.store(false, std::memory_order_release);

    std::deque<EngineEvent> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(queue_);
    }
    return out;
}

/**
 * @brief Number of queued events (approximate: a push may race the read).
 */
size_t EngineEventQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

/**
 * @brief Posts the kick message unless one is already pending.
 *
 * Without a target (or when PostMessageW fails, e.g. the queue is full or the
 * window is gone) the flag is reset so a later push or setTarget() retries.
 */
void EngineEventQueue::kick() {
    // exchange() returns the previous value: true means a kick is already out.
    if (kickPending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    // A function target wins when one is set (the only kind on POSIX).
    std::shared_ptr<const EngineKick> fn;
    {
        std::lock_guard<std::mutex> lock(kickMutex_);
        fn = kickFn_;
    }
    if (fn && *fn) {
        if (!(*fn)()) {
            kickPending_.store(false, std::memory_order_release);
        }
        return;
    }

#if defined(_WIN32)
    const HWND hwnd = hwnd_.load(std::memory_order_acquire);
    const UINT message = message_.load(std::memory_order_acquire);
    if (hwnd == nullptr || message == 0) {
        kickPending_.store(false, std::memory_order_release);
        return;
    }

    if (!::PostMessageW(hwnd, message, 0, 0)) {
        kickPending_.store(false, std::memory_order_release);
    }
#else
    // No target yet: let the next post (or setTarget) retry.
    kickPending_.store(false, std::memory_order_release);
#endif
}

} // namespace hh
