// ---------------------------------------------------------------------------
// FramePacer.h - "does the window need a frame?" flag with coalescing.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>

namespace hh::ui {

class FramePacer {
public:
    void requestFrame() noexcept { needed_.store(true, std::memory_order_relaxed); }
    /// Returns true (and clears) when a frame was requested.
    bool consume() noexcept { return needed_.exchange(false, std::memory_order_relaxed); }
    [[nodiscard]] bool needsFrame() const noexcept { return needed_.load(std::memory_order_relaxed); }
private:
    std::atomic<bool> needed_{true};
};

} // namespace hh::ui
