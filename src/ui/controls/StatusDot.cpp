// ---------------------------------------------------------------------------
// StatusDot.cpp - 8 dip status dot with an optional caption and a pulsing
// halo while linked to Media Encoder.
//
// The halo is driven by frame ticks at ~24 fps and only while the status is
// Linked; every other state costs nothing per frame.
// ---------------------------------------------------------------------------
#include "ui/controls/StatusDot.h"

#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

/// Dot diameter in dips.
constexpr float kDot = 8.0f;
/// Gap between the dot and the caption.
constexpr float kLabelGap = 6.0f;
/// Halo diameter range (dips).
constexpr float kHaloMin = 8.0f;
constexpr float kHaloMax = 22.0f;
/// Halo ring stroke width.
constexpr float kHaloStroke = 1.5f;
/// Peak halo alpha at the start of a pulse.
constexpr float kHaloAlpha = 0.15f;
/// One pulse every 3 s, expanding for 0.9 s.
constexpr double kPulsePeriod = 3.0;
constexpr double kPulseDuration = 0.9;
/// Visual update rate for the pulse.
constexpr double kFrameQuantum = 1.0 / 24.0;

/**
 * @brief Theme colour for a link status.
 */
Color colorForStatus(const Theme& t, LinkStatus s) {
    switch (s) {
    case LinkStatus::Offline:  return t.labelTertiary;
    case LinkStatus::Watching: return t.warning;
    case LinkStatus::Linked:   return t.success;
    case LinkStatus::Warning:  return t.warning;
    }
    return t.labelTertiary;
}

} // namespace

/**
 * @brief Starts offline (no halo, no ticks).
 */
StatusDot::StatusDot() {
    setWantsFrameTicks(false);
}

/**
 * @brief Changes the status; Linked starts the halo pulse, anything else stops it.
 */
void StatusDot::setStatus(LinkStatus status) {
    if (status_ == status) {
        return;
    }
    status_ = status;

    // Reset the pulse so the first halo after linking starts from zero.
    halo_ = 0.0f;
    haloStart_ = 0.0;
    setWantsFrameTicks(status_ == LinkStatus::Linked);
    invalidate();
}

/**
 * @brief Sets (or clears) the caption drawn to the right of the dot.
 */
void StatusDot::setLabel(std::wstring label) {
    if (label_ == label) {
        return;
    }
    label_ = std::move(label);
    invalidateLayout();
}

/**
 * @brief Dot (plus caption) size; the halo may overflow the frame slightly.
 */
Size StatusDot::measure(const Constraints& c) {
    float w = kDot;
    float h = kDot;
    if (!label_.empty()) {
        const Size text = measureTextShared(label_, typography::caption());
        w += kLabelGap + std::ceil(std::max(0.0f, text.w));
        h = std::max(h, std::ceil(std::max(0.0f, text.h)));
    }
    return c.constrain({w, h});
}

/**
 * @brief Paints the halo (if pulsing), the dot and the caption.
 */
void StatusDot::paintSelf(Canvas& c) {
    const Rect b = bounds();
    if (b.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    const Color color = colorForStatus(t, status_);

    // The dot sits at the left edge, vertically centred with the caption.
    const Point centre{b.x + kDot * 0.5f, b.y + b.h * 0.5f};

    // Halo ring: grows from the dot's edge outwards while fading out.
    if (status_ == LinkStatus::Linked && halo_ > 0.0f) {
        const float progress = std::clamp(halo_, 0.0f, 1.0f);
        const float diameter = kHaloMin + (kHaloMax - kHaloMin) * progress;
        const float alpha = kHaloAlpha * (1.0f - progress);
        if (alpha > 0.001f) {
            c.strokeCircle(centre, diameter * 0.5f, color.withAlpha(alpha), kHaloStroke);
        }
    }

    c.fillCircle(centre, kDot * 0.5f, color);

    // Caption in secondary text to the right of the dot.
    if (!label_.empty()) {
        const float x = b.x + kDot + kLabelGap;
        const Rect textRect{x, b.y, std::max(0.0f, b.right() - x), b.h};
        if (!textRect.isEmpty()) {
            c.drawText(label_, typography::caption(), textRect, t.labelSecondary,
                       HAlign::Left, VAlign::Center, Trimming::End, 1);
        }
    }
}

/**
 * @brief Drives the halo: a 0.9 s expansion every 3 s, quantised to 24 fps.
 * @param now timeline seconds
 */
void StatusDot::onFrame(double now) {
    if (status_ != LinkStatus::Linked) {
        // Ticks should already be off; make sure the halo is not left mid-pulse.
        if (halo_ != 0.0f) {
            halo_ = 0.0f;
            invalidate();
        }
        return;
    }

    // The first pulse begins the moment the widget starts ticking.
    if (haloStart_ <= 0.0) {
        haloStart_ = now;
    }
    const double elapsed = std::max(0.0, now - haloStart_);
    const double phase = std::fmod(elapsed, kPulsePeriod);

    // Snap the phase to 24 fps steps so the ring only repaints ~24 times a
    // second regardless of how often the root ticks us.
    const double quantised = std::floor(phase / kFrameQuantum) * kFrameQuantum;
    float target = 0.0f;
    if (quantised < kPulseDuration) {
        target = static_cast<float>(std::clamp(quantised / kPulseDuration, 0.0, 1.0));
    }

    if (target != halo_) {
        halo_ = target;
        if (visible()) {
            invalidate();
        }
    }
}

} // namespace hh::ui
