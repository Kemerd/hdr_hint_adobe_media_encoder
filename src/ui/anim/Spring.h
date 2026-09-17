// ---------------------------------------------------------------------------
// Spring.h - analytic damped spring (SwiftUI-style parameters).
//
// Deterministic: position is a closed-form function of (t - t0), so frame
// rate never changes the trajectory and retargeting mid-flight is seamless.
// ---------------------------------------------------------------------------
#pragma once

namespace hh::ui {

struct SpringParams {
    float response = 0.3f;         ///< seconds for one period (2*pi/omega0)
    float dampingFraction = 0.85f; ///< 1 = critically damped, < 1 bouncy
    float settleEpsilon = 0.0005f; ///< |x| and |v| tolerance for "settled"
};

namespace springs {
inline constexpr SpringParams snappy{0.25f, 0.85f, 0.0005f};
inline constexpr SpringParams gentle{0.45f, 1.0f, 0.0005f};
inline constexpr SpringParams bouncy{0.40f, 0.60f, 0.0005f};
inline constexpr SpringParams interactive{0.15f, 0.90f, 0.0005f};
inline constexpr SpringParams scroll{0.30f, 1.0f, 0.0005f};
inline constexpr SpringParams instant{0.001f, 1.0f, 0.0005f};
} // namespace springs

class SpringSolver {
public:
    /// Starts from (x0, v0) towards target at time t0 (seconds).
    void start(float x0, float v0, float target, double t0, const SpringParams& params);
    /// Changes the target at time tNow keeping the current position and velocity.
    void retarget(float newTarget, double tNow);
    [[nodiscard]] float value(double t) const;
    [[nodiscard]] float velocity(double t) const;
    [[nodiscard]] bool settled(double t) const;
    [[nodiscard]] float target() const noexcept { return target_; }
    /// Snaps to the target (used for reduced-motion).
    void finish();

private:
    enum class Regime { Under, Critical, Over };
    Regime regime_ = Regime::Critical;
    float A_ = 0, B_ = 0, w0_ = 1, wd_ = 1, zeta_ = 1, r1_ = 0, r2_ = 0;
    float target_ = 0;
    double t0_ = 0;
    float scaleHint_ = 1;
    float epsilon_ = 0.0005f;
    bool finished_ = true;
};

} // namespace hh::ui
