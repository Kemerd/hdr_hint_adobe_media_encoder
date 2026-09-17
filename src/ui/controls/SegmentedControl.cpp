// ---------------------------------------------------------------------------
// SegmentedControl.cpp - tabs with a sliding pill.
//
// Segments always share the track equally (fixed segment width only affects
// the measured size). The pill is an Animatable<Rect> that springs to the
// selected segment and is clamped into the track when painted, so a snappy
// spring can overshoot in value without ever leaving the track visually.
// ---------------------------------------------------------------------------
#include "ui/controls/SegmentedControl.h"

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
constexpr float kTrackRadius = 7.0f;
constexpr float kPillRadius = 6.0f;
constexpr float kPad = 2.0f;
/// Horizontal padding added to the widest label for content-sized segments.
constexpr float kLabelPad = 24.0f;
/// Inset of the label rect inside a segment (keeps text off the dividers).
constexpr float kTextInset = 4.0f;
/// Divider vertical inset from the track edges.
constexpr float kDividerInset = 8.0f;
constexpr float kDividerAlpha = 0.4f;
constexpr float kDisabledOpacity = 0.4f;
/// Pill colour in dark mode (light mode uses white).
constexpr uint32_t kPillDarkHex = 0x636366;

/**
 * @brief Label styles: 13 pt, semibold when selected.
 */
TextStyle selectedStyle() { return typography::calloutStrong(); }
TextStyle normalStyle() { return typography::callout(); }

/**
 * @brief True when the focus ring should be painted.
 */
bool showsFocusRing(const Widget& w) {
    RootView* r = w.root();
    return r != nullptr && w.focused() && r->focus().keyboardMode();
}

} // namespace

/**
 * @brief Creates the control with items and an initial selection.
 */
SegmentedControl::SegmentedControl(std::vector<std::wstring> items, int selected)
    : items_(std::move(items)) {
    pill_.setOwner(this);
    if (!items_.empty()) {
        const int last = static_cast<int>(items_.size()) - 1;
        selected_ = std::clamp(selected, 0, last);
    } else {
        selected_ = 0;
    }
}

/**
 * @brief Replaces the items; the selection is clamped and the pill snaps on the next layout.
 */
void SegmentedControl::setItems(std::vector<std::wstring> items) {
    items_ = std::move(items);
    hoverIndex_ = -1;
    pressIndex_ = -1;
    if (items_.empty()) {
        selected_ = 0;
    } else {
        const int last = static_cast<int>(items_.size()) - 1;
        selected_ = std::clamp(selected_, 0, last);
    }
    invalidateLayout();
}

/**
 * @brief Programmatic selection; never fires onChanged.
 */
void SegmentedControl::setSelected(int index, bool animated) {
    select(index, animated, false);
}

/**
 * @brief Natural size: equal segments of (widest label + 24) or the fixed width.
 */
Size SegmentedControl::measure(const Constraints& c) {
    const size_t n = items_.size();
    if (n == 0) {
        return c.constrain({kPad * 2.0f, kHeight});
    }

    float segW = segmentWidth_;
    if (segW <= 0.0f) {
        // Measure every label with the selected (semibold) style so the
        // control never resizes when the selection moves.
        float widest = 0.0f;
        const TextStyle style = selectedStyle();
        for (const std::wstring& item : items_) {
            if (item.empty()) {
                continue;
            }
            widest = std::max(widest, measureTextShared(item, style).w);
        }
        segW = std::ceil(std::max(0.0f, widest)) + kLabelPad;
    }

    const float w = segW * static_cast<float>(n) + kPad * 2.0f;
    return c.constrain({w, kHeight});
}

/**
 * @brief Snaps the pill onto the selected segment for the new frame.
 */
void SegmentedControl::onLayout() {
    pill_.setOwner(this);
    if (items_.empty()) {
        pill_.set(Rect{});
        return;
    }
    // Layout changes (resize, DPI) are not animated: a moving pill during a
    // window resize would look like a bug.
    pill_.set(segmentRect(selected_));
}

/**
 * @brief Paints track, dividers, pill (with shadow/outline) and labels.
 */
