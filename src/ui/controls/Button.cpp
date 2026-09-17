// ---------------------------------------------------------------------------
// Button.cpp - push buttons and icon buttons.
//
// Hover is a fill change only (scaling text under a spring shimmers glyphs);
// the press feedback scales the background rect to 0.97 while the label
// stays put. The fill colour is a spring so hover in/out cross-fades.
// ---------------------------------------------------------------------------
#include "ui/controls/Button.h"

#include "core/Logger.h"
#include "ui/core/FocusManager.h"
#include "ui/core/RootView.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

// ---- metrics ---------------------------------------------------------------
constexpr float kHeight = 28.0f;
constexpr float kHeightCompact = 24.0f;
constexpr float kRadius = 6.0f;
constexpr float kPadX = 12.0f;
constexpr float kIcon = 16.0f;
constexpr float kIconGap = 6.0f;
constexpr float kIconStroke = 1.5f;
constexpr float kPressScale = 0.97f;
constexpr float kDisabledOpacity = 0.4f;
/// Win11 close-button hover red (kept because users rely on it).
constexpr uint32_t kCloseHoverHex = 0xC42B1C;

/**
 * @brief Palette used while the button has no root (measured or painted detached).
 */
const Theme& fallbackTheme() {
    static const Theme kDark = Theme::dark();
    return kDark;
}

/**
 * @brief Root-aware theme lookup for a widget.
 */
const Theme& themeFor(const Widget& w) {
    const Theme* t = w.theme();
    return t ? *t : fallbackTheme();
}

/**
 * @brief True when the focus ring should be painted: focused after keyboard navigation.
 */
bool showsFocusRing(const Widget& w) {
    RootView* r = w.root();
    return r != nullptr && w.focused() && r->focus().keyboardMode();
}

/**
 * @brief Label style: semibold 14, or 13 for compact buttons.
 */
TextStyle labelStyle(bool compact) {
    return compact ? typography::calloutStrong() : typography::bodyStrong();
}

/**
 * @brief Background colour for a button kind and interaction state.
 * @param hover pointer over the button (and enabled)
 * @param down  pressed with the pointer still inside
 */
Color fillFor(const Theme& t, ButtonKind kind, bool hover, bool down) {
    switch (kind) {
    case ButtonKind::Primary:
        // Accent, lightened a touch on hover and darkened while pressed.
        if (down) {
            return Color::lerp(t.accent, Color::black(), 0.08f);
        }
        if (hover) {
            return Color::lerp(t.accent, Color::white(), 0.08f);
        }
        return t.accent;
    case ButtonKind::Secondary:
        if (down) {
            return t.fillQuaternary;
        }
        return hover ? t.fillTertiary : t.fillSecondary;
    case ButtonKind::Destructive:
        if (down) {
            return t.destructive.withAlpha(0.28f);
        }
        return t.destructive.withAlpha(hover ? 0.22f : 0.15f);
    case ButtonKind::Plain:
        if (down) {
            return t.fillTertiary;
        }
        return hover ? t.fillQuaternary : Color::transparent();
    }
    return t.fillSecondary;
}

/**
 * @brief Label/icon colour for a button kind.
 */
Color textFor(const Theme& t, ButtonKind kind) {
    switch (kind) {
    case ButtonKind::Primary:     return t.accentText;
    case ButtonKind::Secondary:   return t.labelPrimary;
    case ButtonKind::Destructive: return t.destructive;
    case ButtonKind::Plain:       return t.accent;
    }
    return t.labelPrimary;
}

/**
 * @brief Scales a rect about its centre (press feedback for backgrounds).
 */
Rect scaledAbout(const Rect& r, float s) {
    const float nw = r.w * s;
    const float nh = r.h * s;
    return {r.x + (r.w - nw) * 0.5f, r.y + (r.h - nh) * 0.5f, nw, nh};
}

} // namespace

// ===========================================================================
// Button
// ===========================================================================

/**
 * @brief Creates a labelled button.
 */
Button::Button(std::wstring text, ButtonKind kind, std::function<void()> onClickFn)
    : onClick(std::move(onClickFn)), text_(std::move(text)), kind_(kind) {
    fill_.setOwner(this);
    pressScale_.setOwner(this);
    updateFill(false);
}

/**
 * @brief Replaces the label text.
 */
void Button::setText(std::wstring text) {
    if (text_ == text) {
        return;
    }
    text_ = std::move(text);
    invalidateLayout();
}

/**
 * @brief Changes the visual kind; the fill snaps to the new palette entry.
 */
