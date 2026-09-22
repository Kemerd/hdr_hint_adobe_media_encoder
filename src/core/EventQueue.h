// ---------------------------------------------------------------------------
// EventQueue.h - thread-safe queue with a "kick" to the UI thread.
//
// Windows kicks with PostMessage(hwnd, WM_APP + n). macOS kicks with a
// function supplied by the app (dispatch_async onto the main queue). Either
// way the UI thread ends up calling Engine::onEventMessage().
// ---------------------------------------------------------------------------
#pragma once

#include "core/EngineEvents.h"
#include "platform/Win.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace hh {

/**
 * @brief Wakes the UI thread so it drains the queue.
 *
 * Called from worker threads; must be cheap, must not block and must be
 * safe to call concurrently. Returns false when the wake-up could not be
 * delivered (the queue then retries on the next post).
 */
using EngineKick = std::function<bool()>;

/**
 * @brief Workers push; the UI thread drains on the kick.
 *
 * Kick coalescing: only one kick is in flight at a time. The drain side must
 * clear the flag *before* swapping the queue so a push that races the drain
 * always produces another kick.
 */
class EngineEventQueue final : public IEngineSink {
public:
#if defined(_WIN32)
    /// Sets (or changes) the window + message used for kicks.
    void setTarget(HWND hwnd, UINT message);
#endif
    /// Sets (or changes) the function used for kicks (any platform).
    void setTarget(EngineKick kick);

    void post(EngineEvent&& event) override;

    /// Takes everything queued (call from the UI thread after the kick).
    std::deque<EngineEvent> drain();

    /// Number of queued events (approximate).
    [[nodiscard]] size_t size() const;

private:
    void kick();

    mutable std::mutex mutex_;
    std::deque<EngineEvent> queue_;
    std::atomic<bool> kickPending_{false};
#if defined(_WIN32)
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<UINT> message_{0};
#endif
    /// Function-based kick; swapped under kickMutex_, called outside it.
    mutable std::mutex kickMutex_;
    std::shared_ptr<const EngineKick> kickFn_;
};

} // namespace hh
