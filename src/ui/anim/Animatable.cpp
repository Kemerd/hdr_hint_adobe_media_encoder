// ---------------------------------------------------------------------------
// Animatable.cpp - member definitions + explicit instantiations.
//
// The template lives here rather than in the header so Widget.h (which owns
// an Animatable<float> for opacity) does not have to see RootView/Timeline
// internals. Every supported T is instantiated at the bottom of the file.
// ---------------------------------------------------------------------------
#include "ui/anim/Animatable.h"

#include "ui/core/Widget.h"

#include <utility>

namespace hh::ui {

// ---------------------------------------------------------------------------
// Owner plumbing
// ---------------------------------------------------------------------------

/**
 * @brief The timeline that should drive this value: the owner's root's clock.
 * @return nullptr while the owner is unset or detached from a RootView.
 */
template <class T>
Timeline* Animatable<T>::resolveTimeline()
{
    if (!owner_) {
        return nullptr;
    }
    return owner_->timeline();
}

/**
 * @brief Asks the owner to repaint (no-op without an owner).
 */
template <class T>
void Animatable<T>::invalidateOwner()
{
    if (owner_) {
        owner_->invalidate();
    }
}

/**
 * @brief Unregisters from the timeline and drops the completion callback.
 *
 * Called from the destructor, so the Timeline must never be left holding a
 * pointer to a dead animation. The callback is discarded, not run: a value
 * that is torn down mid-flight never "completed".
 */
template <class T>
void Animatable<T>::detach()
{
    if (timeline_) {
        timeline_->remove(this);
        timeline_ = nullptr;
    }
    done_ = nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * @brief Jumps to @p v immediately, cancelling any animation in progress.
 */
template <class T>
void Animatable<T>::set(const T& v)
{
    value_ = v;
    target_ = v;
    stop();
    invalidateOwner();
}

/**
 * @brief Freezes the value where it currently is.
 *
 * The target collapses onto the current value, the solvers are marked
 * finished so a later retarget starts from rest, and the pending completion
 * callback is dropped (stopping is not settling).
 */
template <class T>
void Animatable<T>::stop()
{
    if (timeline_) {
        timeline_->remove(this);
        timeline_ = nullptr;
    }
    target_ = value_;
    for (int i = 0; i < AnimTraits<T>::N; ++i) {
        solvers_[i].finish();
    }
    done_ = nullptr;
}

/**
 * @brief Springs towards @p target.
 *
 * Without a timeline (detached widget), under reduced motion, or with an
 * effectively zero response the value jumps and @p done runs synchronously.
 * Otherwise each component gets its own solver started (or retargeted, when
 * already moving) from the current analytic position and velocity, so
 * hammering a control mid-animation never produces a visible discontinuity.
 */
template <class T>
void Animatable<T>::animateTo(const T& target, const SpringParams& params, std::function<void()> done)
{
    Timeline* tl = resolveTimeline();

    // Instant path: no clock to sample from, reduced motion, or an "instant" preset.
    if (!tl || tl->reducedMotion() || params.response <= 0.001f) {
        // set() drops any older completion; the new one runs right away.
        set(target);
        // Nothing of *this may be touched after the callback: it is allowed to
        // remove the owning widget (which destroys this object).
        std::function<void()> finished = std::move(done);
        if (finished) {
            finished();
        }
        return;
    }

    // The owner may have been re-parented under a different root since the
    // last animation; never leave a registration on a timeline we no longer use.
    if (timeline_ && timeline_ != tl) {
        timeline_->remove(this);
        timeline_ = nullptr;
    }

    const double now = tl->now();
    const bool wasAnimating = (timeline_ != nullptr);

    // Start or retarget one solver per component from the live state.
    for (int i = 0; i < AnimTraits<T>::N; ++i) {
        SpringSolver& solver = solvers_[i];
        const float to = AnimTraits<T>::get(target, i);
        float from = AnimTraits<T>::get(value_, i);
        float v0 = 0.0f;
        if (wasAnimating) {
            // Use the analytic position/velocity rather than the last sampled
            // value so the hand-over is exact even between frames.
            from = solver.value(now);
            v0 = solver.velocity(now);
            AnimTraits<T>::set(value_, i, from);
        }
        solver.start(from, v0, to, now, params);
    }

    target_ = target;
    done_ = std::move(done);

    // Register once; the timeline samples us every tick until all settle.
    if (!timeline_) {
        timeline_ = tl;
        tl->add(this);
    }
    invalidateOwner();
}

/**
 * @brief One frame of the animation.
 * @return true while any component is still moving.
 *
 * Settled components snap to their exact target. Once every component has
 * settled the value snaps to target_, the animation unregisters itself and
 * the completion callback is deferred to the end of the tick, where it may
 * safely tear down widgets.
 */
template <class T>
bool Animatable<T>::sample(double now)
{
    if (!timeline_) {
        return false;
    }

    bool allSettled = true;
    for (int i = 0; i < AnimTraits<T>::N; ++i) {
        SpringSolver& solver = solvers_[i];
        float v = solver.value(now);
        if (solver.settled(now)) {
            v = solver.target();
        } else {
            allSettled = false;
        }
        AnimTraits<T>::set(value_, i, v);
    }

    // Every tick changes the painted value, so the owner must repaint.
    invalidateOwner();

    if (!allSettled) {
        return true;
    }

    // Snap exactly, unregister, then hand the completion to the timeline so it
    // runs after the sampling loop (this object may not survive the callback).
    value_ = target_;
    for (int i = 0; i < AnimTraits<T>::N; ++i) {
        solvers_[i].finish();
    }
    Timeline* tl = timeline_;
    timeline_ = nullptr;
    tl->remove(this);
    if (done_) {
        std::function<void()> finished = std::move(done_);
        done_ = nullptr;
        tl->defer(std::move(finished));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Explicit instantiations - the only Ts the UI animates.
// ---------------------------------------------------------------------------
template class Animatable<float>;
template class Animatable<Point>;
template class Animatable<Color>;
template class Animatable<Rect>;

} // namespace hh::ui
