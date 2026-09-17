// ---------------------------------------------------------------------------
// Card.cpp - rounded elevated container with animated insert/remove.
//
// A card is a plain stack container with chrome: a translucent elevated fill,
// a hairline outline that strengthens on hover, and an accent / destructive
// outline for the selected and failed states. Insert and remove are driven by
// two springs: heightScale_ (applied in measure(), so neighbours reflow every
// frame) and the widget's own opacity.
// ---------------------------------------------------------------------------
#include "ui/controls/Card.h"

#include "core/Logger.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Card";

// ---- metrics ---------------------------------------------------------------
constexpr float kPadding = 12.0f;
/// Selected / destructive outlines are a full dip so they read at high DPI.
constexpr float kStateOutlineWidth = 1.0f;
constexpr float kSelectedAlpha = 0.60f;
constexpr float kDestructiveAlpha = 0.40f;

} // namespace

/**
 * @brief Creates an empty card with 12 dip padding.
 *
 * Children are clipped to the card's bounds so a card that is still growing
 * (or already shrinking) never shows its content spilling past the bottom.
 */
Card::Card() {
    heightScale_.setOwner(this);
    hover_.setOwner(this);
    stack().padding = Insets::all(kPadding);
    stack().clipsChildren = true;
}

/**
 * @brief Toggles the accent selection outline (repaint only).
 */
void Card::setSelected(bool on) {
    if (selected_ == on) {
        return;
    }
    selected_ = on;
    invalidate();
}

/**
 * @brief Natural stack size with the height multiplied by the insert/remove spring.
 *
 * The stack is measured unscaled so children keep their real layout; only the
 * card's reported height shrinks, and the clip in paint() hides the overflow.
 */
Size Card::measure(const Constraints& c) {
    Size natural = Widget::measure(c);

    // A spring can overshoot a hair past its endpoints; the scale never may.
    const float scale = std::clamp(heightScale_.value(), 0.0f, 1.0f);
    if (scale < 1.0f) {
        // Whole dips keep neighbouring cards from jittering on sub-pixel heights.
        natural.h = std::round(std::max(0.0f, natural.h) * scale);
    }
    return c.constrain(natural);
}

/**
 * @brief Paints the elevated fill and the state-dependent outline.
 */
void Card::paintSelf(Canvas& c) {
    const Rect raw = bounds();
    if (raw.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();

    // Snap the body so the rounded edges land on whole pixels at every DPI.
    const Rect b = c.scale().snap(raw);
    c.fillRoundedRect(b, radius_, t.elevated);

    // Outline: hairline separator that strengthens with the hover spring;
    // failed cards get a red ring and selection wins over everything.
    const float hover = std::clamp(hover_.value(), 0.0f, 1.0f);
    Color outline = Color::lerp(t.separator, t.separatorStrong, hover);
    float outlineWidth = 0.0f;
    if (destructive_) {
        outline = t.destructive.withAlpha(kDestructiveAlpha);
        outlineWidth = kStateOutlineWidth;
    }
    if (selected_) {
        outline = t.accent.withAlpha(kSelectedAlpha);
        outlineWidth = kStateOutlineWidth;
    }
    c.strokeRoundedRect(b, radius_, outline, outlineWidth);

    // While the height spring runs the parent must re-measure us every frame.
    // The spring only requests repaints, so the layout pass is asked for here;
    // the request lands before the next frame's layout, which then samples the
    // fresh value.
    if (heightScale_.animating()) {
        invalidateLayout();
    }
}

/**
 * @brief Press: remember the pointer is inside so release can complete a click.
 *
 * Both mouse buttons are accepted: left for onClick, right for onContextMenu.
 * Returning true captures the pointer so the release reaches us even when the
 * cursor drifts off the card.
 */
bool Card::onMouseDown(const MouseEvent& e) {
    if (removing_ || !enabled()) {
        return false;
    }
    if (e.button != MouseButton::Left && e.button != MouseButton::Right) {
        return false;
    }
    setPressInside(true);
    invalidate();
    return true;
}

/**
 * @brief Release inside fires onClick (left) or onContextMenu (right).
 *
 * The handlers may remove and destroy the card, so they are the last thing
 * touched here.
 */
bool Card::onMouseUp(const MouseEvent& e) {
    const bool wasPressed = pressed();
    const bool inside = bounds().contains(e.pos);
    setPressed(false);
    setPressInside(false);
    invalidate();

    if (removing_ || !enabled() || !wasPressed || !inside) {
        return true;
    }

    // Copy first: a handler may reassign the callback while it runs.
    if (e.button == MouseButton::Left) {
        std::function<void(const MouseEvent&)> fn = onClick;
        if (fn) {
            fn(e);
        }
    } else if (e.button == MouseButton::Right) {
        std::function<void(Point)> fn = onContextMenu;
        if (fn) {
            fn(e.rootPos);
        }
    }
    return true;
}

/**
 * @brief Hover in: spring the outline towards separatorStrong.
 */
void Card::onMouseEnter() {
    setHovered(true);
    if (!hoverable_ || removing_) {
        return;
    }
    hover_.setOwner(this);
    hover_.animateTo(1.0f, springs::interactive);
}

/**
 * @brief Hover out: spring the outline back to the hairline separator.
 */
void Card::onMouseLeave() {
    setHovered(false);
    hover_.setOwner(this);
    hover_.animateTo(0.0f, springs::interactive);
}

/**
 * @brief Grows from zero height and fades in (call right after adding to the tree).
 *
 * Frame ticks stay on for the duration so the window keeps rendering even if
 * nothing else is animating; the completion turns them off again and runs one
 * last layout so the final height is exact.
 */
void Card::animateIn() {
    removing_ = false;
    heightScale_.setOwner(this);
    opacity_.setOwner(this);

    // Start collapsed and invisible, then let both springs open the card.
    heightScale_.set(0.0f);
    opacity_.set(0.0f);
    setWantsFrameTicks(true);
    invalidateLayout();

    opacity_.animateTo(1.0f, springs::gentle);
    heightScale_.animateTo(1.0f, springs::gentle, [this] {
        setWantsFrameTicks(false);
        invalidateLayout();
    });
}

/**
 * @brief Shrinks and fades out, then runs @p done (which usually removes the card).
 *
 * The card is disabled for the duration so a half-collapsed card cannot be
 * clicked. @p done may destroy the card, so nothing is touched after it runs;
 * when the card is detached (no timeline) the springs complete synchronously
 * and @p done runs before this function returns.
 */
void Card::animateOut(std::function<void()> done) {
    if (removing_) {
        // A second removal request keeps the first animation and its callback.
        HH_LOG_DEBUG(kLog, L"animateOut called while already removing; ignored");
        return;
    }
    removing_ = true;
    setEnabled(false);
    heightScale_.setOwner(this);
    opacity_.setOwner(this);
    setWantsFrameTicks(true);

    opacity_.animateTo(0.0f, springs::gentle);
    heightScale_.animateTo(0.0f, springs::gentle, [this, finished = std::move(done)]() mutable {
        setWantsFrameTicks(false);
        invalidateLayout();
        // Last statement on purpose: the callback usually deletes this card.
        std::function<void()> fn = std::move(finished);
        if (fn) {
            fn();
        }
    });
}

} // namespace hh::ui
