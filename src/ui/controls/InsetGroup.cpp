// ---------------------------------------------------------------------------
// InsetGroup.cpp - macOS "grouped inset" settings list.
//
// SettingsRow lays its three parts out by hand (title/subtitle column on the
// left, accessory right-aligned and vertically centred, or stretched below
// the text when asked). InsetGroup is an ordinary vertical stack whose rows
// container is painted as one rounded box with hairline dividers between
// the rows.
// ---------------------------------------------------------------------------
#include "ui/controls/InsetGroup.h"

#include "core/Logger.h"
#include "ui/controls/Label.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"InsetGroup";

// ---- row metrics -----------------------------------------------------------
constexpr float kRowPadX = 16.0f;
constexpr float kRowPadY = 10.0f;
constexpr float kRowMinHeight = 44.0f;
constexpr float kRowMinHeightNoSubtitle = 32.0f;
/// Gap between the text column and a trailing accessory.
constexpr float kAccessoryGap = 12.0f;
/// Smallest label column a side-by-side row will accept. A wide accessory
/// (a pop-up carrying a long file name) is squeezed instead of collapsing
/// the label to an ellipsis, which is useless to read.
constexpr float kMinTitleW = 96.0f;
/// Largest share of the row a label may claim from a wide accessory.
constexpr float kMaxTitleShare = 0.5f;
/// Gap between the text and an accessory placed on its own line.
constexpr float kAccessoryBelowGap = 8.0f;
/// Gap between title and subtitle.
constexpr float kLineGap = 2.0f;
/// Width assumed when a row is measured without any width bound.
constexpr float kUnboundedRowWidth = 480.0f;
/// Layout treats anything larger than this as unbounded.
constexpr float kUnbounded = 1e8f;

// ---- group metrics ---------------------------------------------------------
constexpr float kGroupRadius = 10.0f;
constexpr float kHeaderPadLeft = 16.0f;
constexpr float kHeaderPadRight = 4.0f;
constexpr float kHeaderSpacing = 8.0f;
/// Space between the header caption and the rounded box.
constexpr float kHeaderGap = 6.0f;
constexpr float kGroupMarginBottom = 16.0f;
constexpr float kDividerInset = 16.0f;
constexpr float kEmptyPadding = 16.0f;
constexpr float kHeaderLetterSpacing = 0.4f;

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
 * @brief Style for the group header: uppercase caption with a little tracking.
 */
TextStyle headerStyle() {
    TextStyle s = typography::caption();
    s.uppercase = true;
    s.letterSpacing = kHeaderLetterSpacing;
    return s;
}

/**
 * @brief Shows the right container for the row count and hides the header
 *        line when it has neither text nor an accessory.
 *
 * setVisible() early-outs when nothing changes, so calling this from
 * measure() does not trigger needless layout passes.
 */
void syncGroupVisibility(Widget* headerRow, Widget* header, Widget* headerAccessory, Widget* rows, Widget* empty) {
    const bool hasRows = rows != nullptr && !rows->children().empty();
    if (rows) {
        rows->setVisible(hasRows);
    }
    if (empty) {
        empty->setVisible(!hasRows);
    }

    // The header line only earns its space when it shows something.
    bool headerText = false;
    if (auto* label = dynamic_cast<Label*>(header)) {
        headerText = !label->text().empty();
    }
    const bool hasAccessory = headerAccessory != nullptr && headerAccessory->visible();
    if (headerRow) {
        headerRow->setVisible(headerText || hasAccessory);
    }
}

} // namespace

// ===========================================================================
// SettingsRow
// ===========================================================================

/**
 * @brief Creates a row with a body title and an optional caption subtitle.
 */
