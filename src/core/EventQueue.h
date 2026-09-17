// ---------------------------------------------------------------------------
// EventQueue.h - thread-safe queue with a PostMessage "kick" to the UI thread.
// ---------------------------------------------------------------------------
#pragma once

#include "core/EngineEvents.h"
#include "platform/Win.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <utility>

namespace hh {

/**
 * @brief Workers push; the UI thread drains on the WM_APP kick message.
 *
 * Kick coalescing: only one kick is in flight at a time. The drain side must
 * clear the flag *before* swapping the queue so a push that races the drain
 * always produces another kick.
 */
class EngineEventQueue final : public IEngineSink {
public:
    /// Sets (or changes) the window + message used for kicks.
    void setTarget(HWND hwnd, UINT message);

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
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<UINT> message_{0};
};

} // namespace hh