void SegmentedControl::paintSelf(Canvas& c) {
    const Rect raw = bounds();
    if (raw.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    pill_.setOwner(this);

    const bool disabled = !enabled();
    if (disabled) {
        c.pushOpacity(kDisabledOpacity);
    }

    // Track.
    const Rect b = c.scale().snap(raw);
    c.fillRoundedRect(b, kTrackRadius, t.fillQuaternary);

    const int n = static_cast<int>(items_.size());
    if (n == 0) {
        if (disabled) {
            c.pop();
        }
        return;
    }

    // Pill rect: the spring value clamped into the inner track so overshoot
    // never pokes past the edge (snappy damping overshoots by ~0.6 %).
    const Rect inner = raw.inset(kPad);
    Rect pill = pill_.value();
    if (pill.w <= 0.0f || pill.h <= 0.0f) {
        pill = segmentRect(selected_);
    }
    if (inner.w > 0.0f && inner.h > 0.0f) {
        pill.w = std::clamp(pill.w, 0.0f, inner.w);
        pill.x = std::clamp(pill.x, inner.x, inner.right() - pill.w);
        pill.y = inner.y;
        pill.h = inner.h;
    }
    pill = c.scale().snap(pill);

    // Dividers between neighbours that are not selected and not under the pill.
    const Color dividerColor = t.labelSecondary.scaledAlpha(kDividerAlpha);
    for (int i = 0; i + 1 < n; ++i) {
        if (i == selected_ || i + 1 == selected_) {
            continue;
        }
        const float x = segmentRect(i).right();
        // Hidden while the moving pill covers it (with a small margin).
        if (x > pill.x + 2.0f && x < pill.right() - 2.0f) {
            continue;
        }
        const float y0 = b.y + kDividerInset;
        const float y1 = b.bottom() - kDividerInset;
        if (y1 > y0) {
            c.drawHairline({x, y0}, {x, y1}, dividerColor);
        }
    }

    // Pill: soft two-ring shadow, fill, hairline outline.
    if (!pill.isEmpty()) {
        c.fillRoundedRect(pill.inset(-1.0f).offset(0.0f, 1.0f), kPillRadius + 1.0f, Color::black(0.06f));
        c.fillRoundedRect(pill.inset(-2.0f).offset(0.0f, 2.0f), kPillRadius + 2.0f, Color::black(0.03f));
        const Color pillFill = t.isDark ? Color::fromHex(kPillDarkHex) : Color::white();
        c.fillRoundedRect(pill, kPillRadius, pillFill);
        c.strokeRoundedRect(pill.inset(0.5f), kPillRadius - 0.5f, Color::black(0.04f), 1.0f);
    }

    // Labels.
    for (int i = 0; i < n; ++i) {
        const std::wstring& item = items_[static_cast<size_t>(i)];
        if (item.empty()) {
            continue;
        }
        const bool isSelected = (i == selected_);
        const bool isHot = (i == hoverIndex_) && enabled();
        const Color color = (isSelected || isHot) ? t.labelPrimary : t.labelSecondary;
        const TextStyle style = isSelected ? selectedStyle() : normalStyle();
        const Rect seg = segmentRect(i).inset(Insets::symmetric(kTextInset, 0.0f));
        if (seg.isEmpty()) {
            continue;
        }
        c.drawText(item, style, seg, color, HAlign::Center, VAlign::Center, Trimming::End, 1);
    }

    if (showsFocusRing(*this)) {
        c.drawFocusRing(b, kTrackRadius, t.focusRing);
    }

    if (disabled) {
        c.pop();
    }
}

/**
 * @brief Press: remember which segment went down and capture the pointer.
 */
bool SegmentedControl::onMouseDown(const MouseEvent& e) {
    if (!enabled() || items_.empty() || e.button != MouseButton::Left) {
        return false;
    }
    pressIndex_ = indexAt(e.pos);
    hoverIndex_ = pressIndex_;
    setPressInside(pressIndex_ >= 0);
    invalidate();
    return true;
}

/**
 * @brief Release over a segment selects it (the handler may delete us; it runs last).
 */
bool SegmentedControl::onMouseUp(const MouseEvent& e) {
    const bool wasPressed = pressed();
    const int idx = indexAt(e.pos);
    pressIndex_ = -1;
    setPressed(false);
    setPressInside(false);
    hoverIndex_ = bounds().contains(e.pos) ? idx : -1;
    invalidate();

    if (wasPressed && enabled() && e.button == MouseButton::Left && idx >= 0) {
        select(idx, true, true);
    }
    return true;
}

/**
 * @brief Hover highlight (also tracked while pressed and captured).
 */
bool SegmentedControl::onMouseMove(const MouseEvent& e) {
    const int idx = bounds().contains(e.pos) ? indexAt(e.pos) : -1;
    if (pressed()) {
        setPressInside(idx >= 0);
    }
    if (idx != hoverIndex_) {
        hoverIndex_ = idx;
        invalidate();
    }
    return true;
}

/**
 * @brief Clears the hover highlight.
 */
void SegmentedControl::onMouseLeave() {
    setHovered(false);
    if (hoverIndex_ != -1) {
        hoverIndex_ = -1;
        invalidate();
    }
}

/**
 * @brief Left/Right/Up/Down step, Home/End jump (no wrap-around).
 */
bool SegmentedControl::onKeyDown(const KeyEvent& e) {
    if (!enabled() || items_.empty()) {
        return false;
    }
    const int last = static_cast<int>(items_.size()) - 1;
    int target = selected_;
    switch (e.vk) {
    case VK_LEFT:
    case VK_UP:
        target = std::max(0, selected_ - 1);
        break;
    case VK_RIGHT:
    case VK_DOWN:
        target = std::min(last, selected_ + 1);
        break;
    case VK_HOME:
        target = 0;
        break;
    case VK_END:
        target = last;
        break;
    default:
        return false;
    }
    // select() may notify a handler that deletes this control: nothing after it.
    select(target, true, true);
    return true;
}

/**
 * @brief Frame-relative rect of a segment (equal widths inside the padded track).
 */
Rect SegmentedControl::segmentRect(int index) const {
    const int n = static_cast<int>(items_.size());
    if (n <= 0 || index < 0 || index >= n) {
        return {};
    }
    const Rect inner = bounds().inset(kPad);
    if (inner.w <= 0.0f || inner.h <= 0.0f) {
        return {};
    }
    const float segW = inner.w / static_cast<float>(n);
    return {inner.x + segW * static_cast<float>(index), inner.y, segW, inner.h};
}

/**
 * @brief Segment under a local point, or -1 when outside the control.
 */
int SegmentedControl::indexAt(Point local) const {
    const int n = static_cast<int>(items_.size());
    if (n <= 0 || !bounds().contains(local)) {
        return -1;
    }
    const Rect inner = bounds().inset(kPad);
    if (inner.w <= 0.0f) {
        return -1;
    }
    // Points in the 2 dip padding map to the nearest segment.
    const float segW = inner.w / static_cast<float>(n);
    const int idx = static_cast<int>(std::floor((local.x - inner.x) / segW));
    return std::clamp(idx, 0, n - 1);
}

/**
 * @brief Applies a selection, moves the pill and optionally notifies.
 *        onChanged may delete this widget, so it is the last call.
 */
void SegmentedControl::select(int index, bool animated, bool notify) {
    const int n = static_cast<int>(items_.size());
    if (n <= 0) {
        return;
    }
    if (index < 0 || index >= n) {
        HH_LOG_WARN(L"SegmentedControl", L"select({}) out of range for {} items; clamping", index, n);
        index = std::clamp(index, 0, n - 1);
    }
    const bool changed = (index != selected_);
    selected_ = index;

    pill_.setOwner(this);
    const Rect target = segmentRect(selected_);
    if (!target.isEmpty()) {
        if (animated) {
            pill_.animateTo(target, springs::snappy);
        } else {
            pill_.set(target);
        }
    }
    invalidate();

    if (!changed || !notify) {
        return;
    }
    std::function<void(int)> fn = onChanged;
    if (fn) {
        fn(selected_);
    }
}

} // namespace hh::ui
