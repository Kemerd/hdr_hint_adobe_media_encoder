// ---------------------------------------------------------------------------
// Chip.cpp - small tinted capsule for status and transfer-function badges.
//
// Fill is the tone colour at 18 %, text is the tone colour itself. The
// optional spinner is a 270 degree arc that turns once per second and is
// advanced from frame ticks quantised to ~24 fps.
// ---------------------------------------------------------------------------
#include "ui/controls/Chip.h"

#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

constexpr float kHeight = 20.0f;
constexpr float kRadius = 10.0f;
constexpr float kPad = 8.0f;
/// Gap between a leading dot/spinner and the text.
constexpr float kLeadGap = 5.0f;
constexpr float kDot = 6.0f;
constexpr float kSpinner = 12.0f;
constexpr float kSpinnerStroke = 1.5f;
constexpr float kSpinnerSweep = 270.0f;
/// Fill alpha relative to the tone colour.
constexpr float kFillAlpha = 0.18f;
/// Spinner visual update rate.
constexpr double kFrameQuantum = 1.0 / 24.0;

} // namespace

/**
 * @brief Creates a chip with text and a tone.
 */
Chip::Chip(std::wstring text, ChipTone tone) : text_(std::move(text)), tone_(tone) {}

/**
 * @brief Replaces the text (re-measures when it changed).
 */
void Chip::setText(std::wstring text) {
    if (text_ == text) {
        return;
    }
    text_ = std::move(text);
    invalidateLayout();
}

/**
 * @brief Changes the tint (repaint only).
 */
void Chip::setTone(ChipTone tone) {
    if (tone_ == tone) {
        return;
    }
    tone_ = tone;
    invalidate();
}

/**
 * @brief Shows or hides the leading spinner and starts/stops frame ticks.
 */
void Chip::setSpinner(bool on) {
    if (spinner_ == on) {
        return;
    }
    spinner_ = on;
    angle_ = 0.0f;
    // The tick flag is stored on the widget; attaching later registers it.
    setWantsFrameTicks(spinner_);
    invalidateLayout();
}

/**
 * @brief Width = padding + optional leading element + text; height is fixed.
 */
Size Chip::measure(const Constraints& c) {
    float w = kPad * 2.0f;
    if (dot_) {
        w += kDot + kLeadGap;
    }
    if (spinner_) {
        w += kSpinner + kLeadGap;
    }
    if (!text_.empty()) {
        const Size text = measureTextShared(text_, typography::chip());
        w += std::ceil(std::max(0.0f, text.w));
    }
    return c.constrain({w, kHeight});
}

/**
 * @brief Paints the capsule, the leading dot/spinner and the uppercase label.
 */
void Chip::paintSelf(Canvas& c) {
    const Rect raw = bounds();
    if (raw.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    const Color tone = toneColor(t, tone_);

    // Snap the capsule so its edges land on whole pixels at every DPI.
    const Rect b = c.scale().snap(raw);
    c.fillRoundedRect(b, kRadius, tone.scaledAlpha(kFillAlpha));

    float x = b.x + kPad;
    const float cy = b.y + b.h * 0.5f;

    // Leading dot.
    if (dot_) {
        c.fillCircle({x + kDot * 0.5f, cy}, kDot * 0.5f, tone);
        x += kDot + kLeadGap;
    }

    // Leading spinner arc (inset half a stroke so it stays inside 12 dip).
    if (spinner_) {
        const float radius = kSpinner * 0.5f - kSpinnerStroke * 0.5f;
        c.strokeArc({x + kSpinner * 0.5f, cy}, radius, angle_, kSpinnerSweep, tone, kSpinnerStroke);
        x += kSpinner + kLeadGap;
    }

    // Label: the chip style uppercases and letter-spaces the text itself.
    if (!text_.empty()) {
        const float avail = b.right() - kPad - x;
        if (avail > 0.0f) {
            const Rect textRect{x, b.y, avail, b.h};
            c.drawText(text_, typography::chip(), textRect, tone, HAlign::Left, VAlign::Center, Trimming::End, 1);
        }
    }
}

/**
 * @brief Rotates the spinner one revolution per second, repainting ~24 times a second.
 * @param now timeline seconds
 */
void Chip::onFrame(double now) {
    if (!spinner_) {
        return;
    }
    // Absolute-time rotation: the angle depends only on the clock, so frame
    // drops never slow the spinner down. Quantising to 1/24 s limits repaints.
    const double quantised = std::floor(now / kFrameQuantum) * kFrameQuantum;
    const double turns = quantised - std::floor(quantised);
    const float angle = static_cast<float>(turns * 360.0);
    if (angle == angle_) {
        return;
    }
    angle_ = angle;
    if (visible()) {
        invalidate();
    }
}

/**
 * @brief Theme colour for a chip tone.
 */
Color Chip::toneColor(const Theme& t, ChipTone tone) {
    switch (tone) {
    case ChipTone::Neutral:     return t.labelSecondary;
    case ChipTone::Accent:      return t.accent;
    case ChipTone::Success:     return t.success;
    case ChipTone::Warning:     return t.warning;
    case ChipTone::Destructive: return t.destructive;
    case ChipTone::Info:        return t.info;
    case ChipTone::PQ:          return t.tonePQ;
    case ChipTone::HLG:         return t.toneHLG;
    case ChipTone::SDR:         return t.toneSDR;
    }
    return t.labelSecondary;
}

} // namespace hh::ui
