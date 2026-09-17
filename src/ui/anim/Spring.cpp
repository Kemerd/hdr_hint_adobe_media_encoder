// ---------------------------------------------------------------------------
// Spring.cpp - closed-form damped spring solver.
//
// The solver never integrates. Position and velocity are analytic functions
// of (t - t0), so a dropped frame, a 144 Hz monitor or a screenshot-driven
// repaint all land on exactly the same trajectory. Retargeting samples the
// current analytic state and simply starts a fresh trajectory from it.
//
// Parameterisation (SwiftUI style):
//   omega0 = 2*pi / response          natural angular frequency
//   zeta   = dampingFraction          1 = critically damped, < 1 rings
//
// All three regimes solve x'' + 2*zeta*omega0*x' + omega0^2*x = 0 for the
// displacement x measured from the target, so every branch decays to 0.
// ---------------------------------------------------------------------------
#include "ui/anim/Spring.h"

#include <algorithm>
#include <cmath>

namespace hh::ui {

namespace {

/// 2*pi as a float; omega0 = 2*pi / response.
constexpr float kTwoPi = 6.283185307179586f;

/// |zeta - 1| below this is treated as critically damped. The under/over
/// damped closed forms divide by sqrt(|1 - zeta^2|), which degenerates here.
constexpr float kCriticalBand = 1e-4f;

/// Damping below this would ring for minutes; the solver clamps to it so a
/// bad preset can never keep a window rendering forever.
constexpr float kMinDamping = 0.01f;

/// Tolerance used when a caller hands over a zero, negative or NaN epsilon.
constexpr float kDefaultEpsilon = 0.0005f;

/**
 * @brief Returns @p v when it is a finite number, otherwise @p fallback.
 *
 * A single NaN in the initial conditions would poison every later sample
 * (and settled() would never become true), so inputs are laundered here.
 */
float finiteOr(float v, float fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

} // namespace

// ---------------------------------------------------------------------------
// Start / retarget / finish
// ---------------------------------------------------------------------------

/**
 * @brief Captures the initial state and picks the damping regime.
 *
 * @param x0     absolute starting value
 * @param v0     starting velocity (units per second)
 * @param target absolute value the spring settles at
 * @param t0     timeline time (seconds) of the start
 * @param params response / damping / settle tolerance
 *
 * The coefficients A/B are computed for the displacement dx0 = x0 - target so
 * value(t) can simply add target_ back. A non-positive response means
 * "instant": the solver is marked finished and value() returns the target.
 */
void SpringSolver::start(float x0, float v0, float target, double t0, const SpringParams& params)
{
    // Launder every input: NaN/inf anywhere would make the spring never settle.
    target_ = finiteOr(target, 0.0f);
    x0 = finiteOr(x0, target_);
    v0 = finiteOr(v0, 0.0f);
    t0_ = std::isfinite(t0) ? t0 : 0.0;
    epsilon_ = (std::isfinite(params.settleEpsilon) && params.settleEpsilon > 0.0f)
                   ? params.settleEpsilon
                   : kDefaultEpsilon;

    // Displacement from the target; every closed form below drives this to 0.
    // The scale hint keeps the settle tolerance proportional to the travel so
    // a 300 dip slide and a 0..1 opacity both stop at a sensible precision.
    const float dx0 = x0 - target_;
    scaleHint_ = std::max(1.0f, std::fabs(dx0));

    // A non-positive (or non-finite) response is the "instant" spring.
    if (!std::isfinite(params.response) || params.response <= 0.0f) {
        finish();
        return;
    }

    // Natural frequency and clamped damping fraction.
    w0_ = kTwoPi / params.response;
    zeta_ = std::isfinite(params.dampingFraction) ? std::max(kMinDamping, params.dampingFraction) : 1.0f;
    finished_ = false;

    if (std::fabs(zeta_ - 1.0f) < kCriticalBand) {
        // Critically damped: x = (A + B t) e^{-w0 t}
        regime_ = Regime::Critical;
        zeta_ = 1.0f;
        wd_ = w0_;
        r1_ = r2_ = -w0_;
        A_ = dx0;
        B_ = v0 + w0_ * dx0;
    } else if (zeta_ < 1.0f) {
        // Under-damped: x = e^{-zeta w0 t} (A cos wd t + B sin wd t)
        regime_ = Regime::Under;
        wd_ = w0_ * std::sqrt(1.0f - zeta_ * zeta_);
        r1_ = r2_ = -zeta_ * w0_;          // envelope decay rate, kept for reference
        A_ = dx0;
        B_ = (v0 + zeta_ * w0_ * dx0) / wd_;
    } else {
        // Over-damped: x = A e^{r1 t} + B e^{r2 t}, r1 the slow root, r2 the fast one.
        regime_ = Regime::Over;
        const float s = std::sqrt(zeta_ * zeta_ - 1.0f);
        r1_ = -w0_ * (zeta_ - s);
        r2_ = -w0_ * (zeta_ + s);
        wd_ = w0_;
        B_ = (v0 - r1_ * dx0) / (r2_ - r1_);
        A_ = dx0 - B_;
    }

    // Starting at rest on the target: nothing to animate, report settled at once.
    if (std::fabs(dx0) < epsilon_ * scaleHint_ && std::fabs(v0) < epsilon_ * 10.0f * scaleHint_) {
        finish();
    }
}

/**
 * @brief Changes the target mid-flight without a visible discontinuity.
 *
 * The analytic position and velocity at @p tNow become the initial
 * conditions of a new trajectory that reuses the current spring parameters.
 * A finished solver simply starts from its resting value with zero velocity.
 */
void SpringSolver::retarget(float newTarget, double tNow)
{
    // Sample the current analytic state before anything is overwritten.
    const float x = value(tNow);
    const float v = finished_ ? 0.0f : velocity(tNow);

    // Rebuild the parameters from the stored frequency/damping/tolerance.
    SpringParams params;
    params.response = (w0_ > 0.0f && std::isfinite(w0_)) ? kTwoPi / w0_ : 0.0f;
    params.dampingFraction = zeta_;
    params.settleEpsilon = epsilon_;

    start(x, v, newTarget, tNow, params);
}

/**
 * @brief Snaps to the target: value() returns target_ and settled() is true.
 */
void SpringSolver::finish()
{
    finished_ = true;
    A_ = 0.0f;
    B_ = 0.0f;
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

/**
 * @brief Analytic position at timeline time @p t (absolute value, not displacement).
 *
 * Time before t0 clamps to the start, and any non-finite intermediate result
 * falls back to the target so a broken sample can never paint garbage.
 */
float SpringSolver::value(double t) const
{
    if (finished_) {
        return target_;
    }

    // Everything is evaluated in double for the exp/trig, then narrowed once.
    const double tau = std::max(0.0, t - t0_);
    double x = 0.0;

    switch (regime_) {
    case Regime::Under: {
        const double env = std::exp(-static_cast<double>(zeta_ * w0_) * tau);
        const double phase = static_cast<double>(wd_) * tau;
        x = env * (static_cast<double>(A_) * std::cos(phase) + static_cast<double>(B_) * std::sin(phase));
        break;
    }
    case Regime::Critical: {
        const double env = std::exp(-static_cast<double>(w0_) * tau);
        x = (static_cast<double>(A_) + static_cast<double>(B_) * tau) * env;
        break;
    }
    case Regime::Over: {
        x = static_cast<double>(A_) * std::exp(static_cast<double>(r1_) * tau)
          + static_cast<double>(B_) * std::exp(static_cast<double>(r2_) * tau);
        break;
    }
    }

    // A NaN here would propagate into layout; the target is the safe answer.
    if (!std::isfinite(x)) {
        return target_;
    }
    return target_ + static_cast<float>(x);
}

/**
 * @brief Analytic velocity (units per second) at timeline time @p t.
 *
 * Each branch is the exact derivative of the matching value() branch, which
 * is what makes retarget() seamless: the new trajectory starts with the same
 * slope the old one had.
 */
float SpringSolver::velocity(double t) const
{
    if (finished_) {
        return 0.0f;
    }

    const double tau = std::max(0.0, t - t0_);
    double v = 0.0;

    switch (regime_) {
    case Regime::Under: {
        // d/dt [e^{-a t}(A cos w t + B sin w t)]
        //   = e^{-a t} [ (B w - a A) cos w t - (A w + a B) sin w t ]
        const double a = static_cast<double>(zeta_ * w0_);
        const double w = static_cast<double>(wd_);
        const double env = std::exp(-a * tau);
        const double phase = w * tau;
        const double A = static_cast<double>(A_);
        const double B = static_cast<double>(B_);
        v = env * ((B * w - a * A) * std::cos(phase) - (A * w + a * B) * std::sin(phase));
        break;
    }
    case Regime::Critical: {
        // d/dt [(A + B t) e^{-w0 t}] = (B - w0 (A + B t)) e^{-w0 t}
        const double w0 = static_cast<double>(w0_);
        const double env = std::exp(-w0 * tau);
        const double A = static_cast<double>(A_);
        const double B = static_cast<double>(B_);
        v = (B - w0 * (A + B * tau)) * env;
        break;
    }
    case Regime::Over: {
        // d/dt [A e^{r1 t} + B e^{r2 t}] = A r1 e^{r1 t} + B r2 e^{r2 t}
        const double r1 = static_cast<double>(r1_);
        const double r2 = static_cast<double>(r2_);
        v = static_cast<double>(A_) * r1 * std::exp(r1 * tau)
          + static_cast<double>(B_) * r2 * std::exp(r2 * tau);
        break;
    }
    }

    if (!std::isfinite(v)) {
        return 0.0f;
    }
    return static_cast<float>(v);
}

/**
 * @brief True once both the displacement and the velocity are within tolerance.
 *
 * The tolerance scales with the initial travel (scaleHint_), so large slides
 * do not spend frames chasing sub-pixel residue. Position and velocity are
 * checked together: an under-damped spring crossing zero at full speed is
 * not settled, only a decayed envelope satisfies both.
 */
bool SpringSolver::settled(double t) const
{
    if (finished_) {
        return true;
    }

    const float x = value(t) - target_;
    const float v = velocity(t);

    // Non-finite state can only come from corrupt coefficients; stop rather than spin.
    if (!std::isfinite(x) || !std::isfinite(v)) {
        return true;
    }

    const float posTolerance = epsilon_ * scaleHint_;
    const float velTolerance = epsilon_ * 10.0f * scaleHint_;
    return std::fabs(x) < posTolerance && std::fabs(v) < velTolerance;
}

} // namespace hh::ui
