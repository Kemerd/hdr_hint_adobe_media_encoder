// ---------------------------------------------------------------------------
// Toast.cpp - transient notifications stacked at the bottom of the window.
//
// ToastHost is laid out over the whole root by the OverlayHost; it places its
// ToastView children bottom-centre, newest at the bottom, and only claims
// hits that land on a toast. ToastView is a private widget: an opaque
// rounded panel with a tone dot, wrapping text, an optional action button
// and a close button. It slides in from below, reflows with a spring when
// its neighbours change, and dismisses itself after its duration (paused
// while hovered).
// ---------------------------------------------------------------------------
#include "ui/controls/Toast.h"

#include "core/Logger.h"
#include "ui/anim/Timeline.h"
#include "ui/controls/Button.h"
#include "ui/controls/Label.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Toast";

// ---- metrics ---------------------------------------------------------------
constexpr float kMaxWidth = 340.0f;
constexpr float kRadius = 10.0f;
constexpr float kPadding = 12.0f;
constexpr float kDot = 8.0f;
/// Gap between the tone dot and the text.
constexpr float kDotGap = 8.0f;
/// Gap between text, action button and close button.
constexpr float kGap = 8.0f;
constexpr float kCloseSize = 22.0f;
constexpr float kShadowBlur = 16.0f;
/// Gap between stacked toasts.
constexpr float kStackGap = 8.0f;
/// Side margin that keeps toasts off the window edges in narrow layouts.
constexpr float kSideMargin = 12.0f;
constexpr int kMaxTextLines = 3;

// ---- motion ----------------------------------------------------------------
/// Entry starts this far below its resting position.
constexpr float kSlideIn = 24.0f;
/// Exit drops this far while fading.
constexpr float kSlideOut = 12.0f;
/// When paused mid-way, a toast always gets at least this long after hover ends.
constexpr double kMinRemainingSec = 0.75;

/// Layout treats anything larger than this as unbounded.
constexpr float kUnbounded = 1e8f;

/**
 * @brief Theme colour for a toast tone.
 */
Color toneColor(const Theme& t, ToastTone tone) {
    switch (tone) {
    case ToastTone::Info:    return t.info;
    case ToastTone::Success: return t.success;
    case ToastTone::Warning: return t.warning;
    case ToastTone::Error:   return t.destructive;
    }
    return t.info;
}

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

} // namespace

// ===========================================================================
// ToastView
// ===========================================================================

/**
 * @brief One notification panel owned by a ToastHost.
 */
class ToastView final : public Widget {
public:
    ToastView(ToastHost& host, ToastSpec spec);
    ~ToastView() override;

    /// Slides in and arms the auto-dismiss timer (call once, after adding).
    void present();
    /// Fades and drops out, then asks the host to remove the view.
    void dismiss();
    /// Visual offset from a previous resting position that springs back to zero.
    void nudge(float dy);
    [[nodiscard]] bool dismissing() const noexcept { return dismissing_; }

    Size measure(const Constraints& c) override;
    void onLayout() override;
    void paint(Canvas& c) override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] bool interactive() const override { return true; }
    void onMouseEnter() override;
    void onMouseLeave() override;
    void onAttached() override;
    void onDetached() override;

private:
    /**
     * @brief Sizes of the parts for a given panel width.
     */
    struct Parts {
        float textW = 0.0f;
        Size text;
        Size action;
        Size close;
        float innerH = 0.0f;
    };
    [[nodiscard]] Parts partsFor(float width);
    void armTimer(double seconds);
    void cancelTimer();

    ToastHost& host_;
    ToastSpec spec_;
    Label* text_ = nullptr;
    Button* action_ = nullptr;
    IconButton* close_ = nullptr;
    Animatable<float> slide_{kSlideIn};
    bool dismissing_ = false;
    bool presented_ = false;
    TimerId timer_ = 0;
    Timeline* timerTimeline_ = nullptr;
    double deadline_ = 0.0;
    double remaining_ = 0.0;
};

/**
 * @brief Builds the panel's children from the spec.
 */
