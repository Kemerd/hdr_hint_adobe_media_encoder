// ---------------------------------------------------------------------------
// ToggleSwitch.cpp - macOS System Settings style switch.
//
// 38x22 track, 18 dip white knob that stretches to 22 while pressed and can
// be dragged across the track. The knob position and the track colour are
// springs; a drag drives the knob directly and the springs take over on
// release.
// ---------------------------------------------------------------------------
#include "ui/controls/ToggleSwitch.h"

#include "ui/core/FocusManager.h"
#include "ui/core/RootView.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

// ---- metrics ---------------------------------------------------------------
constexpr float kTrackW = 38.0f;
constexpr float kTrackH = 22.0f;
constexpr float kTrackRadius = 11.0f;
constexpr float kPad = 2.0f;
constexpr float kKnob = 18.0f;
constexpr float kKnobStretched = 22.0f;
constexpr float kDisabledOpacity = 0.4f;
/// Pointer travel before a press turns into a drag.
constexpr float kDragThreshold = 6.0f;

/**
 * @brief Dark palette used while the switch has no root.
 */
const Theme& fallbackTheme() {
    static const Theme kDark = Theme::dark();
    return kDark;
}

/**
 * @brief Root-aware theme lookup.
 */
const Theme& themeFor(const Widget& w) {
    const Theme* t = w.theme();
    return t ? *t : fallbackTheme();
}

/**
 * @brief Track colour for a state.
 */
Color trackColorFor(const Theme& t, bool on, bool green) {
    if (!on) {
        return t.fillTertiary;
    }
    return green ? t.success : t.accent;
}

/**
 * @brief The 38x22 track centred inside the widget's frame.
 */
Rect trackRectIn(const Rect& b) {
    return {b.x + (b.w - kTrackW) * 0.5f, b.y + (b.h - kTrackH) * 0.5f, kTrackW, kTrackH};
}

/**
 * @brief Current knob width from the stretch amount (0..1).
 */
float knobWidthFor(float stretch) {
    return kKnob + (kKnobStretched - kKnob) * std::clamp(stretch, 0.0f, 1.0f);
}

/**
 * @brief Horizontal travel available to the knob's left edge for a knob width.
 *
 * The right edge of the fully-on knob always sits 2 dip from the track's
 * right edge, so a wider (stretched) knob has less travel.
 */
float travelFor(float knobW) {
    return std::max(0.0f, kTrackW - kPad * 2.0f - knobW);
}

/**
 * @brief True when the focus ring should be painted.
 */
bool showsFocusRing(const Widget& w) {
    RootView* r = w.root();
    return r != nullptr && w.focused() && r->focus().keyboardMode();
}

} // namespace

/**
 * @brief Off switch.
 */
ToggleSwitch::ToggleSwitch() {
    knobX_.setOwner(this);
    knobStretch_.setOwner(this);
    track_.setOwner(this);
    animateToState(false);
}

/**
 * @brief Switch with an initial state and change handler.
 */
ToggleSwitch::ToggleSwitch(bool on, std::function<void(bool)> onChangedFn)
    : onChanged(std::move(onChangedFn)), on_(on) {
    knobX_.setOwner(this);
    knobStretch_.setOwner(this);
    track_.setOwner(this);
    animateToState(false);
}

/**
 * @brief Programmatic change; never fires onChanged.
 */
void ToggleSwitch::setOn(bool on, bool animated) {
    if (on_ == on) {
        return;
    }
    on_ = on;
    animateToState(animated);
}

/**
 * @brief Fixed 38x22.
 */
Size ToggleSwitch::measure(const Constraints& c) {
    return c.constrain({kTrackW, kTrackH});
}

/**
 * @brief Paints track, knob shadow, knob and outline, plus the focus ring.
 */
