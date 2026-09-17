// ---------------------------------------------------------------------------
// Spinner.cpp - rotating arc for indeterminate progress.
//
// The arc is advanced from elapsed time (so its speed is independent of the
// frame rate) but only every 1/24 s: decorative motion does not need to run
// at monitor refresh while the GPU is busy encoding HDR video next door.
// ---------------------------------------------------------------------------
#include "ui/controls/Spinner.h"

#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace hh::ui {

namespace {

/// Angular speed: 1.1 revolutions per second, in degrees.
constexpr float kDegreesPerSecond = 1.1f * 360.0f;
/// Visible portion of the circle.
constexpr float kSweepDegrees = 300.0f;
/// Stroke thickness in dips.
constexpr float kStroke = 2.0f;
/// Minimum interval between visual updates (~24 fps).
constexpr double kTickInterval = 1.0 / 24.0;
/// Largest time step applied in one tick (guards against clock jumps after a
/// paused window resumes).
constexpr double kMaxStep = 0.25;

} // namespace

/**
 * @brief Creates a spinner of the given diameter (dips).
 */
Spinner::Spinner(float size) : size_(std::max(0.0f, size)) {
    // Ask for frame ticks up front so attaching to a root registers the
    // widget immediately; onAttached() repeats this defensively.
    setWantsFrameTicks(true);
}

/**
 * @brief A spinner is always square.
 */
Size Spinner::measure(const Constraints& c) {
    const float s = std::max(0.0f, size_);
    return c.constrain({s, s});
}

/**
 * @brief Strokes the 300 degree arc at the current rotation.
 */
void Spinner::paintSelf(Canvas& c) {
    const Rect b = bounds();
    if (b.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    const Color color = accent_ ? t.accent : t.labelSecondary;

    // Keep the stroke fully inside the frame: radius shrinks by half a stroke.
    const float diameter = std::min(b.w, b.h);
    const float radius = diameter * 0.5f - kStroke * 0.5f;
    if (radius <= 0.0f) {
        return;
    }
    c.strokeArc(b.center(), radius, angle_, kSweepDegrees, color, kStroke);
}

/**
 * @brief Advances the rotation from elapsed time, at most 24 times per second.
 * @param now timeline seconds
 */
void Spinner::onFrame(double now) {
    // First tick after attaching: just record the time so the arc does not
    // leap forward by however long the widget sat detached.
    if (lastTick_ <= 0.0) {
        lastTick_ = now;
        return;
    }
    const double elapsed = now - lastTick_;
    if (elapsed < kTickInterval) {
        return;
    }
    lastTick_ = now;

    // Clamp the step so a long pause (hidden window) does not spin wildly.
    const float step = static_cast<float>(std::clamp(elapsed, 0.0, kMaxStep));
    angle_ = std::fmod(angle_ + step * kDegreesPerSecond, 360.0f);
    if (angle_ < 0.0f) {
        angle_ += 360.0f;
    }
    if (visible()) {
        invalidate();
    }
}

/**
 * @brief Registers for per-frame ticks once the widget has a root.
 */
void Spinner::onAttached() {
    lastTick_ = 0.0;
    setWantsFrameTicks(true);
}

/**
 * @brief Stops ticking when the widget leaves the tree.
 */
void Spinner::onDetached() {
    setWantsFrameTicks(false);
    lastTick_ = 0.0;
}

} // namespace hh::ui