ToastView::ToastView(ToastHost& host, ToastSpec spec) : host_(host), spec_(std::move(spec)) {
    slide_.setOwner(this);

    // Body text wraps to at most three lines.
    auto label = std::make_unique<Label>(spec_.text, typography::body(), LabelTone::Primary);
    label->setMaxLines(kMaxTextLines);
    label->setVAlign(VAlign::Top);
    text_ = add(std::move(label));

    // Optional plain action button: runs the handler, then dismisses.
    if (!spec_.actionLabel.empty()) {
        auto button = std::make_unique<Button>(spec_.actionLabel, ButtonKind::Plain, [this] {
            std::function<void()> fn = spec_.onAction;
            if (fn) {
                fn();
            }
            dismiss();
        });
        button->setCompact(true);
        action_ = add(std::move(button));
    }

    // Close button.
    auto closeButton = std::make_unique<IconButton>(IconId::Close, [this] { dismiss(); });
    closeButton->setSize(kCloseSize, kCloseSize);
    closeButton->setTooltipText(L"Dismiss");
    close_ = add(std::move(closeButton));

    // Invisible until present() slides it in.
    opacity_.set(0.0f);
}

/**
 * @brief Cancels a pending timer as a last resort (onDetached normally does it).
 */
ToastView::~ToastView() {
    cancelTimer();
}

/**
 * @brief Springs from 24 dip below with a bounce, fades in, starts the clock.
 */
void ToastView::present() {
    if (presented_) {
        return;
    }
    presented_ = true;
    slide_.setOwner(this);
    opacity_.setOwner(this);
    slide_.set(kSlideIn);
    opacity_.set(0.0f);
    slide_.animateTo(0.0f, springs::bouncy);
    opacity_.animateTo(1.0f, springs::snappy);
    armTimer(spec_.durationSec);
}

/**
 * @brief Fades and drops the panel, then removes it through the host.
 *
 * The removal runs from the opacity spring's completion, which the timeline
 * defers to the end of its tick, so the view may be destroyed there safely.
 * Without a timeline the spring completes synchronously and this object is
 * gone before animateTo returns, hence nothing follows the call.
 */
void ToastView::dismiss() {
    if (dismissing_) {
        return;
    }
    dismissing_ = true;
    cancelTimer();
    // No more clicks on a toast that is leaving.
    setEnabled(false);

    slide_.setOwner(this);
    opacity_.setOwner(this);
    slide_.animateTo(kSlideOut, springs::gentle);
    ToastHost* host = &host_;
    ToastView* self = this;
    opacity_.animateTo(0.0f, springs::snappy, [host, self] { host->remove(self); });
}

/**
 * @brief Adds a visual offset that springs back to the resting position.
 *
 * Used by the host when a neighbour appears or disappears: the toast is
 * laid out at its new spot immediately and visually glides there.
 */
void ToastView::nudge(float dy) {
    if (!std::isfinite(dy) || dy == 0.0f || dismissing_) {
        return;
    }
    slide_.setOwner(this);
    slide_.set(slide_.value() + dy);
    slide_.animateTo(0.0f, springs::snappy);
}

/**
 * @brief Measures the parts for a panel of the given width.
 */
ToastView::Parts ToastView::partsFor(float width) {
    Parts p;
    const float innerW = std::max(0.0f, width - kPadding * 2.0f);

    // Trailing controls first: they decide what the text has left.
    if (close_ && close_->visible()) {
        p.close = close_->measure(looseWidth(innerW));
    }
    if (action_ && action_->visible()) {
        p.action = action_->measure(looseWidth(innerW));
    }
    float used = kDot + kDotGap;
    if (p.close.w > 0.0f) {
        used += p.close.w + kGap;
    }
    if (p.action.w > 0.0f) {
        used += p.action.w + kGap;
    }
    p.textW = std::max(0.0f, innerW - used);

    // Text wraps within what is left.
    if (text_ && text_->visible()) {
        p.text = text_->measure(looseWidth(p.textW));
    }
    p.innerH = std::max({p.text.h, p.action.h, p.close.h, kDot});
    return p;
}

/**
 * @brief Natural size: up to 340 dip wide, as tall as its wrapped text needs.
 */
Size ToastView::measure(const Constraints& c) {
    const bool bounded = c.maxW < kUnbounded;
    const float width = bounded ? std::min(kMaxWidth, std::max(0.0f, c.maxW)) : kMaxWidth;
    const Parts p = partsFor(width);
    return c.constrain({width, p.innerH + kPadding * 2.0f});
}

/**
 * @brief Places text, action and close button inside the padding.
 */