void ToggleSwitch::paintSelf(Canvas& c) {
    const Rect b = bounds();
    if (b.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    knobX_.setOwner(this);
    knobStretch_.setOwner(this);
    track_.setOwner(this);

    // Resync the track colour when state changed without a spring (initial
    // paint, theme swap while detached). Never during a drag: the drag sets
    // the colour directly from the knob position.
    const bool dragBlend = dragging_ && dragMoved_;
    if (!dragBlend && !track_.animating()) {
        const Color wanted = trackColorFor(t, on_, green_);
        if (track_.target() != wanted) {
            track_.set(wanted);
        }
    }

    const bool disabled = !enabled();
    if (disabled) {
        c.pushOpacity(kDisabledOpacity);
    }

    // Track (pixel-snapped so the capsule edges are crisp at 125/150 %).
    const Rect track = c.scale().snap(trackRectIn(b));
    c.fillRoundedRect(track, kTrackRadius, track_.value());

    // A faint outline gives the off track definition on light backgrounds;
    // it fades out as the knob travels towards "on".
    const float pos = std::clamp(knobX_.value(), 0.0f, 1.0f);
    const float outlineAlpha = 1.0f - pos;
    if (outlineAlpha > 0.01f) {
        c.strokeRoundedRect(track, kTrackRadius, t.separator.scaledAlpha(outlineAlpha), 0.0f);
    }

    // Knob geometry: width follows the stretch spring, x follows the knob spring.
    const float knobW = knobWidthFor(knobStretch_.value());
    const float left = kPad + pos * travelFor(knobW);
    const Rect knob{track.x + left, track.y + kPad, knobW, kKnob};
    const float knobRadius = kKnob * 0.5f;

    // Two-ring shadow underneath the knob (no effects, just translucent rings).
    c.fillRoundedRect(knob.inset(-1.0f).offset(0.0f, 1.0f), knobRadius + 1.0f, Color::black(0.10f));
    c.fillRoundedRect(knob.inset(-2.0f).offset(0.0f, 2.0f), knobRadius + 2.0f, Color::black(0.05f));

    // Knob: white with a 1 dip black@8% outline drawn inside the edge.
    c.fillRoundedRect(knob, knobRadius, Color::white());
    c.strokeRoundedRect(knob.inset(0.5f), knobRadius - 0.5f, Color::black(0.08f), 1.0f);

    if (showsFocusRing(*this)) {
        c.drawFocusRing(track, kTrackRadius, t.focusRing);
    }

    if (disabled) {
        c.pop();
    }
}

/**
 * @brief Press: remember the start, stretch the knob and capture the pointer.
 */
bool ToggleSwitch::onMouseDown(const MouseEvent& e) {
    if (!enabled() || e.button != MouseButton::Left) {
        return false;
    }
    knobStretch_.setOwner(this);
    dragging_ = true;
    dragMoved_ = false;
    dragStartX_ = e.pos.x;
    setPressInside(true);
    knobStretch_.animateTo(1.0f, springs::interactive);
    return true;
}

/**
 * @brief Release: commit a drag by knob position, otherwise toggle on a click.
 */
bool ToggleSwitch::onMouseUp(const MouseEvent& e) {
    if (!dragging_) {
        return false;
    }
    knobStretch_.setOwner(this);
    const bool moved = dragMoved_;
    const bool inside = bounds().contains(e.pos);
    dragging_ = false;
    dragMoved_ = false;
    setPressed(false);
    setPressInside(false);
    knobStretch_.animateTo(0.0f, springs::interactive);

    if (e.button != MouseButton::Left || !enabled()) {
        animateToState(true);
        return true;
    }

    // commit() may fire onChanged, whose handler may delete this switch, so
    // it is the last call on every path.
    if (moved) {
        commit(knobX_.value() >= 0.5f);
    } else if (inside) {
        commit(!on_);
    } else {
        // Pressed and released outside without dragging: no change.
        animateToState(true);
    }
    return true;
}

/**
 * @brief Drag: past 6 dip of travel the knob follows the pointer directly.
 */
bool ToggleSwitch::onMouseMove(const MouseEvent& e) {
    if (!dragging_) {
        return false;
    }
    const float dx = e.pos.x - dragStartX_;
    if (!dragMoved_ && std::fabs(dx) > kDragThreshold) {
        dragMoved_ = true;
    }
    if (!dragMoved_) {
        return true;
    }

    // Relative mapping: the knob starts at its resting position for the
    // current state and moves with the pointer, so there is no jump on the
    // first drag event. Instant set(): a spring would lag the finger.
    const float knobW = knobWidthFor(knobStretch_.value());
    const float travel = travelFor(knobW);
    const float start = on_ ? 1.0f : 0.0f;
    const float pos = (travel > 0.0f) ? std::clamp(start + dx / travel, 0.0f, 1.0f) : start;
    knobX_.setOwner(this);
    knobX_.set(pos);

    // Blend the track between off and on colours as the knob travels.
    const Theme& t = themeFor(*this);
    track_.setOwner(this);
    track_.set(Color::lerp(trackColorFor(t, false, green_), trackColorFor(t, true, green_), pos));
    return true;
}

/**
 * @brief Hover out (a captured drag continues regardless).
 */
void ToggleSwitch::onMouseLeave() {
    setHovered(false);
}

/**
 * @brief Space/Enter toggle.
 */
bool ToggleSwitch::onKeyDown(const KeyEvent& e) {
    if (!enabled()) {
        return false;
    }
    if (e.vk != VK_SPACE && e.vk != VK_RETURN) {
        return false;
    }
    if (e.repeat) {
        return true;
    }
    commit(!on_);
    return true;
}

/**
 * @brief Recolour the track for the new palette (instant).
 */
void ToggleSwitch::onThemeChanged() {
    if (dragging_ && dragMoved_) {
        return;
    }
    animateToState(false);
}

/**
 * @brief Springs (or snaps) knob and track to the current on/off state.
 */
void ToggleSwitch::animateToState(bool animated) {
    knobX_.setOwner(this);
    track_.setOwner(this);
    const float knobTarget = on_ ? 1.0f : 0.0f;
    const Color trackTarget = trackColorFor(themeFor(*this), on_, green_);
    if (animated) {
        knobX_.animateTo(knobTarget, springs::snappy);
        track_.animateTo(trackTarget, springs::gentle);
    } else {
        knobX_.set(knobTarget);
        track_.set(trackTarget);
    }
}

/**
 * @brief User commit: applies the state, animates, and notifies when it changed.
 *        The handler may delete this widget, so it runs last.
 */
void ToggleSwitch::commit(bool on) {
    const bool changed = (on != on_);
    on_ = on;
    animateToState(true);
    if (!changed) {
        return;
    }
    std::function<void(bool)> fn = onChanged;
    if (fn) {
        fn(on);
    }
}

} // namespace hh::ui