SettingsRow::SettingsRow(std::wstring title, std::wstring subtitle) {
    auto titleLabel = std::make_unique<Label>(std::move(title), typography::body(), LabelTone::Primary);
    titleLabel->setTrimming(Trimming::End);
    title_ = add(std::move(titleLabel));

    // Subtitles are mostly paths ("C:\...\mkvmerge.exe"), so they trim in the
    // middle to keep the file name readable.
    const bool hasSubtitle = !subtitle.empty();
    auto subtitleLabel = std::make_unique<Label>(std::move(subtitle), typography::caption(), LabelTone::Secondary);
    subtitleLabel->setTrimming(Trimming::Middle);
    subtitleLabel->setVisible(hasSubtitle);
    subtitle_ = add(std::move(subtitleLabel));
}

/**
 * @brief Replaces the title text.
 */
void SettingsRow::setTitle(std::wstring title) {
    if (auto* label = dynamic_cast<Label*>(title_)) {
        label->setText(std::move(title));
    }
}

/**
 * @brief Replaces the subtitle; an empty subtitle collapses the row to 32 dip.
 */
void SettingsRow::setSubtitle(std::wstring subtitle) {
    auto* label = dynamic_cast<Label*>(subtitle_);
    if (!label) {
        return;
    }
    const bool show = !subtitle.empty();
    label->setText(std::move(subtitle));
    label->setVisible(show);
    invalidateLayout();
}

/**
 * @brief Colours the subtitle destructive (e.g. "not found") or back to secondary.
 */
void SettingsRow::setSubtitleDestructive(bool on) {
    if (auto* label = dynamic_cast<Label*>(subtitle_)) {
        label->setTone(on ? LabelTone::Destructive : LabelTone::Secondary);
    }
}

/**
 * @brief Installs (or replaces) the trailing control.
 * @return the raw pointer to the installed accessory (nullptr when @p accessory was null)
 */
Widget* SettingsRow::setAccessory(std::unique_ptr<Widget> accessory) {
    // Drop the old one first so the new widget becomes the topmost child.
    if (accessory_) {
        std::unique_ptr<Widget> old = removeChild(accessory_);
        old.reset();
        accessory_ = nullptr;
    }
    if (!accessory) {
        invalidateLayout();
        return nullptr;
    }
    accessory_ = addChild(std::move(accessory));
    invalidateLayout();
    return accessory_;
}

/**
 * @brief Width the title column keeps when a side-by-side accessory is wide.
 *
 * A pop-up carrying a long file name must shrink (it ellipsizes its value)
 * before a short label like "Default LUT (PQ)" is cut: the floor is the
 * title's natural single-line width, bounded to [kMinTitleW, half the row].
 */
float SettingsRow::titleFloor(float innerW) {
    const float cap = std::max(0.0f, innerW);
    float natural = 0.0f;
    if (title_ && title_->visible()) {
        natural = std::max(0.0f, title_->measure(looseWidth(cap)).w);
    }
    // The label's own width, but never more than half the row (the control
    // keeps the other half) and never less than the old fixed floor.
    const float floorW = std::max(kMinTitleW, std::min(natural, cap * kMaxTitleShare));
    return std::min(floorW, cap);
}

/**
 * @brief Measures the row: text column beside (or above) the accessory.
 *
 * The width comes from the parent; the height is the taller of the text
 * column and the accessory plus 10 dip padding, never below 44 dip (32 when
 * there is no subtitle).
 */
