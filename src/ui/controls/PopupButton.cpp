// ---------------------------------------------------------------------------
// PopupButton.cpp - macOS pop-up button: an optional caption, the current
// value and an accent chevron badge. Opens a PopupMenu in the popup window.
// ---------------------------------------------------------------------------
#include "ui/controls/PopupButton.h"

#include "core/Logger.h"
#include "ui/core/OverlayHost.h"
#include "ui/core/RootView.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <memory>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"PopupButton";

// Control metrics (dips).
constexpr float kHeight = 28.0f;
constexpr float kHeightCompact = 24.0f;
constexpr float kRadius = 6.0f;
constexpr float kLeftPad = 10.0f;
constexpr float kCaptionGap = 6.0f;
constexpr float kLabelGap = 8.0f;
constexpr float kBadgeSize = 16.0f;
constexpr float kBadgeRadius = 4.0f;
constexpr float kBadgeRightPad = 6.0f;
constexpr float kChevronW = 11.0f;
constexpr float kChevronH = 11.0f;

// Width added to caption + widest label so the value never crowds the badge.
constexpr float kWidthExtra = 60.0f;

// Press feedback scales only the background, never the text.
constexpr float kPressScale = 0.97f;

// Disabled controls fade to 40 %.
constexpr float kDisabledOpacity = 0.40f;

/**
 * @brief Picks the theme to paint with: the root's when attached, else the canvas's.
 */
const Theme& paintTheme(const Widget& w, const Canvas& c) {
    const Theme* t = w.theme();
    return t ? *t : c.theme();
}

} // namespace

/**
 * @brief Builds a pop-up with items and an initial selection.
 */
PopupButton::PopupButton(std::vector<PopupItem> items, int selected)
    : items_(std::move(items)) {
    hover_.setOwner(this);
    setSelectedIndex(selected);
}

/**
 * @brief Replaces the items; an out-of-range selection is cleared.
 */
void PopupButton::setItems(std::vector<PopupItem> items) {
    items_ = std::move(items);
    if (selected_ >= static_cast<int>(items_.size())) {
        selected_ = -1;
    }
    invalidateLayout();
}

/**
 * @brief Selects by index (-1 or out of range shows the placeholder).
 */
void PopupButton::setSelectedIndex(int index) {
    const int count = static_cast<int>(items_.size());
    selected_ = (index >= 0 && index < count) ? index : -1;
    invalidate();
}

/**
 * @brief Selects the item whose value matches; false when no item has it.
 */
bool PopupButton::setSelectedValue(std::wstring_view value) {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].value == value) {
            setSelectedIndex(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

/**
 * @brief Value of the selected item (empty when nothing is selected).
 */
std::wstring PopupButton::selectedValue() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(items_.size())) {
        return {};
    }
    return items_[static_cast<size_t>(selected_)].value;
}

/**
 * @brief Label of the selected item (empty when nothing is selected).
 */
std::wstring PopupButton::selectedLabel() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(items_.size())) {
        return {};
    }
    return items_[static_cast<size_t>(selected_)].label;
}

// ---- popup ------------------------------------------------------------------------

/**
 * @brief Opens the menu below the control in the popup window.
 */
void PopupButton::open() {
    if (open_ || !enabled()) {
        return;
    }
    RootView* rv = root();
    if (!rv) {
        HH_LOG_WARN(kLog, L"open() called while detached; ignoring");
        return;
    }
    if (items_.empty()) {
        HH_LOG_DEBUG(kLog, L"open() with no items; ignoring");
        return;
    }

    // The menu mirrors our items and checks the current selection.
    auto menu = std::make_unique<PopupMenu>(items_, selected_);
    menu->setMinWidth(frame().w);
    menu->onSelect = [this](int index) {
        setSelectedIndex(index);
        if (onChanged) {
            onChanged(selected_);
        }
        close();
    };
    menu->onCancel = [this] { close(); };

    // Mark open before showing so a synchronous dismiss can reset it.
    open_ = true;
    invalidate();
    rv->overlay().showPopup(std::move(menu), frameInRoot(), PopupPlacement::Below, [this] {
        open_ = false;
        invalidate();
    });
}

/**
 * @brief Dismisses the menu (the dismiss callback clears the open state).
 */
void PopupButton::close() {
    if (!open_) {
        return;
    }
    if (RootView* rv = root()) {
        rv->overlay().dismissPopup();
    }

    // Belt and braces: never leave the control stuck "open" if no callback ran.
    open_ = false;
    invalidate();
}

// ---- layout / paint --------------------------------------------------------------------

/**
 * @brief Preferred size: caption + widest label + chrome, fixed height.
 */
