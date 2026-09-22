// ---------------------------------------------------------------------------
// PopupMenu.cpp - the menu content shown inside a PopupWindow.
//
// The popup container (the window) paints the rounded background, outline
// and shadow; this widget only paints rows. It owns the highlight, keyboard
// navigation, type-ahead, internal scrolling and the open animation.
// ---------------------------------------------------------------------------
#include "ui/controls/PopupMenu.h"

#include "core/Logger.h"
#include "platform/Time.h"
#include "platform/Utf.h"
#include "ui/core/RootView.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"PopupMenu";

// Row anatomy (dips): a 20 dip checkmark column, an optional 22 dip icon
// column, then the label with 12 dips of breathing room on the right.
constexpr float kCheckColumn = 20.0f;
constexpr float kIconColumn = 22.0f;
constexpr float kLabelRightPad = 12.0f;
constexpr float kCheckW = 12.0f;
constexpr float kCheckH = 9.0f;
constexpr float kIconSize = 16.0f;
constexpr float kHighlightRadius = 5.0f;

// Extra width beyond the widest label (check column + right pad + padding + slack).
constexpr float kLabelExtra = 44.0f;

// Tallest the menu grows before it scrolls internally.
constexpr float kMaxHeight = 480.0f;

// Scrollbar: 3 dip wide thumb, 2 dips in from the right edge.
constexpr float kBarWidth = 3.0f;
constexpr float kBarInset = 2.0f;
constexpr float kBarMinThumb = 20.0f;

// Type-ahead: characters typed within 800 ms form one search prefix.
constexpr double kTypeAheadWindow = 0.8;

// Open animation: the menu scales from 0.94 to 1 while fading in.
constexpr float kOpenScaleFrom = 0.94f;

// Opacity applied to disabled rows.
constexpr float kDisabledAlpha = 0.35f;

/**
 * @brief Picks the theme to paint with: the root's when attached, else the canvas's.
 */
const Theme& paintTheme(const Widget& w, const Canvas& c) {
    const Theme* t = w.theme();
    return t ? *t : c.theme();
}

/**
 * @brief True when @p label starts with @p prefix, ignoring case (ordinal).
 */
bool startsWithNoCase(const std::wstring& label, const std::wstring& prefix) {
    if (prefix.empty() || label.size() < prefix.size()) {
        return false;
    }
    // Ordinal, case-insensitive (CompareStringOrdinal on Windows).
    return platform::istartsWith(label, prefix);
}

/**
 * @brief Wall clock for the type-ahead buffer (independent of the animation clock).
 */
double typeAheadClock() {
    return platform::nowMonotonicSeconds();
}

} // namespace

/**
 * @brief Builds a menu with items and the index that shows a checkmark.
 */
PopupMenu::PopupMenu(std::vector<PopupItem> items, int selected)
    : items_(std::move(items)) {
    open_.setOwner(this);
    setSelected(selected);
}

/**
 * @brief Replaces the items; the highlight and selection are re-validated.
 */
void PopupMenu::setItems(std::vector<PopupItem> items) {
    items_ = std::move(items);
    const int count = static_cast<int>(items_.size());

    // Indices that no longer exist are cleared rather than left dangling.
    if (selected_ >= count) {
        selected_ = -1;
    }
    if (highlight_ >= count) {
        highlight_ = -1;
    }
    scroll_ = 0.0f;
    typeAhead_.clear();
    invalidateLayout();
}

/**
 * @brief Shows a checkmark at @p index (-1 = none). Out-of-range clears it.
 */
void PopupMenu::setSelected(int index) {
    const int count = static_cast<int>(items_.size());
    selected_ = (index >= 0 && index < count) ? index : -1;
    invalidate();
}

// ---- geometry ---------------------------------------------------------------

/**
 * @brief Height of row @p i (two-line rows are taller); 0 when out of range.
 */
float PopupMenu::rowHeight(int i) const {
    if (i < 0 || i >= static_cast<int>(items_.size())) {
        return 0.0f;
    }
    return items_[static_cast<size_t>(i)].subtitle.empty() ? kRowHeight : kRowHeightTwoLine;
}