Size SettingsRow::measure(const Constraints& c) {
    const bool bounded = c.maxW < kUnbounded;
    const float availW = bounded ? std::max(0.0f, c.maxW) : kUnboundedRowWidth;
    const float innerW = std::max(0.0f, availW - kRowPadX * 2.0f);

    // Accessory first: its width decides how much the text column gets.
    Size acc;
    const bool hasAccessory = accessory_ != nullptr && accessory_->visible();
    if (hasAccessory) {
        acc = accessory_->measure(looseWidth(innerW));
        acc.w = std::clamp(acc.w, 0.0f, innerW);
        acc.h = std::max(0.0f, acc.h);
    }

    // Text column: full width when the accessory sits below, else what is left
    // after the accessory, with the same minimum the layout pass enforces so
    // the measured height matches what is actually drawn.
    float textW = innerW;
    if (hasAccessory && !below_) {
        const float wanted = std::max(0.0f, innerW - acc.w - kAccessoryGap);
        const float floorW = titleFloor(innerW);
        if (wanted < floorW) {
            textW = floorW;
            acc.w = std::max(0.0f, innerW - floorW - kAccessoryGap);
        } else {
            textW = wanted;
        }
    }
    float textH = 0.0f;
    float naturalTextW = 0.0f;
    if (title_ && title_->visible()) {
        const Size s = title_->measure(looseWidth(textW));
        textH += std::max(0.0f, s.h);
        naturalTextW = std::max(naturalTextW, s.w);
    }
    const bool hasSubtitle = subtitle_ != nullptr && subtitle_->visible();
    if (hasSubtitle) {
        const Size s = subtitle_->measure(looseWidth(textW));
        textH += kLineGap + std::max(0.0f, s.h);
        naturalTextW = std::max(naturalTextW, s.w);
    }

    // Height: stacked or side by side, then the HIG minimum.
    float h = kRowPadY * 2.0f;
    if (below_) {
        h += textH + (hasAccessory ? kAccessoryBelowGap + acc.h : 0.0f);
    } else {
        h += std::max(textH, acc.h);
    }
    h = std::max(h, hasSubtitle ? kRowMinHeight : kRowMinHeightNoSubtitle);

    // Width: fill the parent when bounded, else hug the content.
    float w = availW;
    if (!bounded) {
        w = kRowPadX * 2.0f + naturalTextW + (hasAccessory && !below_ ? kAccessoryGap + acc.w : 0.0f);
        if (below_ && hasAccessory) {
            w = std::max(w, kRowPadX * 2.0f + acc.w);
        }
    }
    return c.constrain({w, h});
}

/**
 * @brief Positions title, subtitle and accessory inside the final frame.
 */
void SettingsRow::onLayout() {
    const Rect b = bounds();
    const float innerW = std::max(0.0f, b.w - kRowPadX * 2.0f);
    const float innerH = std::max(0.0f, b.h - kRowPadY * 2.0f);

    // Accessory size (clamped to the row so a wide control never escapes).
    Size acc;
    const bool hasAccessory = accessory_ != nullptr && accessory_->visible();
    if (hasAccessory) {
        acc = accessory_->measure(looseWidth(innerW));
        acc.w = std::clamp(acc.w, 0.0f, innerW);
        acc.h = std::clamp(acc.h, 0.0f, std::max(innerH, 0.0f));
    }

    // Text column width and the two label heights. The accessory yields space
    // back to the label when the row is too narrow to give both what they want.
    float textW = innerW;
    if (hasAccessory && !below_) {
        const float wanted = std::max(0.0f, innerW - acc.w - kAccessoryGap);
        const float floorW = titleFloor(innerW);
        if (wanted < floorW) {
            textW = floorW;
            acc.w = std::max(0.0f, innerW - floorW - kAccessoryGap);
        } else {
            textW = wanted;
        }
    }
    float titleH = 0.0f;
    if (title_ && title_->visible()) {
        titleH = std::max(0.0f, title_->measure(looseWidth(textW)).h);
    }
    float subtitleH = 0.0f;
    const bool hasSubtitle = subtitle_ != nullptr && subtitle_->visible();
    if (hasSubtitle) {
        subtitleH = std::max(0.0f, subtitle_->measure(looseWidth(textW)).h);
    }
    const float textH = titleH + (hasSubtitle ? kLineGap + subtitleH : 0.0f);

    if (below_) {
        // Text on top, accessory stretched across the full inner width below.
        float y = kRowPadY;
        if (title_ && title_->visible()) {
            title_->layout({kRowPadX, y, textW, titleH});
            y += titleH;
        }
        if (hasSubtitle) {
            y += kLineGap;
            subtitle_->layout({kRowPadX, y, textW, subtitleH});
            y += subtitleH;
        }
        if (hasAccessory) {
            y += kAccessoryBelowGap;
            accessory_->layout({kRowPadX, y, innerW, acc.h});
        }
        return;
    }

    // Side by side: the text column is centred vertically, the accessory too.
    const float textY = kRowPadY + std::max(0.0f, (innerH - textH) * 0.5f);
    if (title_ && title_->visible()) {
        title_->layout({kRowPadX, textY, textW, titleH});
    }
    if (hasSubtitle) {
        subtitle_->layout({kRowPadX, textY + titleH + kLineGap, textW, subtitleH});
    }
    if (hasAccessory) {
        const float ax = std::max(kRowPadX, b.w - kRowPadX - acc.w);
        const float ay = kRowPadY + std::max(0.0f, (innerH - acc.h) * 0.5f);
        accessory_->layout({ax, ay, acc.w, acc.h});
    }
}

