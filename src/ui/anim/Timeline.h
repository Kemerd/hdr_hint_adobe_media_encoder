// ---------------------------------------------------------------------------
// Timeline.h - one clock, all animations and timers of a window.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace hh::ui {

/// Anything sampled once per frame.
class IAnimation {
public:
    virtual ~IAnimation() = default;
    /// Samples at @p now; returns true while still active.
    virtual bool sample(double now) = 0;
};

using TimerId = uint64_t;

/**
 * @brief Per-window animation clock.
 *
 * tick() samples a snapshot of the active list (animations may add/remove
 * themselves during sampling), then runs due timers, then fires completion
 * callbacks queued by animations. pause()/resume() shift the clock so hidden
 * windows do not "jump" when shown again.
 */
class Timeline {
public:
    Timeline();

    /// Current animation time in seconds (monotonic, paused-time excluded).
    [[nodiscard]] double now() const;

    void add(IAnimation* a);
    void remove(IAnimation* a);
    [[nodiscard]] bool hasActive() const noexcept { return !active_.empty(); }

    /// Samples everything. Returns true when a redraw is needed.
    bool tick();

    /// Runs @p fn once at @p atSeconds (timeline time). Returns an id for cancel.
    TimerId addTimer(double atSeconds, std::function<void()> fn);
    /// Runs @p fn after @p delaySeconds from now.
    TimerId addTimerIn(double delaySeconds, std::function<void()> fn) { return addTimer(now() + delaySeconds, std::move(fn)); }
    void cancelTimer(TimerId id);
    /// Earliest pending timer deadline (timeline seconds), if any.
    [[nodiscard]] std::optional<double> nextDeadline() const;

    /// Queues a callback to run after the current sampling pass (safe from inside sample()).
    void defer(std::function<void()> fn);

    void setReducedMotion(bool reduced) noexcept { reducedMotion_ = reduced; }
    [[nodiscard]] bool reducedMotion() const noexcept { return reducedMotion_; }

    void pause();
    void resume();
    [[nodiscard]] bool paused() const noexcept { return paused_; }

private:
    struct Timer { TimerId id; double at; std::function<void()> fn; };

    std::vector<IAnimation*> active_;
    std::vector<Timer> timers_;
    std::vector<std::function<void()>> deferred_;
    TimerId nextTimer_ = 1;
    bool reducedMotion_ = false;
    bool paused_ = false;
    double pausedAt_ = 0;
    double offset_ = 0;         ///< subtracted from the monotonic clock
    bool ticking_ = false;
};

} // namespace hh::ui
