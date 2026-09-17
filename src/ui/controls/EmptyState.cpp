// ---------------------------------------------------------------------------
// EmptyState.cpp - centred illustration + title + subtitle for empty lists.
//
// The block (icon, title, subtitle, status dot) is centred inside whatever
// frame the parent hands over. The two labels are children; the icon and the
// dot are painted here relative to the labels' frames, so there is only one
// source of truth for the vertical rhythm.
// ---------------------------------------------------------------------------
#include "ui/controls/EmptyState.h"

#include "core/Logger.h"
#include "platform/Time.h"
#include "ui/anim/Easing.h"
#include "ui/anim/Timeline.h"
#include "ui/controls/Label.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"EmptyState";

// ---- metrics ---------------------------------------------------------------
constexpr float kIconWidth = 96.0f;
constexpr float kIconHeight = 64.0f;
constexpr float kIconStroke = 1.5f;
constexpr float kIconTitleGap = 16.0f;
constexpr float kTitleSubtitleGap = 6.0f;
constexpr float kSubtitleDotGap = 16.0f;
constexpr float kDot = 6.0f;
/// Subtitles wrap at this width for comfortable line lengths.
constexpr float kSubtitleMaxWidth = 320.0f;
/// Width assumed when measured without any bound.
constexpr float kUnboundedWidth = 320.0f;
/// Layout treats anything larger than this as unbounded.
constexpr float kUnbounded = 1e8f;

// ---- pulse -----------------------------------------------------------------
constexpr double kPulsePeriodSec = 2.0;
constexpr float kPulseMin = 0.9f;
constexpr float kPulseMax = 1.0f;
/// The dot only repaints at this rate; the frame loop may run much faster.
constexpr double kPulseFrameRate = 24.0;
/// The pulse stops itself after this long to save power.
constexpr double kPulseAutoStopSec = 30.0;

/**
 * @brief Loose constraints with the given width bound and unbounded height.
 */
Constraints looseWidth(float maxW) {
    Constraints c;
    c.minW = 0.0f;
    c.maxW = std::max(0.0f, maxW);
    c.minH = 0.0f;
    c.maxH = 1e9f;
    return c;
}

/**
 * @brief Alpha of the status dot at a moment of the pulse cycle.
 *
 * 0.9 -> 1.0 -> 0.9 over one period with an ease-in-out-sine shape, sampled
 * at 24 fps so consecutive frames with the same value paint nothing new.
 */
float pulseAlphaAt(double elapsed) {
    if (!(elapsed >= 0.0)) {
        return kPulseMax;
    }
    const double quantised = std::floor(elapsed * kPulseFrameRate) / kPulseFrameRate;
    const double phase = std::fmod(quantised, kPulsePeriodSec) / kPulsePeriodSec;
    // Rise during the first half, fall during the second.
    const float t = static_cast<float>(phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0);
    return kPulseMin + (kPulseMax - kPulseMin) * easing::easeInOutSine(t);
}

} // namespace

/**
 * @brief Creates the empty state with a headline title and a wrapping subtitle.
 */
EmptyState::EmptyState(std::wstring title, std::wstring subtitle, IconId icon) : icon_(icon) {
    auto titleLabel = std::make_unique<Label>(std::move(title), typography::headline(), LabelTone::Secondary);
    titleLabel->setAlign(HAlign::Center);
    title_ = add(std::move(titleLabel));

    auto subtitleLabel = std::make_unique<Label>(std::move(subtitle), typography::calloutWrap(), LabelTone::Tertiary);
    subtitleLabel->setAlign(HAlign::Center);
    subtitleLabel->setMaxLines(0);
    subtitle_ = add(std::move(subtitleLabel));
}

/**
 * @brief Replaces the title.
 */
void EmptyState::setTitle(std::wstring title) {
    if (auto* label = dynamic_cast<Label*>(title_)) {
        label->setText(std::move(title));
    }
}

/**
 * @brief Replaces the subtitle (hidden when empty).
 */
void EmptyState::setSubtitle(std::wstring subtitle) {
    auto* label = dynamic_cast<Label*>(subtitle_);
    if (!label) {
        return;
    }
    const bool show = !subtitle.empty();
    label->setText(std::move(subtitle));
    label->setVisible(show);
}

/**
 * @brief Starts or stops the slow pulse of the status dot.
 */
void EmptyState::setPulsing(bool on) {
    if (pulsing_ == on) {
        return;
    }
    pulsing_ = on;
    pulse_ = kPulseMax;
    // The start time comes from the frame clock; a detached widget has none
    // yet, so onFrame() picks it up on the first tick instead.
    Timeline* tl = timeline();
    pulseStart_ = (on && tl) ? tl->now() : 0.0;
    setWantsFrameTicks(on);
    invalidate();
}