/**
 * @brief Rect of row @p i in local coordinates (scroll applied); empty when out of range.
 */
Rect PopupMenu::rowRect(int i) const {
    if (i < 0 || i >= static_cast<int>(items_.size())) {
        return {};
    }

    // Walk the rows above to find this one's content offset.
    float y = kPadding - scroll_;
    for (int j = 0; j < i; ++j) {
        y += rowHeight(j);
        if (items_[static_cast<size_t>(j)].separatorAfter) {
            y += kSeparatorHeight;
        }
    }
    const float w = std::max(0.0f, frame_.w - 2.0f * kPadding);
    return {kPadding, y, w, rowHeight(i)};
}

/**
 * @brief Index of the row under @p local, or -1.
 */
int PopupMenu::indexAt(Point local) const {
    const int count = static_cast<int>(items_.size());
    for (int i = 0; i < count; ++i) {
        if (rowRect(i).contains(local)) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Natural size: widest label plus columns, all rows plus padding (capped).
 */
Size PopupMenu::measure(const Constraints& c) {
    float widest = 0.0f;
    float height = 2.0f * kPadding;
    bool anyIcon = false;

    // Measure every label (and subtitle) so the menu never trims its own rows.
    const TextStyle labelStyle = typography::callout();
    const TextStyle subStyle = typography::caption();
    for (size_t i = 0; i < items_.size(); ++i) {
        const PopupItem& it = items_[i];
        widest = std::max(widest, measureTextShared(it.label, labelStyle).w);
        if (!it.subtitle.empty()) {
            widest = std::max(widest, measureTextShared(it.subtitle, subStyle).w);
        }
        if (it.icon != IconId::None) {
            anyIcon = true;
        }
        height += rowHeight(static_cast<int>(i));
        if (it.separatorAfter) {
            height += kSeparatorHeight;
        }
    }

    // Remember the full content height; the frame may end up shorter.
    contentHeight_ = height;

    const float width = std::max(std::max(0.0f, minWidth_), widest + kLabelExtra + (anyIcon ? kIconColumn : 0.0f));
    const float capped = std::min(height, kMaxHeight);
    return c.constrain({width, capped});
}

/**
 * @brief Clamps the scroll offset to the new frame.
 */
void PopupMenu::onLayout() {
    const float maxScroll = std::max(0.0f, contentHeight_ - frame_.h);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
}

/**
 * @brief Scrolls so row @p index is fully inside the padded viewport.
 */
void PopupMenu::ensureVisible(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size()) || frame_.h <= 0.0f) {
        return;
    }

    // Convert the row to content space (scroll removed) and nudge the offset.
    const Rect r = rowRect(index);
    const float top = r.y + scroll_;
    const float bottom = top + r.h;
    const float viewTop = kPadding;
    const float viewBottom = frame_.h - kPadding;

    float s = scroll_;
    if (top - s < viewTop) {
        s = top - viewTop;
    } else if (bottom - s > viewBottom) {
        s = bottom - viewBottom;
    }
    const float maxScroll = std::max(0.0f, contentHeight_ - frame_.h);
    s = std::clamp(s, 0.0f, maxScroll);
    if (s != scroll_) {
        scroll_ = s;
        invalidate();
    }
}

// ---- painting -----------------------------------------------------------------

/**
 * @brief Paints the rows, checkmarks, separators and the scroll thumb.
 */