Size PopupButton::measure(const Constraints& c) {
    const TextStyle style = typography::callout();

    // Size for the widest possible value so selection changes never reflow.
    float widest = measureTextShared(placeholder_, style).w;
    for (const PopupItem& it : items_) {
        widest = std::max(widest, measureTextShared(it.label, style).w);
    }
    float captionW = 0.0f;
    if (!caption_.empty()) {
        captionW = measureTextShared(caption_, style).w + kCaptionGap;
    }

    const float w = std::max(0.0f, captionW + widest + kWidthExtra);
    const float h = compact_ ? kHeightCompact : kHeight;
    return c.constrain({w, h});
}

/**
 * @brief Paints the pill, caption, value and the accent chevron badge.
 */
void PopupButton::paintSelf(Canvas& c) {
    const Theme& t = paintTheme(*this, c);
    const Rect b = bounds();
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return;
    }

    // Disabled: whole control at 40 %.
    const bool dimmed = !enabled();
    if (dimmed) {
        c.pushOpacity(kDisabledOpacity);
    }

    // Background: fillSecondary with fillTertiary layered in on hover/open.
    const bool pressing = pressed() && pressInside();
    if (pressing) {
        c.pushTransform(D2D1::Matrix3x2F::Scale(kPressScale, kPressScale, b.center().toD2D()));
    }
    const float hairline = c.scale().hairline();
    c.fillRoundedRect(b, kRadius, t.fillSecondary);
    const float hover = open_ ? 1.0f : std::clamp(hover_.value(), 0.0f, 1.0f);
    if (hover > 0.001f) {
        c.fillRoundedRect(b, kRadius, t.fillTertiary.scaledAlpha(hover));
    }
    c.strokeRoundedRect(b.inset(hairline * 0.5f), kRadius, t.separator, 0.0f);
    if (pressing) {
        c.pop();
    }

    // Chevron badge on the right: accent rounded square with a white chevron.
    const Rect badge{b.w - kBadgeRightPad - kBadgeSize, (b.h - kBadgeSize) * 0.5f, kBadgeSize, kBadgeSize};
    c.fillRoundedRect(c.scale().snap(badge), kBadgeRadius, t.accent);
    const Rect chevron{badge.x + (kBadgeSize - kChevronW) * 0.5f, badge.y + (kBadgeSize - kChevronH) * 0.5f, kChevronW, kChevronH};
    c.drawIcon(IconId::ChevronDown, chevron, t.accentText, 1.5f);

    // Caption, then the value, trimmed before the badge.
    const TextStyle style = typography::callout();
    float x = kLeftPad;
    const float textRight = badge.x - kLabelGap;
    if (!caption_.empty()) {
        const float capW = std::min(c.measureText(caption_, style).w, std::max(0.0f, textRight - x));
        c.drawText(caption_, style, {x, 0.0f, capW, b.h}, t.labelSecondary, HAlign::Left, VAlign::Center, Trimming::End, 1);
        x += capW + kCaptionGap;
    }
    const float labelW = std::max(0.0f, textRight - x);
    if (labelW > 0.0f) {
        const bool hasValue = selected_ >= 0 && selected_ < static_cast<int>(items_.size());
        const std::wstring& text = hasValue ? items_[static_cast<size_t>(selected_)].label : placeholder_;
        const Color colour = hasValue ? t.labelPrimary : t.labelTertiary;
        c.drawText(text, style, {x, 0.0f, labelW, b.h}, colour, HAlign::Left, VAlign::Center, Trimming::End, 1);
    }

    // Focus ring only after keyboard navigation (macOS behaviour).
    RootView* rv = root();
    if (rv && focused() && rv->focus().keyboardMode()) {
        c.drawFocusRing(b, kRadius, t.focusRing);
    }

    if (dimmed) {
        c.pop();
    }
}

// ---- input --------------------------------------------------------------------------

/**
 * @brief Press opens the menu immediately; no capture is needed.
 */
bool PopupButton::onMouseDown(const MouseEvent& e) {
    if (!enabled() || e.button != MouseButton::Left) {
        return false;
    }
    open();
    return false;
}

/**
 * @brief Nothing happens on release: the press already opened the menu.
 */
bool PopupButton::onMouseUp(const MouseEvent&) {
    return false;
}

/**
 * @brief Hover springs the fill towards the tertiary tint.
 */
void PopupButton::onMouseEnter() {
    hover_.setOwner(this);
    hover_.animateTo(1.0f, springs::snappy);
}

/**
 * @brief Leaving springs the fill back.
 */
void PopupButton::onMouseLeave() {
    hover_.setOwner(this);
    hover_.animateTo(0.0f, springs::snappy);
}

/**
 * @brief Space / Enter / Down / Alt+Down open the menu.
 */
bool PopupButton::onKeyDown(const KeyEvent& e) {
    if (!enabled()) {
        return false;
    }
    switch (e.vk) {
    case VK_SPACE:
    case VK_RETURN:
    case VK_DOWN:
        open();
        return true;
    default:
        return false;
    }
}

} // namespace hh::ui
