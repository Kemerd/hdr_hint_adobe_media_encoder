// ---------------------------------------------------------------------------
// Timeline.cpp - the per-window animation clock.
//
// One tick does three things in a fixed order:
//   1. sample every registered animation at one shared "now"
//   2. run the timers that are due (ascending deadline, FIFO for ties)
//   3. run callbacks queued with defer() (spring completions and the like)
//
// Animations may add/remove themselves while being sampled, timers may
// cancel each other from inside a callback, and the whole thing survives a
// pause/resume without any spring jumping forward.
// ---------------------------------------------------------------------------
#include "ui/anim/Timeline.h"

#include "core/Logger.h"
#include "platform/Time.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

/// Log component tag.
constexpr const wchar_t* kLog = L"Timeline";

/// Deferred callbacks that queue more deferred work are drained for at most
/// this many rounds per tick; anything left over runs on the next tick.
constexpr int kMaxDeferredRounds = 8;

} // namespace

// ---------------------------------------------------------------------------
// Construction / clock
// ---------------------------------------------------------------------------

/**
 * @brief Reserves a little capacity so the first animations never allocate mid-frame.
 */
Timeline::Timeline()
{
    active_.reserve(16);
    timers_.reserve(8);
    deferred_.reserve(8);
}

/**
 * @brief Current timeline time in seconds.
 *
 * The monotonic clock minus the accumulated paused duration. While paused
 * the raw clock is frozen at pausedAt_, so springs sampled by a hidden
 * window (screenshot, forced repaint) see no time passing.
 */
double Timeline::now() const
{
    const double raw = paused_ ? pausedAt_ : platform::nowMonotonicSeconds();
    return raw - offset_;
}

/**
 * @brief Freezes the clock. Safe to call repeatedly.
 */
void Timeline::pause()
{
    if (paused_) {
        return;
    }
    paused_ = true;
    pausedAt_ = platform::nowMonotonicSeconds();
}

/**
 * @brief Unfreezes the clock so now() continues from where pause() left it.
 *
 * The time spent paused is folded into offset_, which is why every spring
 * (whose t0 is timeline time) resumes seamlessly instead of jumping.
 */
void Timeline::resume()
{
    if (!paused_) {
        return;
    }
    const double raw = platform::nowMonotonicSeconds();
    const double pausedFor = std::max(0.0, raw - pausedAt_);
    offset_ += pausedFor;
    paused_ = false;
}

// ---------------------------------------------------------------------------
// Animations
// ---------------------------------------------------------------------------

/**
 * @brief Registers an animation for sampling. Duplicates are ignored.
 *
 * Adding during tick() is fine: the sampling loop walks a snapshot, so a
 * freshly added animation is first sampled on the next tick.
 */
void Timeline::add(IAnimation* a)
{
    if (!a) {
        HH_LOG_WARN(kLog, L"add: null animation ignored");
        return;
    }
    if (std::find(active_.begin(), active_.end(), a) != active_.end()) {
        return;
    }
    active_.push_back(a);
}

/**
 * @brief Unregisters an animation. Safe from inside sample() and destructors.
 *
 * tick() re-checks membership in active_ before every sample, so removing
 * an entry mid-pass guarantees it is never touched again in that pass.
 */