void Button::setKind(ButtonKind kind) {
    if (kind_ == kind) {
        return;
    }
    kind_ = kind;
    updateFill(false);
    invalidate();
}

/**
 * @brief Adds (or removes with IconId::None) a 16 dip leading icon.
 */
void Button::setIcon(IconId icon) {
    if (icon_ == icon) {
        return;
    }
    icon_ = icon;
    invalidateLayout();
}

/**
 * @brief Switches between the 28 dip and 24 dip heights.
 */
void Button::setCompact(bool compact) {
    if (compact_ == compact) {
        return;
    }
    compact_ = compact;
    invalidateLayout();
}

/**
 * @brief Natural size: label + 24 dip padding (+ icon), fixed height.
 */
Size Button::measure(const Constraints& c) {
    const float h = compact_ ? kHeightCompact : kHeight;

    float w = kPadX * 2.0f;
    if (!text_.empty()) {
        const Size text = measureTextShared(text_, labelStyle(compact_));
        w += std::ceil(std::max(0.0f, text.w));
    }
    if (icon_ != IconId::None) {
        w += kIcon + (text_.empty() ? 0.0f : kIconGap);
    }
    // Never narrower than it is tall (an icon-only button becomes a square).
    w = std::max(w, h);

    // Stretch buttons take the whole row when the parent gives a bound.
    if (stretch_ && c.hasBoundedWidth()) {
        w = std::max(w, c.maxW);
    }
    return c.constrain({w, h});
}

/**
 * @brief Paints background (scaled while pressed), outline, icon, label and focus ring.
 */