void ToastView::onLayout() {
    const Rect b = bounds();
    const Parts p = partsFor(b.w);
    const float innerH = std::max(0.0f, b.h - kPadding * 2.0f);

    // Close button hugs the right edge, centred vertically.
    float right = b.w - kPadding;
    if (close_ && close_->visible()) {
        const float x = std::max(kPadding, right - p.close.w);
        const float y = kPadding + std::max(0.0f, (innerH - p.close.h) * 0.5f);
        close_->layout({x, y, p.close.w, p.close.h});
        right = x - kGap;
    }

    // Action button sits left of the close button.
    if (action_ && action_->visible()) {
        const float x = std::max(kPadding, right - p.action.w);
        const float y = kPadding + std::max(0.0f, (innerH - p.action.h) * 0.5f);
        action_->layout({x, y, p.action.w, p.action.h});
        right = x - kGap;
    }

    // Text after the dot, vertically centred in the inner area.
    if (text_ && text_->visible()) {
        const float x = kPadding + kDot + kDotGap;
        const float w = std::max(0.0f, std::min(p.textW, right - x));
        const float y = kPadding + std::max(0.0f, (innerH - p.text.h) * 0.5f);
        text_->layout({x, y, w, p.text.h});
    }
}

/**
 * @brief Paints with the slide offset applied on top of the usual frame translation.
 */
void ToastView::paint(Canvas& c) {
    const float dy = slide_.value();
    const bool sliding = std::isfinite(dy) && dy != 0.0f;
    if (sliding) {
        c.pushTransform(D2D1::Matrix3x2F::Translation(0.0f, c.scale().snap(dy)));
    }
    Widget::paint(c);
    if (sliding) {
        c.pop();
    }
}

/**
 * @brief Shadow, opaque elevated panel, hairline outline and the tone dot.
 */
void ToastView::paintSelf(Canvas& c) {
    const Rect raw = bounds();
    if (raw.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    const Rect b = c.scale().snap(raw);

    c.drawShadow(b, kRadius, kShadowBlur, t.shadow);
    c.fillRoundedRect(b, kRadius, t.elevatedOpaque);
    c.strokeRoundedRect(b, kRadius, t.separator, 0.0f);

    // Tone dot aligned with the first text line.
    float cy = b.y + b.h * 0.5f;
    if (text_ && text_->visible() && !text_->frame().isEmpty()) {
        const TextCache::LineMetrics lm = lineMetricsShared(text_->style());
        const float lineH = lm.lineHeight > 0.0f ? lm.lineHeight : 1.3f * text_->style().size;
        cy = text_->frame().y + lineH * 0.5f;
    }
    c.fillCircle({b.x + kPadding + kDot * 0.5f, cy}, kDot * 0.5f, toneColor(t, spec_.tone));
}

/**
 * @brief Hover pauses the auto-dismiss clock.
 */
void ToastView::onMouseEnter() {
    setHovered(true);
    if (timer_ == 0 || !timerTimeline_) {
        return;
    }
    // Remember what is left so the countdown resumes rather than restarts.
    remaining_ = std::max(kMinRemainingSec, deadline_ - timerTimeline_->now());
    cancelTimer();
}

/**
 * @brief Hover out resumes the countdown with the remaining time.
 */
void ToastView::onMouseLeave() {
    setHovered(false);
    if (dismissing_ || spec_.durationSec <= 0.0) {
        return;
    }
    armTimer(remaining_ > 0.0 ? remaining_ : spec_.durationSec);
}

/**
 * @brief A toast shown while detached arms its clock once it has a timeline.
 */
void ToastView::onAttached() {
    if (presented_ && !dismissing_ && timer_ == 0) {
        armTimer(remaining_ > 0.0 ? remaining_ : spec_.durationSec);
    }
}

/**
 * @brief Leaving the tree cancels the timer while the timeline is still reachable.
 */
void ToastView::onDetached() {
    if (timer_ != 0 && timerTimeline_) {
        remaining_ = std::max(kMinRemainingSec, deadline_ - timerTimeline_->now());
    }
    cancelTimer();
}

/**
 * @brief Schedules the auto-dismiss (0 or less = sticky).
 */
void ToastView::armTimer(double seconds) {
    if (!(seconds > 0.0) || dismissing_) {
        return;
    }
    Timeline* tl = timeline();
    if (!tl) {
        // No clock yet: onAttached() will arm it.
        return;
    }
    cancelTimer();
    deadline_ = tl->now() + seconds;
    timerTimeline_ = tl;
    timer_ = tl->addTimer(deadline_, [this] {
        timer_ = 0;
        timerTimeline_ = nullptr;
        dismiss();
    });
    if (timer_ == 0) {
        timerTimeline_ = nullptr;
        HH_LOG_WARN(kLog, L"failed to arm the toast timer");
    }
}

/**
 * @brief Cancels a pending auto-dismiss.
 */
void ToastView::cancelTimer() {
    if (timer_ != 0 && timerTimeline_) {
        timerTimeline_->cancelTimer(timer_);
    }
    timer_ = 0;
    timerTimeline_ = nullptr;
}

// ===========================================================================
// ToastHost
// ===========================================================================

/**
 * @brief Creates the (empty) host; it covers the root and paints nothing itself.
 */
ToastHost::ToastHost() = default;

/**
 * @brief Shows a toast, dismissing the oldest ones beyond the visible cap.
 */
void ToastHost::show(ToastSpec spec) {
    if (spec.text.empty() && spec.actionLabel.empty()) {
        HH_LOG_DEBUG(kLog, L"show: empty toast ignored");
        return;
    }
    if (!std::isfinite(spec.durationSec)) {
        spec.durationSec = 0.0;
    }

    // Count the live toasts and retire the oldest until there is room.
    int live = 0;
    for (const auto& child : children()) {
        auto* view = dynamic_cast<ToastView*>(child.get());
        if (view && !view->dismissing()) {
            ++live;
        }
    }
    for (const auto& child : children()) {
        if (live < kMaxVisible) {
            break;
        }
        auto* view = dynamic_cast<ToastView*>(child.get());
        if (view && !view->dismissing()) {
            view->dismiss();
            --live;
        }
    }

    ToastView* view = add(std::make_unique<ToastView>(*this, std::move(spec)));
    if (!view) {
        return;
    }
    view->present();
    invalidateLayout();
}

/**
 * @brief Dismisses the newest live toast.
 */
bool ToastHost::dismissTop() {
    const auto& kids = children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        auto* view = dynamic_cast<ToastView*>(it->get());
        if (view && !view->dismissing()) {
            view->dismiss();
            return true;
        }
    }
    return false;
}