void Timeline::remove(IAnimation* a)
{
    if (!a) {
        return;
    }
    std::erase(active_, a);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

/**
 * @brief Samples animations, fires due timers, runs deferred callbacks.
 * @return true when anything ran, i.e. the window should repaint.
 *
 * Re-entrant calls (a callback calling tick()) are ignored rather than
 * allowed to corrupt the pass in progress.
 */
bool Timeline::tick()
{
    if (paused_) {
        return false;
    }
    if (ticking_) {
        HH_LOG_WARN(kLog, L"tick: nested call ignored");
        return false;
    }
    ticking_ = true;

    // One shared timestamp for the whole pass keeps every spring in phase.
    const double t = now();
    bool work = false;

    // -- 1. animations --------------------------------------------------------
    // Sample a snapshot: sample() may add or remove entries (an Animatable
    // unregisters itself the moment it settles). Entries that vanished from
    // active_ during the pass are skipped, which also covers destruction.
    if (!active_.empty()) {
        const std::vector<IAnimation*> snapshot = active_;
        for (IAnimation* a : snapshot) {
            if (!a) {
                continue;
            }
            if (std::find(active_.begin(), active_.end(), a) == active_.end()) {
                continue;   // removed earlier in this pass
            }
            work = true;
            const bool stillActive = a->sample(t);
            // An animation that reports "done" without unregistering itself
            // is dropped here so it cannot be sampled forever.
            if (!stillActive) {
                std::erase(active_, a);
            }
        }
    }

    // -- 2. timers ------------------------------------------------------------
    // Only timers that existed when the pass began are eligible (ids are
    // monotonic), so a callback that re-arms itself for "now" runs on the
    // next tick instead of looping here. timers_ is kept sorted by deadline
    // with FIFO order for ties, and the running timer is removed *before*
    // its callback runs so cancelTimer() from inside a callback is safe.
    const TimerId idLimit = nextTimer_;
    for (;;) {
        auto it = std::find_if(timers_.begin(), timers_.end(), [&](const Timer& timer) {
            return timer.id < idLimit && timer.at <= t;
        });
        if (it == timers_.end()) {
            break;
        }
        Timer due = std::move(*it);
        timers_.erase(it);
        work = true;
        if (due.fn) {
            due.fn();
        }
    }

    // -- 3. deferred callbacks ------------------------------------------------
    // Completion handlers queued from sample() run here, outside the
    // sampling loop, so they may freely destroy widgets and animations.
    for (int round = 0; round < kMaxDeferredRounds && !deferred_.empty(); ++round) {
        std::vector<std::function<void()>> batch;
        batch.swap(deferred_);
        for (auto& fn : batch) {
            if (fn) {
                work = true;
                fn();
            }
        }
    }
    if (!deferred_.empty()) {
        HH_LOG_DEBUG(kLog, L"tick: {} deferred callbacks carried over to the next tick", deferred_.size());
    }

    ticking_ = false;
    return work;
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

/**
 * @brief Schedules @p fn to run once at timeline time @p atSeconds.
 * @return an id for cancelTimer(); 0 when @p fn is empty.
 *
 * Insertion keeps timers_ sorted by deadline; upper_bound places a timer
 * after any existing entry with the same deadline so ties fire FIFO.
 */
TimerId Timeline::addTimer(double atSeconds, std::function<void()> fn)
{
    if (!fn) {
        HH_LOG_WARN(kLog, L"addTimer: empty callback ignored");
        return 0;
    }
    if (!std::isfinite(atSeconds)) {
        HH_LOG_WARN(kLog, L"addTimer: non-finite deadline, firing on the next tick");
        atSeconds = now();
    }

    const TimerId id = nextTimer_++;
    auto pos = std::upper_bound(timers_.begin(), timers_.end(), atSeconds,
                                [](double at, const Timer& timer) { return at < timer.at; });
    timers_.insert(pos, Timer{id, atSeconds, std::move(fn)});
    return id;
}

/**
 * @brief Removes a pending timer. Unknown ids (including 0) are ignored.
 *
 * Works from inside a timer callback: the running timer has already been
 * taken out of timers_, and erasing any other entry is safe because tick()
 * searches the vector afresh for each due timer.
 */
void Timeline::cancelTimer(TimerId id)
{
    if (id == 0) {
        return;
    }
    std::erase_if(timers_, [id](const Timer& timer) { return timer.id == id; });
}

/**
 * @brief Earliest pending deadline, if any (lets the message loop sleep exactly until then).
 */
std::optional<double> Timeline::nextDeadline() const
{
    if (timers_.empty()) {
        return std::nullopt;
    }
    // timers_ is sorted, but a min scan costs nothing at this size and
    // stays correct even if the ordering invariant were ever disturbed.
    const auto it = std::min_element(timers_.begin(), timers_.end(),
                                     [](const Timer& a, const Timer& b) { return a.at < b.at; });
    return it->at;
}

// ---------------------------------------------------------------------------
// Deferred work
// ---------------------------------------------------------------------------

/**
 * @brief Queues @p fn to run at the end of the current (or next) tick.
 */
void Timeline::defer(std::function<void()> fn)
{
    if (!fn) {
        return;
    }
    deferred_.push_back(std::move(fn));
}

} // namespace hh::ui