void PopupMenu::paintSelf(Canvas& c) {
    const Theme& t = paintTheme(*this, c);
    const Rect b = bounds();
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return;
    }

    // Open animation: fade and grow from the top-left corner.
    const float open = std::clamp(open_.value(), 0.0f, 1.0f);
    if (open <= 0.001f) {
        return;
    }
    const bool animating = open < 0.999f;
    if (animating) {
        c.pushOpacity(open);
        const float s = kOpenScaleFrom + (1.0f - kOpenScaleFrom) * open;
        c.pushTransform(Transform2D::scale(s, s, Point(0.0f, 0.0f)));
    }

    // Rows scroll under the padding, so clip to our bounds.
    c.pushClip(b);

    const TextStyle labelStyle = typography::callout();
    const TextStyle subStyle = typography::caption();
    const bool anyIcon = std::any_of(items_.begin(), items_.end(), [](const PopupItem& it) { return it.icon != IconId::None; });
    const int count = static_cast<int>(items_.size());

    for (int i = 0; i < count; ++i) {
        const PopupItem& it = items_[static_cast<size_t>(i)];
        const Rect row = rowRect(i);

        // Skip rows entirely outside the viewport (cheap culling for long lists).
        if (row.bottom() < 0.0f || row.y > b.h) {
            continue;
        }

        const bool highlighted = (i == highlight_) && it.enabled;

        // Highlighted row: accent pill with accent-coloured text.
        if (highlighted) {
            c.fillRoundedRect(row, kHighlightRadius, t.accent);
        }

        // Resolve the label colours for this row's state.
        Color label = highlighted ? t.accentText : (it.destructive ? t.destructive : t.labelPrimary);
        Color sub = highlighted ? t.accentText.scaledAlpha(0.8f) : t.labelSecondary;
        Color glyph = highlighted ? t.accentText : t.labelPrimary;
        if (!it.enabled) {
            label = label.scaledAlpha(kDisabledAlpha);
            sub = sub.scaledAlpha(kDisabledAlpha);
            glyph = glyph.scaledAlpha(kDisabledAlpha);
        }

        // Checkmark column.
        float x = row.x;
        if (i == selected_) {
            const Rect check{x + (kCheckColumn - kCheckW) * 0.5f, row.y + (row.h - kCheckH) * 0.5f, kCheckW, kCheckH};
            c.drawIcon(IconId::Check, check, glyph, 1.5f);
        }
        x += kCheckColumn;

        // Optional icon column (reserved for every row when any row has one).
        if (anyIcon) {
            if (it.icon != IconId::None) {
                const Rect icon{x + (kIconColumn - kIconSize) * 0.5f, row.y + (row.h - kIconSize) * 0.5f, kIconSize, kIconSize};
                c.drawIcon(it.icon, icon, glyph, 1.5f);
            }
            x += kIconColumn;
        }

        // Label (and subtitle for two-line rows).
        const float labelW = std::max(0.0f, row.right() - kLabelRightPad - x);
        if (it.subtitle.empty()) {
            c.drawText(it.label, labelStyle, {x, row.y, labelW, row.h}, label, HAlign::Left, VAlign::Center, Trimming::End, 1);
        } else {
            const float half = row.h * 0.5f;
            c.drawText(it.label, labelStyle, {x, row.y + 2.0f, labelW, half}, label, HAlign::Left, VAlign::Center, Trimming::End, 1);
            c.drawText(it.subtitle, subStyle, {x, row.y + half - 2.0f, labelW, half}, sub, HAlign::Left, VAlign::Center, Trimming::End, 1);
        }

        // Hairline separator centred in the gap after this row.
        if (it.separatorAfter) {
            const float y = row.bottom() + kSeparatorHeight * 0.5f;
            c.drawHairline({row.x + 8.0f, y}, {row.right() - 8.0f, y}, t.separator);
        }
    }

    // Thin scroll thumb when the content is taller than the frame.
    if (contentHeight_ > b.h + 0.5f) {
        const float trackH = std::max(0.0f, b.h - 2.0f * kPadding);
        const float ratio = std::clamp(b.h / contentHeight_, 0.0f, 1.0f);
        const float thumbH = std::clamp(trackH * ratio, std::min(kBarMinThumb, trackH), trackH);
        const float maxScroll = std::max(1.0f, contentHeight_ - b.h);
        const float thumbY = kPadding + (trackH - thumbH) * std::clamp(scroll_ / maxScroll, 0.0f, 1.0f);
        const Rect thumb{b.w - kBarInset - kBarWidth, thumbY, kBarWidth, thumbH};
        c.fillRoundedRect(thumb, kBarWidth * 0.5f, t.scrollThumb);
    }

    c.pop();    // clip
    if (animating) {
        c.pop();    // transform
        c.pop();    // opacity
    }
}