/**
 * @brief Natural size of the centred block; wider parents just get more margin.
 */
Size EmptyState::measure(const Constraints& c) {
    const bool bounded = c.maxW < kUnbounded;
    const float width = bounded ? std::max(0.0f, c.maxW) : kUnboundedWidth;

    float h = kIconHeight + kIconTitleGap;
    if (title_ && title_->visible()) {
        h += std::max(0.0f, title_->measure(looseWidth(width)).h);
    }
    if (subtitle_ && subtitle_->visible()) {
        h += kTitleSubtitleGap + std::max(0.0f, subtitle_->measure(looseWidth(std::min(width, kSubtitleMaxWidth))).h);
    }
    h += kSubtitleDotGap + kDot;
    return c.constrain({width, h});
}

/**
 * @brief Centres the block vertically and each label horizontally.
 */
void EmptyState::onLayout() {
    const Rect b = bounds();

    // Measure the labels at the widths they may use.
    Size titleS;
    if (title_ && title_->visible()) {
        titleS = title_->measure(looseWidth(b.w));
        titleS.w = std::clamp(titleS.w, 0.0f, b.w);
    }
    Size subS;
    const bool hasSubtitle = subtitle_ != nullptr && subtitle_->visible();
    if (hasSubtitle) {
        subS = subtitle_->measure(looseWidth(std::min(b.w, kSubtitleMaxWidth)));
        subS.w = std::clamp(subS.w, 0.0f, b.w);
    }

    // Block height decides where the icon starts.
    float blockH = kIconHeight + kIconTitleGap + titleS.h;
    if (hasSubtitle) {
        blockH += kTitleSubtitleGap + subS.h;
    }
    blockH += kSubtitleDotGap + kDot;
    float y = std::max(0.0f, (b.h - blockH) * 0.5f) + kIconHeight + kIconTitleGap;

    if (title_ && title_->visible()) {
        title_->layout({std::round((b.w - titleS.w) * 0.5f), y, titleS.w, titleS.h});
        y += titleS.h;
    }
    if (hasSubtitle) {
        y += kTitleSubtitleGap;
        subtitle_->layout({std::round((b.w - subS.w) * 0.5f), y, subS.w, subS.h});
    }
}

/**
 * @brief Paints the film-frame illustration above the title and the status dot below.
 */
void EmptyState::paintSelf(Canvas& c) {
    const Rect b = bounds();
    if (b.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();

    // The illustration sits 16 dip above the title (or at the top of the block).
    float iconBottom = kIconHeight;
    float dotTop = kIconHeight + kSubtitleDotGap;
    if (title_ && title_->visible()) {
        iconBottom = title_->frame().y - kIconTitleGap;
        dotTop = title_->frame().bottom() + kSubtitleDotGap;
    }
    if (subtitle_ && subtitle_->visible()) {
        dotTop = subtitle_->frame().bottom() + kSubtitleDotGap;
    }
    const Rect icon = c.scale().snap({(b.w - kIconWidth) * 0.5f, iconBottom - kIconHeight, kIconWidth, kIconHeight});
    c.drawIcon(icon_, icon, t.labelTertiary, kIconStroke);

    // Status dot: accent, pulsing gently while something is being waited for.
    const float alpha = pulsing_ ? std::clamp(pulse_, 0.0f, 1.0f) : kPulseMax;
    const Point center{c.scale().snap(b.w * 0.5f), dotTop + kDot * 0.5f};
    c.fillCircle(center, kDot * 0.5f, t.accent.scaledAlpha(alpha));
}

/**
 * @brief Advances the pulse; repaints only when the 24 fps sample changed.
 */
void EmptyState::onFrame(double now) {
    if (!pulsing_) {
        setWantsFrameTicks(false);
        return;
    }
    // First tick after a detached start (or a clock that went backwards).
    if (pulseStart_ <= 0.0 || now < pulseStart_) {
        pulseStart_ = now;
    }
    const double elapsed = now - pulseStart_;

    // Power saving: the dot settles after 30 s.
    if (elapsed >= kPulseAutoStopSec) {
        HH_LOG_DEBUG(kLog, L"pulse auto-stopped after {:.0f} s", elapsed);
        pulsing_ = false;
        pulse_ = kPulseMax;
        setWantsFrameTicks(false);
        invalidate();
        return;
    }

    const float next = pulseAlphaAt(elapsed);
    if (next != pulse_) {
        pulse_ = next;
        invalidate();
    }
}

} // namespace hh::ui