void Button::paintSelf(Canvas& c) {
    const Rect b = bounds();
    if (b.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    fill_.setOwner(this);
    pressScale_.setOwner(this);

    // Keep the fill in sync with state changes that bypass updateFill()
    // (initial theme, setEnabled, a theme swap while detached). Only when
    // no spring is running, so hover cross-fades are never interrupted.
    const bool hover = hovered() && enabled();
    const bool down = pressed() && pressInside() && enabled();
    const Color wanted = fillFor(t, kind_, hover, down);
    if (!fill_.animating() && fill_.target() != wanted) {
        fill_.set(wanted);
    }

    const bool disabled = !enabled();
    if (disabled) {
        c.pushOpacity(kDisabledOpacity);
    }

    // Background: scaled about the centre by the press spring, pixel-snapped.
    const float scale = std::clamp(pressScale_.value(), 0.5f, 1.0f);
    const Rect bg = c.scale().snap(scaledAbout(b, scale));
    const Color fill = fill_.value();
    if (fill.a > 0.001f) {
        c.fillRoundedRect(bg, kRadius, fill);
    }
    // Secondary buttons carry a hairline outline for definition on Mica.
    if (kind_ == ButtonKind::Secondary) {
        c.strokeRoundedRect(bg, kRadius, t.separator, 0.0f);
    }

    // Content: icon + label centred as a group inside the padded area.
    const Color textColor = textFor(t, kind_);
    const TextStyle style = labelStyle(compact_);
    const Rect inner = b.inset(Insets::symmetric(kPadX, 0.0f));
    if (inner.w > 0.0f && inner.h > 0.0f) {
        const bool hasIcon = icon_ != IconId::None;
        const bool hasText = !text_.empty();
        if (hasIcon) {
            // Measure the label so the icon/label pair can be centred together.
            float textW = 0.0f;
            if (hasText) {
                textW = std::ceil(std::max(0.0f, c.measureText(text_, style).w));
            }
            const float groupW = kIcon + (hasText ? kIconGap + textW : 0.0f);
            const float startX = inner.x + std::max(0.0f, (inner.w - groupW) * 0.5f);
            const Rect iconRect{startX, inner.y + (inner.h - kIcon) * 0.5f, kIcon, kIcon};
            c.drawIcon(icon_, iconRect, textColor, kIconStroke);
            if (hasText) {
                const float textX = startX + kIcon + kIconGap;
                const Rect textRect{textX, inner.y, std::max(0.0f, inner.right() - textX), inner.h};
                if (!textRect.isEmpty()) {
                    c.drawText(text_, style, textRect, textColor, HAlign::Left, VAlign::Center, Trimming::End, 1);
                }
            }
        } else if (hasText) {
            c.drawText(text_, style, inner, textColor, HAlign::Center, VAlign::Center, Trimming::End, 1);
        }
    }

    // Focus ring only after keyboard navigation (macOS-style).
    if (showsFocusRing(*this)) {
        c.drawFocusRing(b, kRadius, t.focusRing);
    }

    if (disabled) {
        c.pop();
    }
}

/**
 * @brief Press: capture the pointer, shrink the background, darken the fill.
 */
bool Button::onMouseDown(const MouseEvent& e) {
    if (!enabled() || e.button != MouseButton::Left) {
        return false;
    }
    pressScale_.setOwner(this);
    setPressInside(true);
    pressScale_.animateTo(kPressScale, springs::interactive);
    updateFill(true);
    return true;
}

/**
 * @brief Release: restore the background and fire the click when released inside.
 */
bool Button::onMouseUp(const MouseEvent& e) {
    pressScale_.setOwner(this);
    const bool wasPressed = pressed();
    const bool inside = bounds().contains(e.pos);

    // Clear the press state ourselves so the fill target is computed for the
    // released state (the root clears it again afterwards, harmlessly).
    setPressed(false);
    setPressInside(false);
    pressScale_.animateTo(1.0f, springs::interactive);
    updateFill(true);

    // click() may run a handler that destroys this button: it must be the
    // last thing touched here.
    if (wasPressed && inside && enabled() && e.button == MouseButton::Left) {
        click();
    }
    return true;
}

/**
 * @brief Tracks whether a press is still inside the bounds (captured moves).
 */
bool Button::onMouseMove(const MouseEvent& e) {
    if (!pressed()) {
        return false;
    }
    const bool inside = bounds().contains(e.pos);
    if (inside != pressInside()) {
        setPressInside(inside);
        pressScale_.setOwner(this);
        pressScale_.animateTo(inside ? kPressScale : 1.0f, springs::interactive);
        updateFill(true);
    }
    return true;
}

/**
 * @brief Hover in: cross-fade the fill.
 */
void Button::onMouseEnter() {
    setHovered(true);
    updateFill(true);
}

/**
 * @brief Hover out: cross-fade the fill back.
 */
void Button::onMouseLeave() {
    setHovered(false);
    updateFill(true);
}

/**
 * @brief Enter/Space activate the button with a quick press pop.
 */
bool Button::onKeyDown(const KeyEvent& e) {
    if (!enabled()) {
        return false;
    }
    if (e.vk != VK_SPACE && e.vk != VK_RETURN) {
        return false;
    }
    // Held keys auto-repeat; only the first press counts.
    if (e.repeat) {
        return true;
    }
    pressScale_.setOwner(this);
    pressScale_.set(kPressScale);
    pressScale_.animateTo(1.0f, springs::interactive);
    click();
    return true;
}

/**
 * @brief Re-resolves the fill for the new palette (instant, no cross-fade).
 */
void Button::onThemeChanged() {
    updateFill(false);
}

/**
 * @brief Springs (or snaps) the fill to the colour for the current state.
 */
void Button::updateFill(bool animated) {
    fill_.setOwner(this);
    const Theme& t = themeFor(*this);
    const bool hover = hovered() && enabled();
    const bool down = pressed() && pressInside() && enabled();
    const Color target = fillFor(t, kind_, hover, down);
    if (animated) {
        // Animatable falls back to set() when there is no timeline yet.
        fill_.animateTo(target, springs::interactive);
    } else {
        fill_.set(target);
    }
}

/**
 * @brief Fires onClick. The handler may delete this widget, so nothing is
 *        touched after the call.
 */
void Button::click() {
    if (!enabled()) {
        return;
    }
    // Copy first: the handler may reassign or clear onClick while running.
    std::function<void()> fn = onClick;
    if (fn) {
        fn();
    }
}

// ===========================================================================
// IconButton
// ===========================================================================

/**
 * @brief Creates a square 28 dip icon button.
 */
IconButton::IconButton(IconId icon, std::function<void()> onClickFn)
    : onClick(std::move(onClickFn)), icon_(icon) {
    hoverAmount_.setOwner(this);
}

/**
 * @brief Replaces the glyph.
 */
void IconButton::setIcon(IconId icon) {
    if (icon_ == icon) {
        return;
    }
    icon_ = icon;
    invalidate();
}

/**
 * @brief Sets the button size in dips (window buttons use 46x32).
 */
void IconButton::setSize(float w, float h) {
    w_ = std::max(0.0f, w);
    h_ = std::max(0.0f, h);
    invalidateLayout();
}

/**
 * @brief Fixed size.
 */
Size IconButton::measure(const Constraints& c) {
    return c.constrain({w_, h_});
}

/**
 * @brief Paints the hover fill (spring-blended) and the glyph.
 */
void IconButton::paintSelf(Canvas& c) {
    const Rect b = bounds();
    if (b.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    hoverAmount_.setOwner(this);

    const bool disabled = !enabled();
    if (disabled) {
        c.pushOpacity(kDisabledOpacity);
    }

    const float hover = std::clamp(hoverAmount_.value(), 0.0f, 1.0f);
    const bool down = pressed() && pressInside() && enabled();

    // Fill: neutral label tint, or the Win11 close red for destructive hover.
    // Pressing strengthens the tint a little (no scaling on icon buttons).
    Color fill;
    Color glyph;
    if (destructiveHover_) {
        const Color red = Color::fromHex(kCloseHoverHex);
        fill = red.withAlpha(hover * (down ? 0.85f : 1.0f));
        const Color base = hasCustom_ ? custom_ : t.labelSecondary;
        glyph = Color::lerp(base, Color::white(), hover);
    } else {
        fill = t.labelPrimary.withAlpha(hover * (down ? 0.12f : 0.08f));
        const Color base = hasCustom_ ? custom_ : t.labelSecondary;
        const Color hot = hasCustom_ ? custom_ : t.labelPrimary;
        glyph = Color::lerp(base, hot, hover);
    }
    if (fill.a > 0.001f) {
        c.fillRoundedRect(c.scale().snap(b), kRadius, fill);
    }

    // Glyph box: caption buttons (larger than 28) follow the Win11 10 dip
    // convention; in-content buttons use a 12 dip box (10 when compact).
    const float smaller = std::min(b.w, b.h);
    // Caption buttons (46x32) use the Windows 11 10 dip glyph; ordinary icon
    // buttons scale with their size between 12 and 16 dip.
    const float glyphSize = (smaller > 28.0f) ? 12.0f : std::clamp(smaller * 0.55f, 12.0f, 16.0f);
    if (icon_ != IconId::None && glyphSize > 0.0f) {
        const Rect box{b.x + (b.w - glyphSize) * 0.5f, b.y + (b.h - glyphSize) * 0.5f, glyphSize, glyphSize};
        c.drawIcon(icon_, box, glyph, std::max(0.5f, stroke_));
    }

    if (showsFocusRing(*this)) {
        c.drawFocusRing(b, kRadius, t.focusRing);
    }

    if (disabled) {
        c.pop();
    }
}

/**
 * @brief Press: capture; the darker tint comes from pressed()/pressInside().
 */
bool IconButton::onMouseDown(const MouseEvent& e) {
    if (!enabled() || e.button != MouseButton::Left) {
        return false;
    }
    setPressInside(true);
    invalidate();
    return true;
}

/**
 * @brief Release inside fires the click (last, since it may delete us).
 */
bool IconButton::onMouseUp(const MouseEvent& e) {
    const bool wasPressed = pressed();
    const bool inside = bounds().contains(e.pos);
    setPressed(false);
    setPressInside(false);
    invalidate();
    if (wasPressed && inside && enabled() && e.button == MouseButton::Left) {
        std::function<void()> fn = onClick;
        if (fn) {
            fn();
        }
    }
    return true;
}

/**
 * @brief Tracks the pointer during a captured press.
 */
bool IconButton::onMouseMove(const MouseEvent& e) {
    if (!pressed()) {
        return false;
    }
    const bool inside = bounds().contains(e.pos);
    if (inside != pressInside()) {
        setPressInside(inside);
        invalidate();
    }
    return true;
}

/**
 * @brief Hover in: spring the fill up.
 */
void IconButton::onMouseEnter() {
    setHovered(true);
    hoverAmount_.setOwner(this);
    hoverAmount_.animateTo(enabled() ? 1.0f : 0.0f, springs::interactive);
}

/**
 * @brief Hover out: spring the fill away.
 */
void IconButton::onMouseLeave() {
    setHovered(false);
    hoverAmount_.setOwner(this);
    hoverAmount_.animateTo(0.0f, springs::interactive);
}

/**
 * @brief Enter/Space activate the button.
 */
bool IconButton::onKeyDown(const KeyEvent& e) {
    if (!enabled()) {
        return false;
    }
    if (e.vk != VK_SPACE && e.vk != VK_RETURN) {
        return false;
    }
    if (e.repeat) {
        return true;
    }
    std::function<void()> fn = onClick;
    if (fn) {
        fn();
    }
    return true;
}

} // namespace hh::ui