// ---- mouse ------------------------------------------------------------------------

/**
 * @brief Press highlights the row under the pointer and captures the mouse.
 */
bool PopupMenu::onMouseDown(const MouseEvent& e) {
    const int i = indexAt(e.pos);
    const int next = (i >= 0 && items_[static_cast<size_t>(i)].enabled) ? i : -1;
    if (next != highlight_) {
        highlight_ = next;
        invalidate();
    }
    return true;
}

/**
 * @brief Release on an enabled row chooses it (press-drag-release works too).
 */
bool PopupMenu::onMouseUp(const MouseEvent& e) {
    const int i = indexAt(e.pos);
    if (i >= 0 && items_[static_cast<size_t>(i)].enabled) {
        choose(i);
    }
    return true;
}

/**
 * @brief Hover moves the highlight (disabled rows clear it).
 */
bool PopupMenu::onMouseMove(const MouseEvent& e) {
    const int i = indexAt(e.pos);
    const int next = (i >= 0 && items_[static_cast<size_t>(i)].enabled) ? i : -1;
    if (next != highlight_) {
        highlight_ = next;
        invalidate();
    }
    return true;
}

/**
 * @brief Leaving the menu clears the hover highlight.
 */
void PopupMenu::onMouseLeave() {
    if (highlight_ != -1) {
        highlight_ = -1;
        invalidate();
    }
}

/**
 * @brief Scrolls the rows when the content is taller than the menu.
 */
bool PopupMenu::onWheel(const WheelEvent& e) {
    const float maxScroll = std::max(0.0f, contentHeight_ - frame_.h);
    if (maxScroll <= 0.0f) {
        return false;
    }

    // One notch scrolls linesPerNotch rows-ish (a page when the system says -1).
    const float notches = e.delta / 120.0f;
    const float step = (e.linesPerNotch < 0) ? std::max(frame_.h, kRowHeight)
                                             : static_cast<float>(std::max(1, e.linesPerNotch)) * (kRowHeight * 0.75f);
    const float next = std::clamp(scroll_ - notches * step, 0.0f, maxScroll);
    if (next != scroll_) {
        scroll_ = next;

        // The row under the (stationary) pointer changed; re-evaluate the highlight.
        const int i = indexAt(e.pos);
        highlight_ = (i >= 0 && items_[static_cast<size_t>(i)].enabled) ? i : -1;
        invalidate();
    }
    return true;
}

// ---- keyboard ---------------------------------------------------------------------

/**
 * @brief Moves the highlight by @p delta rows, skipping disabled ones (no wrap).
 */
void PopupMenu::moveHighlight(int delta) {
    const int count = static_cast<int>(items_.size());
    if (count == 0 || delta == 0) {
        return;
    }

    // Start just outside the list when nothing is highlighted yet.
    int i = highlight_;
    if (i < 0 || i >= count) {
        i = (delta > 0) ? -1 : count;
    }

    // Step until an enabled row is found or the end is reached.
    const int step = (delta > 0) ? 1 : -1;
    int remaining = std::abs(delta);
    int candidate = i;
    while (remaining > 0) {
        int probe = candidate + step;
        while (probe >= 0 && probe < count && !items_[static_cast<size_t>(probe)].enabled) {
            probe += step;
        }
        if (probe < 0 || probe >= count) {
            break;
        }
        candidate = probe;
        --remaining;
    }

    if (candidate >= 0 && candidate < count && candidate != highlight_ && items_[static_cast<size_t>(candidate)].enabled) {
        highlight_ = candidate;
        ensureVisible(candidate);
        invalidate();
    }
}

/**
 * @brief Arrow/Home/End navigate, Enter/Space choose, Escape cancels.
 */