/**
 * @brief Rows paint nothing of their own.
 *
 * The rounded box and the dividers belong to the InsetGroup so the group
 * reads as one surface; the labels and the accessory are children and paint
 * themselves.
 */
void SettingsRow::paintSelf(Canvas&) {
}

// ===========================================================================
// InsetGroup
// ===========================================================================

/**
 * @brief Creates a group with an optional uppercase header caption.
 */
InsetGroup::InsetGroup(std::wstring header) {
    stack().axis = Axis::Vertical;
    stack().spacing = kHeaderGap;
    stack().crossAlign = CrossAlign::Stretch;
    layoutParams().margin.bottom = kGroupMarginBottom;

    // Header line: caption on the left (flexible), optional accessory on the right.
    auto row = std::make_unique<Widget>();
    row->stack().axis = Axis::Horizontal;
    row->stack().spacing = kHeaderSpacing;
    row->stack().padding = Insets{0.0f, kHeaderPadRight, 0.0f, kHeaderPadLeft};
    row->stack().crossAlign = CrossAlign::Center;
    auto caption = std::make_unique<Label>(std::move(header), headerStyle(), LabelTone::Secondary);
    caption->layoutParams().flexGrow = 1.0f;
    header_ = row->add(std::move(caption));
    headerRow_ = add(std::move(row));

    // Rows container: painted as the rounded box by paintSelf().
    auto rows = std::make_unique<Widget>();
    rows->stack().axis = Axis::Vertical;
    rows->stack().crossAlign = CrossAlign::Stretch;
    rows_ = add(std::move(rows));

    // Empty placeholder: a padded callout inside the same rounded box.
    auto empty = std::make_unique<Widget>();
    empty->stack().padding = Insets::all(kEmptyPadding);
    auto emptyLabel = std::make_unique<Label>(L"", typography::calloutWrap(), LabelTone::Tertiary);
    empty->add(std::move(emptyLabel));
    empty_ = add(std::move(empty));

    syncGroupVisibility(headerRow_, header_, headerAccessory_, rows_, empty_);
}

/**
 * @brief Replaces the header caption (hidden when empty and there is no accessory).
 */
void InsetGroup::setHeader(std::wstring header) {
    if (auto* label = dynamic_cast<Label*>(header_)) {
        label->setText(std::move(header));
    }
    syncGroupVisibility(headerRow_, header_, headerAccessory_, rows_, empty_);
    invalidateLayout();
}

/**
 * @brief Installs a trailing header widget (e.g. an "Add" button).
 */