/**
 * @brief Fills whatever it is given: the overlay lays it over the whole root.
 */
Size ToastHost::measure(const Constraints& c) {
    const float w = (c.maxW < 1e8f) ? std::max(0.0f, c.maxW) : 0.0f;
    const float h = (c.maxH < 1e8f) ? std::max(0.0f, c.maxH) : 0.0f;
    return c.constrain({w, h});
}

/**
 * @brief Stacks the toasts bottom-centre, newest at the bottom.
 *
 * A toast that already had a spot and moves vertically (a neighbour came or
 * went) is nudged so it glides to the new position; window resizes change
 * x as well and are not animated.
 */
void ToastHost::layout(const Rect& frame) {
    Widget::layout(frame);

    const Rect b = bounds();
    const float maxW = std::min(kMaxWidth, std::max(0.0f, b.w - kSideMargin * 2.0f));
    float bottom = b.h - std::max(0.0f, bottomInset_);

    const auto& kids = children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        Widget* child = it->get();
        if (!child || !child->visible()) {
            continue;
        }
        Size s = child->measure(looseWidth(maxW));
        s.w = std::clamp(s.w, 0.0f, std::max(0.0f, b.w));
        s.h = std::max(0.0f, s.h);

        const float x = std::round((b.w - s.w) * 0.5f);
        const float y = bottom - s.h;
        const Rect prev = child->frame();
        child->layout({x, y, s.w, s.h});

        // Reflow animation: same column, different row.
        const bool hadSpot = prev.w > 0.0f && prev.h > 0.0f;
        if (hadSpot && std::fabs(prev.x - x) < 0.5f && std::fabs(prev.y - y) >= 0.5f) {
            if (auto* view = dynamic_cast<ToastView*>(child)) {
                view->nudge(prev.y - y);
            }
        }
        bottom = y - kStackGap;
    }
}

/**
 * @brief Only the toasts themselves take input; everything else falls through.
 */
Widget* ToastHost::hitTest(Point local) {
    if (!visible() || !enabled()) {
        return nullptr;
    }
    const auto& kids = children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        Widget* child = it->get();
        if (!child || !child->visible() || !child->frame().contains(local)) {
            continue;
        }
        if (Widget* hit = child->hitTest(local - child->frame().origin())) {
            return hit;
        }
    }
    return nullptr;
}

/**
 * @brief Removes (and destroys) a toast after its exit animation.
 */
void ToastHost::remove(ToastView* view) {
    if (!view) {
        return;
    }
    std::unique_ptr<Widget> owned = removeChild(view);
    if (!owned) {
        HH_LOG_DEBUG(kLog, L"remove: toast was not a child");
        return;
    }
    owned.reset();
    invalidateLayout();
}

} // namespace hh::ui