bool PopupMenu::onKeyDown(const KeyEvent& e) {
    const int count = static_cast<int>(items_.size());

    switch (e.vk) {
    case VK_UP:
        moveHighlight(-1);
        return true;
    case VK_DOWN:
        moveHighlight(1);
        return true;
    case VK_PRIOR:
        moveHighlight(-8);
        return true;
    case VK_NEXT:
        moveHighlight(8);
        return true;
    case VK_HOME:
        highlight_ = -1;
        moveHighlight(1);
        return true;
    case VK_END:
        highlight_ = -1;
        moveHighlight(-1);
        return true;
    case VK_SPACE: {
        // A space typed mid type-ahead is part of the search, not a choice.
        const bool searching = !typeAhead_.empty() && (typeAheadClock() - typeAheadAt_) <= kTypeAheadWindow;
        if (searching) {
            return false;
        }
        [[fallthrough]];
    }
    case VK_RETURN:
        if (highlight_ >= 0 && highlight_ < count && items_[static_cast<size_t>(highlight_)].enabled) {
            choose(highlight_);
        }
        return true;
    case VK_ESCAPE: {
        // Copy the callback: cancelling tears the popup down and may drop us.
        RootView* rv = root();
        auto cb = onCancel;
        if (cb) {
            cb();
        }
        if (rv) {
            rv->window().dismissPopupWindow();
        }
        return true;
    }
    default:
        return false;
    }
}

/**
 * @brief Type-ahead: characters typed within 800 ms select the first matching label.
 */
bool PopupMenu::onChar(char32_t ch) {
    if (ch < 0x20 || ch == 0x7F) {
        return false;
    }

    // Expire the buffer when the last keystroke is too old.
    const double now = typeAheadClock();
    if (now - typeAheadAt_ > kTypeAheadWindow) {
        typeAhead_.clear();
    }

    // A lone space is the "choose" key, not the start of a search.
    if (ch == U' ' && typeAhead_.empty()) {
        return false;
    }
    typeAheadAt_ = now;

    // Append as UTF-16 (astral characters become a surrogate pair).
    if (ch > 0xFFFF) {
        const char32_t v = ch - 0x10000;
        typeAhead_.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
        typeAhead_.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
    } else {
        typeAhead_.push_back(static_cast<wchar_t>(ch));
    }

    // Highlight the first enabled label that starts with the buffer.
    const int count = static_cast<int>(items_.size());
    for (int i = 0; i < count; ++i) {
        const PopupItem& it = items_[static_cast<size_t>(i)];
        if (it.enabled && startsWithNoCase(it.label, typeAhead_)) {
            if (i != highlight_) {
                highlight_ = i;
                ensureVisible(i);
                invalidate();
            }
            return true;
        }
    }
    return true;
}

/**
 * @brief Runs the selection callback, then asks the window to dismiss the popup.
 *
 * The callback is copied first because the opener typically closes the popup
 * from inside it; nothing on @c this is touched after the callback returns.
 */
void PopupMenu::choose(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        HH_LOG_WARN(kLog, L"choose: index {} out of range ({} items)", index, items_.size());
        return;
    }
    if (!items_[static_cast<size_t>(index)].enabled) {
        return;
    }

    RootView* rv = root();
    auto cb = onSelect;
    if (cb) {
        cb(index);
    }
    if (rv) {
        rv->window().dismissPopupWindow();
    }
}

/**
 * @brief On attach: take focus, highlight the checked row and play the open spring.
 */
void PopupMenu::onAttached() {
    open_.setOwner(this);

    // Keyboard navigation needs focus inside the popup's own root.
    RootView* rv = root();
    if (rv && rv->focus().focused() != this) {
        rv->focus().focus(this);
    }

    // Start from the checked item, like a macOS pop-up button menu.
    const int count = static_cast<int>(items_.size());
    if (highlight_ < 0 && selected_ >= 0 && selected_ < count && items_[static_cast<size_t>(selected_)].enabled) {
        highlight_ = selected_;
    }
    ensureVisible(highlight_);

    // Scale + fade in; Animatable snaps when there is no timeline or reduced motion.
    open_.set(0.0f);
    open_.animateTo(1.0f, springs::snappy);
    invalidate();
}

} // namespace hh::ui