Widget* InsetGroup::setHeaderAccessory(std::unique_ptr<Widget> w) {
    if (!headerRow_) {
        HH_LOG_WARN(kLog, L"setHeaderAccessory: no header row");
        return nullptr;
    }
    if (headerAccessory_) {
        std::unique_ptr<Widget> old = headerRow_->removeChild(headerAccessory_);
        old.reset();
        headerAccessory_ = nullptr;
    }
    if (w) {
        headerAccessory_ = headerRow_->addChild(std::move(w));
    }
    syncGroupVisibility(headerRow_, header_, headerAccessory_, rows_, empty_);
    invalidateLayout();
    return headerAccessory_;
}

/**
 * @brief Appends a title/subtitle row and returns it for accessory setup.
 */
SettingsRow* InsetGroup::addRow(std::wstring title, std::wstring subtitle) {
    if (!rows_) {
        HH_LOG_WARN(kLog, L"addRow: no rows container");
        return nullptr;
    }
    SettingsRow* row = rows_->add(std::make_unique<SettingsRow>(std::move(title), std::move(subtitle)));
    syncGroupVisibility(headerRow_, header_, headerAccessory_, rows_, empty_);
    invalidateLayout();
    return row;
}

/**
 * @brief Appends any widget as a row.
 */
Widget* InsetGroup::addCustomRow(std::unique_ptr<Widget> row) {
    if (!rows_) {
        HH_LOG_WARN(kLog, L"addCustomRow: no rows container");
        return nullptr;
    }
    if (!row) {
        HH_LOG_WARN(kLog, L"addCustomRow: null row ignored");
        return nullptr;
    }
    Widget* raw = rows_->addChild(std::move(row));
    syncGroupVisibility(headerRow_, header_, headerAccessory_, rows_, empty_);
    invalidateLayout();
    return raw;
}

/**
 * @brief Removes every row; the empty text (if any) takes their place.
 */
void InsetGroup::clearRows() {
    if (rows_) {
        rows_->clearChildren();
    }
    syncGroupVisibility(headerRow_, header_, headerAccessory_, rows_, empty_);
    invalidateLayout();
}

/**
 * @brief Text shown inside the box while the group has no rows.
 */
void InsetGroup::setEmptyText(std::wstring text) {
    if (!empty_ || empty_->children().empty()) {
        return;
    }
    if (auto* label = dynamic_cast<Label*>(empty_->children().front().get())) {
        label->setText(std::move(text));
    }
    invalidateLayout();
}

/**
 * @brief Stack measurement after making sure the right containers are visible.
 */
Size InsetGroup::measure(const Constraints& c) {
    syncGroupVisibility(headerRow_, header_, headerAccessory_, rows_, empty_);
    return Widget::measure(c);
}

/**
 * @brief Paints the rounded box behind the rows (or the empty text).
 */
void InsetGroup::paintSelf(Canvas& c) {
    // Whichever container is showing owns the box.
    Widget* box = nullptr;
    if (rows_ && rows_->visible()) {
        box = rows_;
    } else if (empty_ && empty_->visible()) {
        box = empty_;
    }
    if (!box) {
        return;
    }
    const Rect raw = box->frame();
    if (raw.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();

    // Elevated fill with a hairline outline, pixel-snapped like every card.
    const Rect b = c.scale().snap(raw);
    c.fillRoundedRect(b, kGroupRadius, t.elevated);
    c.strokeRoundedRect(b, kGroupRadius, t.separator, 0.0f);
}

/**
 * @brief Draws the hairline dividers between rows, inset 16 dip from the left.
 */
void InsetGroup::paintOverlay(Canvas& c) {
    if (!rows_ || !rows_->visible()) {
        return;
    }
    const Rect box = rows_->frame();
    if (box.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();

    // A divider sits on the top edge of every visible row but the first.
    bool first = true;
    for (const auto& child : rows_->children()) {
        if (!child || !child->visible()) {
            continue;
        }
        if (first) {
            first = false;
            continue;
        }
        const float y = box.y + child->frame().y;
        c.drawHairline({box.x + kDividerInset, y}, {box.right(), y}, t.separator);
    }
}

} // namespace hh::ui
