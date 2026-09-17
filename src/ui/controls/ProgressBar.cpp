// ---------------------------------------------------------------------------
// ProgressBar.cpp - determinate (spring fill) and indeterminate (shimmer).
//
// The determinate bar springs its fill width towards the last progress value
// so per-second progress updates from the encoder read as one smooth motion.
// The indeterminate bar sweeps a 30 %-wide gradient across the track; its
// phase is quantised to ~24 fps so a busy queue does not repaint at monitor
// refresh for the whole duration of an encode.
// ---------------------------------------------------------------------------
#include "ui/controls/ProgressBar.h"

#include "ui/anim/Easing.h"
#include "ui/core/RootView.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace hh::ui {

namespace {

// Track geometry: 4 dip tall (6 in the thick variant), fully rounded ends.
constexpr float kHeightRegular = 4.0f;
constexpr float kHeightThick = 6.0f;
constexpr float kRadius = 2.0f;

// Indeterminate sweep: one pass every 1.6 s, the bar is 30 % of the track and
// the phase is sampled at 24 steps per second.
constexpr double kSweepPeriod = 1.6;
constexpr float kSweepFraction = 0.30f;
constexpr double kPhaseRate = 24.0;

// Preferred width when nobody constrains us (a stack usually stretches us).
constexpr float kDefaultWidth = 120.0f;

/**
 * @brief Picks the theme to paint with: the root's when attached, else the canvas's.
 */
const Theme& paintTheme(const Widget& w, const Canvas& c) {
    const Theme* t = w.theme();
    return t ? *t : c.theme();
}

} // namespace

/**
 * @brief Wires the fill spring to this widget so every sample repaints it.
 */
ProgressBar::ProgressBar() {
    fill_.setOwner(this);
}

/**
 * @brief Sets the determinate value (clamped to 0..1), springing when asked.
 */
void ProgressBar::setProgress(float value, bool animated) {
    // NaN and out-of-range values are clamped so the fill never overshoots the track.
    if (std::isnan(value)) {
        value = 0.0f;
    }
    value = std::clamp(value, 0.0f, 1.0f);

    // A gentle, critically damped spring keeps encoder updates from looking stepped.
    if (animated) {
        fill_.animateTo(value, springs::gentle);
    } else {
        fill_.set(value);
    }
    invalidate();
}

/**
 * @brief Switches between the sweeping shimmer and the determinate fill.
 *
 * Frame ticks are only wanted while indeterminate; the widget kernel registers
 * them with the root when (and only when) the widget is attached.
 */
void ProgressBar::setIndeterminate(bool on) {
    if (indeterminate_ == on) {
        return;
    }
    indeterminate_ = on;
    phase_ = 0.0;

    // Ticks drive the sweep; drop them as soon as the bar is determinate again.
    setWantsFrameTicks(on);
    invalidate();
}

/**
 * @brief Preferred size: fills the offered width, fixed track height.
 */
Size ProgressBar::measure(const Constraints& c) {
    const float h = thick_ ? kHeightThick : kHeightRegular;

    // Prefer an explicit preferred width, then the offered width, then a default.
    float w = preferredSize_.w > 0.0f ? preferredSize_.w : (c.hasBoundedWidth() ? c.maxW : kDefaultWidth);
    w = std::max(0.0f, w);
    return c.constrain({w, h});
}

/**
 * @brief Paints the track and either the spring fill or the sweeping bar.
 */
void ProgressBar::paintSelf(Canvas& c) {
    const Theme& t = paintTheme(*this, c);
    const Rect b = bounds();
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return;
    }

    // The track is vertically centred inside whatever height the parent gave us.
    const float h = std::min(b.h, thick_ ? kHeightThick : kHeightRegular);
    const Rect track = c.scale().snap(Rect{0.0f, (b.h - h) * 0.5f, b.w, h});
    c.fillRoundedRect(track, kRadius, t.fillTertiary);

    // The fill colour follows the semantic tone of the job state.
    Color fill = t.accent;
    switch (tone_) {
    case ProgressTone::Success: fill = t.success; break;
    case ProgressTone::Warning: fill = t.warning; break;
    case ProgressTone::Destructive: fill = t.destructive; break;
    case ProgressTone::Accent: default: break;
    }

    if (!indeterminate_) {
        // Determinate: the spring value maps straight onto the track width.
        const float fraction = std::clamp(fill_.value(), 0.0f, 1.0f);
        const float fw = track.w * fraction;
        if (fw > 0.0f) {
            // Keep the rounded ends intact even for a sliver of progress.
            const float visible = std::max(fw, std::min(track.w, 2.0f * kRadius));
            c.fillRoundedRect({track.x, track.y, visible, track.h}, kRadius, fill);
        }
        return;
    }

    // Indeterminate: a soft bar sweeps from fully off the left edge to fully
    // off the right edge, eased so it lingers slightly at both ends.
    const float eased = easing::easeInOutSine(static_cast<float>(phase_));
    const float barW = std::max(track.w * kSweepFraction, 2.0f * kRadius);
    const float x = -barW + eased * (track.w + barW);
    const Rect bar{x, track.y, barW, track.h};

    // Clip to the rounded track so the bar never pokes out of the ends.
    c.pushRoundedClip(track, kRadius);

    // Two gradient halves: transparent -> colour -> transparent.
    const Color faint = fill.withAlpha(0.0f);
    const Rect leftHalf{bar.x, bar.y, bar.w * 0.5f, bar.h};
    const Rect rightHalf{bar.x + bar.w * 0.5f, bar.y, bar.w * 0.5f, bar.h};
    c.fillLinearGradient(leftHalf, faint, fill, false);
    c.fillLinearGradient(rightHalf, fill, faint, false);
    c.pop();
}

/**
 * @brief Advances the sweep phase at ~24 steps per second while indeterminate.
 */
void ProgressBar::onFrame(double now) {
    if (!indeterminate_) {
        return;
    }
    if (std::isnan(now) || now < 0.0) {
        now = 0.0;
    }

    // Quantise the clock so the bar only repaints when its position changes.
    const double stepped = std::floor(now * kPhaseRate) / kPhaseRate;
    const double phase = std::fmod(stepped, kSweepPeriod) / kSweepPeriod;
    if (phase != phase_) {
        phase_ = phase;
        invalidate();
    }
}

} // namespace hh::ui
