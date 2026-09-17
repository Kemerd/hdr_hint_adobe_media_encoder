// ---------------------------------------------------------------------------
// ScrollView.cpp - vertical scrolling with spring wheel scrolling, touchpad
// inertia, rubber-band overscroll and a fading overlay scrollbar.
//
// Two kinds of input feed one offset:
//   * mouse-wheel notches move a target that the offset spring chases
//     (deterministic, never rubber-bands: mice do not on macOS either);
//   * precision deltas (touchpad) move the offset directly, may run past
//     the edges (the rubber band compresses the excess on screen), and hand
//     over to an inertia phase 80 ms after the last event, which finally
//     springs back to the nearest edge when it stops overscrolled.
//
// The content is laid out at its full measured height and painted through a
// clip, translated by the displayed (rubber-banded) offset. Hit-testing
// applies the same translation so children receive local coordinates that
// match what they see on screen.
// ---------------------------------------------------------------------------
#include "ui/controls/ScrollView.h"

#include "core/Logger.h"
#include "platform/Time.h"
#include "ui/core/RootView.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"ScrollView";

// ---- wheel -----------------------------------------------------------------
/// Distance of one notch at the default three lines per notch.
constexpr float kNotchDips = 48.0f;
constexpr int kDefaultLinesPerNotch = 3;
/// SPI_GETWHEELSCROLLLINES reports this for "one screen at a time".
constexpr int kPageScroll = -1;
/// After a precision delta, every wheel event counts as precision for this long.
constexpr double kPreciseLatchSec = 0.5;

// ---- inertia / rubber band -------------------------------------------------
/// Quiet time after the last precision event before coasting starts.
constexpr double kInertiaDelaySec = 0.08;
/// Coasting stops below this speed (dips per second).
constexpr float kMinVelocity = 4.0f;
/// Velocity decay per millisecond while coasting (macOS constant).
constexpr float kDecayPerMs = 0.998f;
/// Much faster decay while coasting past an edge (the band resists).
constexpr float kDecayBeyondEdgePerMs = 0.98f;
/// Apple's rubber-band coefficient.
constexpr float kRubberCoeff = 0.55f;
/// Weight of the newest sample in the velocity moving average.
constexpr float kVelocityEma = 0.6f;
/// Longest frame step integrated (a stall never teleports the content).
constexpr double kMaxFrameDt = 0.1;
/// Fallback delta time between two precision events with a broken clock.
constexpr double kNominalFrameDt = 1.0 / 60.0;
/// Spring that pulls an overscrolled offset back to the edge.
constexpr SpringParams kOverscrollSpring{0.35f, 0.85f, 0.0005f};
/// Extra room kept when scrolling a widget into view.
constexpr float kIntoViewMargin = 8.0f;

// ---- scrollbar -------------------------------------------------------------
constexpr float kBarWidth = 6.0f;
constexpr float kBarWidthHover = 10.0f;
constexpr float kBarInset = 2.0f;
constexpr float kThumbMinHeight = 24.0f;
/// Hovering this close to the right edge widens the bar and shows the track.
constexpr float kBarHoverZone = 12.0f;
constexpr double kBarHideDelaySec = 0.8;
constexpr float kTrackAlpha = 0.06f;
/// Thumb opacity at rest. The bar never disappears completely while there
/// is something to scroll: in a small docked panel an invisible scrollbar
/// reads as "this is all there is".
constexpr float kBarRestAlpha = 0.45f;

/// Layout treats anything larger than this as unbounded.
constexpr float kUnbounded = 1e8f;

/**
 * @brief Apple's rubber-band curve: how far an overscroll of @p over is shown.
 * @param over  raw distance past the edge (>= 0)
 * @param dim   viewport height
 */
float rubberBand(float over, float dim) {
    if (over <= 0.0f || dim <= 0.0f) {
        return 0.0f;
    }
    return dim * (1.0f - 1.0f / ((over * kRubberCoeff / dim) + 1.0f));
}

/**
 * @brief Finds @p target in the live tree below @p node by pointer comparison.
 *
 * Never dereferences @p target: it may be a dangling pointer captured by a
 * timer that outlived its widget. Only a pointer that is still reachable
 * from the root is returned, and that one is alive by definition.
 */
Widget* findInTree(Widget* node, const Widget* target) {
    if (!node || !target) {
        return nullptr;
    }
    if (node == target) {
        return node;
    }
    for (const auto& child : node->children()) {
        if (Widget* found = findInTree(child.get(), target)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

/**
 * @brief Creates an empty scroll view.
 */
ScrollView::ScrollView() {
    offset_.setOwner(this);
    barOpacity_.setOwner(this);
    stack().clipsChildren = true;
}

/**
 * @brief Replaces the scrolled content and resets the offset.
 */
Widget* ScrollView::setContent(std::unique_ptr<Widget> content) {
    if (content_) {
        std::unique_ptr<Widget> old = removeChild(content_);
        old.reset();
        content_ = nullptr;
    }
    // Scroll state belongs to the old content.
    inertia_ = false;
    overscrolling_ = false;
    velocity_ = 0.0f;
    target_ = 0.0f;
    offset_.set(0.0f);

    if (!content) {
        HH_LOG_DEBUG(kLog, L"setContent: content cleared");
        invalidateLayout();
        return nullptr;
    }
    content_ = addChild(std::move(content));
    invalidateLayout();
    return content_;
}

/**
 * @brief Scrolls to @p y (clamped), with the scroll spring or instantly.
 */
void ScrollView::scrollTo(float y, bool animated) {
    // Programmatic scrolling cancels whatever gesture was in flight.
    inertia_ = false;
    overscrolling_ = false;
    velocity_ = 0.0f;

    const float maxO = std::max(0.0f, maxOffset_);
    target_ = std::clamp(std::isfinite(y) ? y : 0.0f, 0.0f, maxO);
    offset_.setOwner(this);
    if (animated && timeline()) {
        offset_.animateTo(target_, springs::scroll);
    } else {
        offset_.set(target_);
    }
    showBar();
}

/**
 * @brief Scrolls relative to the current target (so repeated calls accumulate).
 */
void ScrollView::scrollBy(float dy, bool animated) {
    const float base = offset_.animating() ? offset_.target() : offset_.value();
    scrollTo(base + (std::isfinite(dy) ? dy : 0.0f), animated);
}

/**
 * @brief Scrolls to the end of the content.
 */
void ScrollView::scrollToBottom(bool animated) {
    scrollTo(maxOffset_, animated);
}

/**
 * @brief Scrolls the minimum distance that makes @p w fully visible.
 *
 * Children are laid out in unscrolled content space (the scroll offset is a
 * paint-time translation), so the difference of root positions is exactly
 * the widget's position within the content.
 */
void ScrollView::scrollIntoView(Widget* w) {
    if (!w || !content_) {
        return;
    }
    // Only descendants make sense here.
    bool descendant = false;
    for (const Widget* p = w->parent(); p != nullptr; p = p->parent()) {
        if (p == this) {
            descendant = true;
            break;
        }
    }
    if (!descendant) {
        HH_LOG_DEBUG(kLog, L"scrollIntoView: widget is not inside this view");
        return;
    }
    const float vh = frame_.h;
    if (vh <= 0.0f) {
        return;
    }

    // Position of the widget relative to the (unscrolled) view origin.
    const Point widgetTop = w->toRoot({0.0f, 0.0f});
    const Point viewTop = toRoot({0.0f, 0.0f});
    const float top = widgetTop.y - viewTop.y;
    const float bottom = top + std::max(0.0f, w->frame().h);

    // Compare against where the view is heading, not where it happens to be mid-spring.
    const float maxO = std::max(0.0f, maxOffset_);
    const float current = offset_.animating() ? offset_.target() : std::clamp(offset_.value(), 0.0f, maxO);
    float wanted = current;
    if (top < current) {
        wanted = top - kIntoViewMargin;
    } else if (bottom > current + vh) {
        wanted = bottom - vh + kIntoViewMargin;
    }
    if (wanted != current) {
        scrollTo(wanted, true);
    }
}

/**
 * @brief Takes the bounded size it is given; hugs the content when unbounded.
 */
Size ScrollView::measure(const Constraints& c) {
    const bool boundedW = c.maxW < kUnbounded;
    const bool boundedH = c.maxH < kUnbounded;
    float w = boundedW ? std::max(0.0f, c.maxW) : 0.0f;
    float h = boundedH ? std::max(0.0f, c.maxH) : 0.0f;

    // The content is measured at the view width (minus insets) with no height
    // bound; that is the height the view will scroll over.
    if (content_ && content_->visible()) {
        Constraints cc;
        cc.minH = 0.0f;
        cc.maxH = 1e9f;
        if (boundedW) {
            const float cw = std::max(0.0f, w - insets_.horizontal());
            cc.minW = cw;
            cc.maxW = cw;
        } else {
            cc.minW = 0.0f;
            cc.maxW = 1e9f;
        }
        const Size s = content_->measure(cc);
        if (!boundedW) {
            w = std::max(0.0f, s.w) + insets_.horizontal();
        }
        if (!boundedH) {
            h = std::max(0.0f, s.h) + insets_.vertical();
        }
    }
    return c.constrain({w, h});
}

/**
 * @brief Lays the content out at its full height and recomputes the scroll range.
 *
 * The base implementation stores the frame and clears the dirty flag; its
 * stack pass hands the content a viewport-sized placeholder frame which is
 * immediately replaced by the real one below.
 */
void ScrollView::layout(const Rect& frame) {
    Widget::layout(frame);

    const Rect b = bounds();
    const float cw = std::max(0.0f, b.w - insets_.horizontal());
    contentHeight_ = 0.0f;
    if (content_ && content_->visible()) {
        Constraints cc;
        cc.minW = cw;
        cc.maxW = cw;
        cc.minH = 0.0f;
        cc.maxH = 1e9f;
        const Size s = content_->measure(cc);
        contentHeight_ = std::max(0.0f, s.h);
        content_->layout({insets_.left, insets_.top, cw, contentHeight_});
    }

    // Range: everything that does not fit into the viewport.
    const float total = contentHeight_ + insets_.vertical();
    const float previousMax = maxOffset_;
    maxOffset_ = std::max(0.0f, total - b.h);
    clampAndSettle();

    // Content that has just become scrollable (first layout, a window resize,
    // a job card appearing) shows the bar without waiting for input, so the
    // panel never looks like it is showing everything it has.
    if (maxOffset_ > 0.0f && previousMax <= 0.0f) {
        showBar();
    } else if (maxOffset_ <= 0.0f && previousMax > 0.0f) {
        // Nothing left to scroll: retire the bar instead of parking it at the
        // resting alpha over content that fits.
        barOpacity_.setOwner(this);
        if (timeline()) {
            barOpacity_.animateTo(0.0f, springs::gentle);
        } else {
            barOpacity_.set(0.0f);
        }
    }
}

/**
 * @brief Paints the chrome, the clipped and translated content, then the bar.
 */
void ScrollView::paint(Canvas& c) {
    if (!visible()) {
        return;
    }
    const float op = std::clamp(opacity_.value(), 0.0f, 1.0f);
    if (op <= 0.0f) {
        return;
    }
    const bool faded = op < 1.0f;
    if (faded) {
        c.pushOpacity(op);
    }
    c.pushTransform(D2D1::Matrix3x2F::Translation(frame_.x, frame_.y));

    paintSelf(c);

    // Remember the raw overscroll for the thumb compression in paintOverlay.
    const float raw = offset_.value();
    const float maxO = std::max(0.0f, maxOffset_);
    overscroll_ = (raw < 0.0f) ? raw : (raw > maxO ? raw - maxO : 0.0f);

    // Content scrolls inside the clip; the translation is snapped to whole
    // pixels so glyphs stay crisp while the view moves.
    const float shown = c.scale().snap(displayedOffset());
    c.pushClip(bounds());
    c.pushTransform(D2D1::Matrix3x2F::Translation(0.0f, -shown));
    for (const auto& child : children()) {
        if (child && child->visible()) {
            child->paint(c);
        }
    }
    c.pop();   // content translation
    c.pop();   // clip

    paintOverlay(c);

    c.pop();   // frame translation
    if (faded) {
        c.pop();
    }
}

/**
 * @brief Draws the overlay scrollbar (track only while hovered/dragged).
 */
void ScrollView::paintOverlay(Canvas& c) {
    const float alpha = std::clamp(barOpacity_.value(), 0.0f, 1.0f);
    if (alpha <= 0.001f) {
        return;
    }
    const Rect thumb = thumbRect();
    if (thumb.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    const Rect b = bounds();

    // Track: a faint strip behind the thumb, only when the bar is engaged.
    if (barHover_ || draggingThumb_) {
        const Rect track{thumb.x, kBarInset, thumb.w, std::max(0.0f, b.h - kBarInset * 2.0f)};
        c.fillRoundedRect(c.scale().snap(track), thumb.w * 0.5f, t.separator.withAlpha(kTrackAlpha * alpha));
    }

    // Thumb: fully rounded capsule in the theme's scroll-thumb colour.
    c.fillRoundedRect(c.scale().snap(thumb), thumb.w * 0.5f, t.scrollThumb.scaledAlpha(alpha));
}

/**
 * @brief Hit-tests children with the scroll translation applied.
 *
 * The strip along the right edge always belongs to the view while there is
 * something to scroll, so the thumb can be grabbed even over a card.
 */
Widget* ScrollView::hitTest(Point local) {
    if (!visible() || !enabled() || !bounds().contains(local)) {
        return nullptr;
    }
    if (maxOffset_ > 0.0f && nearRightEdge(local)) {
        return this;
    }

    // Children see the same coordinates they were painted with.
    const float shown = displayedOffset();
    const auto& kids = children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        Widget* child = it->get();
        if (!child || !child->visible()) {
            continue;
        }
        Point childLocal = local - child->frame().origin();
        childLocal.y += shown;
        if (Widget* hit = child->hitTest(childLocal)) {
            return hit;
        }
    }
    return hitTestSelf(local) ? this : nullptr;
}

/**
 * @brief Wheel input: notches spring towards a target, precision deltas move directly.
 * @return false when there is nothing to scroll so an outer view may take the event
 */
bool ScrollView::onWheel(const WheelEvent& e) {
    if (!visible() || !enabled()) {
        return false;
    }
    const float vh = std::max(0.0f, frame_.h);
    const float maxO = std::max(0.0f, maxOffset_);
    if (maxO <= 0.0f && !overscrolling_ && offset_.value() == 0.0f) {
        return false;
    }
    if (!std::isfinite(e.delta) || e.delta == 0.0f) {
        return true;
    }

    Timeline* tl = timeline();
    const double now = tl ? tl->now() : platform::nowMonotonicSeconds();
    offset_.setOwner(this);

    // Latch: a touchpad flick that happens to land on a multiple of 120 must
    // not become a notch jump in the middle of a gesture.
    const bool precise = e.precise || (lastPreciseAt_ > 0.0 && (now - lastPreciseAt_) < kPreciseLatchSec);

    if (!precise) {
        // Notch: retarget the spring; mice never rubber-band.
        inertia_ = false;
        overscrolling_ = false;
        velocity_ = 0.0f;
        lastPreciseAt_ = 0.0;

        float step = 0.0f;
        if (e.linesPerNotch == kPageScroll) {
            step = std::max(1.0f, vh);
        } else {
            const int lines = e.linesPerNotch > 0 ? e.linesPerNotch : kDefaultLinesPerNotch;
            step = kNotchDips * static_cast<float>(lines) / static_cast<float>(kDefaultLinesPerNotch);
        }
        // A finished spring leaves target_ stale; pick the chain up from the
        // real position (clamped in case a touchpad left us overscrolled).
        if (!offset_.animating()) {
            target_ = std::clamp(offset_.value(), 0.0f, maxO);
        }
        target_ = std::clamp(target_ + (-e.delta / WHEEL_DELTA) * step, 0.0f, maxO);
        if (tl) {
            offset_.animateTo(target_, springs::scroll);
        } else {
            offset_.set(target_);
        }
        lastInputAt_ = now;
        showBar();
        return true;
    }

    // Precision: move immediately, keep a velocity estimate for the inertia.
    const float dy = (-e.delta / WHEEL_DELTA) * kNotchDips;
    double dt = now - lastInputAt_;
    if (!(dt > 0.0) || dt > 0.25) {
        dt = kNominalFrameDt;
    }
    float raw = offset_.value() + dy;
    // One viewport past either edge is all the band will ever show.
    raw = std::clamp(raw, -vh, maxO + vh);
    offset_.set(raw);
    target_ = std::clamp(raw, 0.0f, maxO);

    const float instant = static_cast<float>(static_cast<double>(dy) / dt);
    velocity_ = velocity_ * (1.0f - kVelocityEma) + instant * kVelocityEma;
    inertia_ = false;
    overscrolling_ = false;
    lastInputAt_ = now;
    lastPreciseAt_ = now;

    // Frame ticks watch for the end of the gesture (see onFrame).
    setWantsFrameTicks(true);
    showBar();
    return true;
}

/**
 * @brief Left press on the thumb starts a drag; on the track it jumps there.
 */
bool ScrollView::onMouseDown(const MouseEvent& e) {
    if (!enabled() || e.button != MouseButton::Left) {
        return false;
    }
    const Rect thumb = thumbRect();
    if (thumb.isEmpty() || !nearRightEdge(e.pos)) {
        return false;
    }
    const float maxO = std::max(0.0f, maxOffset_);

    if (thumb.contains(e.pos)) {
        // Grab: freeze any motion at a valid offset and remember the start.
        inertia_ = false;
        overscrolling_ = false;
        velocity_ = 0.0f;
        dragStartOffset_ = std::clamp(offset_.value(), 0.0f, maxO);
        dragStartY_ = e.pos.y;
        offset_.setOwner(this);
        offset_.set(dragStartOffset_);
        target_ = dragStartOffset_;
        draggingThumb_ = true;
        showBar();
        invalidate();
        return true;
    }

    // Track click: centre the thumb on the pointer.
    const float trackH = std::max(1.0f, frame_.h - kBarInset * 2.0f);
    const float travel = std::max(1.0f, trackH - thumb.h);
    const float ratio = std::clamp((e.pos.y - kBarInset - thumb.h * 0.5f) / travel, 0.0f, 1.0f);
    scrollTo(ratio * maxO, true);
    return true;
}

/**
 * @brief Ends a thumb drag; the bar fades after its usual delay.
 */
bool ScrollView::onMouseUp(const MouseEvent&) {
    if (!draggingThumb_) {
        return false;
    }
    draggingThumb_ = false;
    showBar();
    invalidate();
    return true;
}

/**
 * @brief Drags the thumb, or tracks whether the pointer hovers the bar zone.
 */
bool ScrollView::onMouseMove(const MouseEvent& e) {
    if (draggingThumb_) {
        // Pointer travel over the thumb's travel range maps onto the scroll range.
        const Rect thumb = thumbRect();
        const float trackH = std::max(0.0f, frame_.h - kBarInset * 2.0f);
        const float travel = std::max(1.0f, trackH - thumb.h);
        const float maxO = std::max(0.0f, maxOffset_);
        const float dy = e.pos.y - dragStartY_;
        const float next = std::clamp(dragStartOffset_ + dy * (maxO / travel), 0.0f, maxO);
        offset_.setOwner(this);
        offset_.set(next);
        target_ = next;
        showBar();
        return true;
    }

    // Widen the bar and reveal the track while the pointer is by the edge.
    const bool overBar = maxOffset_ > 0.0f && nearRightEdge(e.pos);
    if (overBar != barHover_) {
        barHover_ = overBar;
        if (overBar) {
            showBar();
        }
        invalidate();
    }
    return overBar;
}

/**
 * @brief Hover in: nothing beyond the hover flag (the bar shows on scroll).
 */
void ScrollView::onMouseEnter() {
    setHovered(true);
}

/**
 * @brief Hover out: release the bar so it can fade.
 */
void ScrollView::onMouseLeave() {
    setHovered(false);
    if (barHover_) {
        barHover_ = false;
        invalidate();
        // A held-open bar starts its hide countdown now.
        if (barOpacity_.target() > 0.0f) {
            showBar();
        }
    }
}

/**
 * @brief Per-frame work: end-of-gesture detection, inertia and tick housekeeping.
 *
 * Ticks are only requested while a precision gesture is live or the view is
 * coasting; the springs (offset, overscroll return, bar fade) drive their own
 * frames through the timeline.
 */
void ScrollView::onFrame(double now) {
    const float vh = std::max(0.0f, frame_.h);
    const float maxO = std::max(0.0f, maxOffset_);
    offset_.setOwner(this);

    // A precision gesture is live until it has been quiet for 80 ms.
    const bool gestureLive = lastPreciseAt_ > 0.0 && (now - lastPreciseAt_) < kInertiaDelaySec;

    // Hand-over: the touchpad went quiet, so either coast or settle.
    if (!gestureLive && !inertia_ && velocity_ != 0.0f) {
        if (std::fabs(velocity_) >= kMinVelocity && !draggingThumb_) {
            inertia_ = true;
            // Integrate from now: the content stood still during the quiet gap.
            lastInputAt_ = now;
        } else {
            velocity_ = 0.0f;
            clampAndSettle();
        }
    }

    // Coasting: integrate the velocity and let it decay (faster past an edge).
    if (inertia_) {
        double dt = now - lastInputAt_;
        if (!(dt > 0.0)) {
            dt = 0.0;
        }
        dt = std::min(dt, kMaxFrameDt);
        lastInputAt_ = now;

        float raw = offset_.value() + velocity_ * static_cast<float>(dt);
        const bool beyond = raw < 0.0f || raw > maxO;
        raw = std::clamp(raw, -vh, maxO + vh);
        offset_.set(raw);

        const float decay = beyond ? kDecayBeyondEdgePerMs : kDecayPerMs;
        velocity_ *= std::pow(decay, static_cast<float>(dt * 1000.0));
        if (std::fabs(velocity_) < kMinVelocity) {
            velocity_ = 0.0f;
            inertia_ = false;
            clampAndSettle();
        }
    }

    // Stop ticking as soon as nothing needs a per-frame step.
    const bool busy = gestureLive || inertia_ || draggingThumb_ || overscrolling_ || offset_.animating() ||
                      barOpacity_.animating();
    if (!busy) {
        setWantsFrameTicks(false);
    }
}

/**
 * @brief The thumb rectangle in local dips (empty when the content fits).
 *
 * Overscroll compresses the thumb against the edge it is pinned to, the
 * way macOS does, so the user sees the band even with the bar alone.
 */
Rect ScrollView::thumbRect() const {
    const Rect b = bounds();
    const float total = contentHeight_ + insets_.vertical();
    if (b.h <= 0.0f || b.w <= 0.0f || total <= b.h + 0.5f) {
        return {};
    }
    const float trackH = std::max(0.0f, b.h - kBarInset * 2.0f);
    if (trackH <= 0.0f) {
        return {};
    }

    // Proportional height with a minimum so it stays grabbable.
    float thumbH = std::clamp(trackH * (b.h / total), kThumbMinHeight, trackH);

    // Compression while overscrolled.
    const float shown = displayedOffset();
    const float maxO = std::max(0.0f, maxOffset_);
    float over = 0.0f;
    if (shown < 0.0f) {
        over = -shown;
    } else if (shown > maxO) {
        over = shown - maxO;
    }
    thumbH = std::max(kThumbMinHeight * 0.5f, thumbH - over);

    // Position along the track from the clamped offset.
    const float ratio = (maxO > 0.0f) ? std::clamp(shown, 0.0f, maxO) / maxO : 0.0f;
    float y = kBarInset + (trackH - thumbH) * ratio;
    if (shown < 0.0f) {
        y = kBarInset;
    } else if (shown > maxO) {
        y = kBarInset + trackH - thumbH;
    }

    const float w = (barHover_ || draggingThumb_) ? kBarWidthHover : kBarWidth;
    return {b.w - kBarInset - w, y, w, thumbH};
}

/**
 * @brief True within the 12 dip strip along the right edge.
 */
bool ScrollView::nearRightEdge(Point local) const {
    return local.x >= frame_.w - kBarHoverZone && local.x <= frame_.w && local.y >= 0.0f && local.y <= frame_.h;
}

/**
 * @brief Brings the offset (or its target) back into [0, maxOffset].
 *
 * Called after layout (the range may have shrunk) and when a gesture ends
 * overscrolled: the return uses the slightly bouncy overscroll spring.
 * Interactive phases (drag, inertia) keep their own bounds.
 */
void ScrollView::clampAndSettle() {
    if (draggingThumb_ || inertia_) {
        return;
    }
    const float maxO = std::max(0.0f, maxOffset_);
    offset_.setOwner(this);

    // A spring in flight is retargeted when its destination fell out of range.
    if (offset_.animating()) {
        const float tgt = offset_.target();
        const float clamped = std::clamp(tgt, 0.0f, maxO);
        if (clamped != tgt) {
            target_ = clamped;
            offset_.animateTo(clamped, springs::scroll, [this] { overscrolling_ = false; });
        }
        return;
    }

    // At rest: nothing to do inside the range.
    const float raw = offset_.value();
    const float edge = std::clamp(raw, 0.0f, maxO);
    if (edge == raw) {
        overscrolling_ = false;
        return;
    }
    target_ = edge;
    if (!timeline()) {
        offset_.set(edge);
        overscrolling_ = false;
        return;
    }
    overscrolling_ = true;
    offset_.animateTo(edge, kOverscrollSpring, [this] { overscrolling_ = false; });
}

/**
 * @brief Reveals the bar and (re)starts the 800 ms hide countdown.
 *
 * The countdown is a timeline timer. Its callback only touches the view
 * after confirming, through the live widget tree, that the view still
 * exists: a screen may be torn down while the timer is pending.
 */
void ScrollView::showBar() {
    Timeline* tl = timeline();
    if (!tl) {
        return;
    }
    barOpacity_.setOwner(this);
    barOpacity_.animateTo(1.0f, springs::snappy);

    if (barTimer_ != 0) {
        tl->cancelTimer(barTimer_);
        barTimer_ = 0;
    }
    RootView* rootView = root();
    ScrollView* self = this;
    barTimer_ = tl->addTimerIn(kBarHideDelaySec, [rootView, self] {
        Widget* live = findInTree(rootView, self);
        ScrollView* view = live ? dynamic_cast<ScrollView*>(live) : nullptr;
        if (!view) {
            return;
        }
        view->barTimer_ = 0;
        // An engaged bar stays; it fades once the pointer leaves the strip.
        if (view->barHover_ || view->draggingThumb_) {
            return;
        }
        // Settle to the resting alpha, not to nothing, whenever the content
        // actually overflows; a view with nothing to scroll hides the bar.
        view->barOpacity_.setOwner(view);
        view->barOpacity_.animateTo(view->maxOffset_ > 0.0f ? kBarRestAlpha : 0.0f, springs::gentle);
    });
}

/**
 * @brief The offset actually painted: the raw offset with the rubber band
 *        applied past either edge.
 */
float ScrollView::displayedOffset() const {
    const float raw = offset_.value();
    if (!std::isfinite(raw)) {
        return 0.0f;
    }
    const float maxO = std::max(0.0f, maxOffset_);
    const float dim = std::max(1.0f, frame_.h);
    if (raw < 0.0f) {
        return -rubberBand(-raw, dim);
    }
    if (raw > maxO) {
        return maxO + rubberBand(raw - maxO, dim);
    }
    return raw;
}

} // namespace hh::ui
