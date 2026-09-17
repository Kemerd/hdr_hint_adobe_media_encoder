// ---------------------------------------------------------------------------
// Label.cpp - static text.
//
// A Label owns nothing device-dependent: it measures through the shared
// TextCache helpers (no Canvas is available during layout) and paints with
// Canvas::drawText. Colour comes from a semantic tone so a theme switch
// repaints every label correctly without any per-widget bookkeeping.
// ---------------------------------------------------------------------------
#include "ui/controls/Label.h"

#include "core/Logger.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

/**
 * @brief Palette used when the label is measured or painted while detached.
 *
 * Widgets can legitimately be painted before they have a root (previews,
 * tests), so every colour lookup falls back to the dark palette instead of
 * dereferencing a null theme pointer.
 */
const Theme& fallbackTheme() {
    static const Theme kDark = Theme::dark();
    return kDark;
}

/**
 * @brief Maps a label tone to the matching theme token.
 * @param t       palette to read from
 * @param tone    semantic tone
 * @param custom  colour used for LabelTone::Custom
 */
Color colorForTone(const Theme& t, LabelTone tone, const Color& custom) {
    switch (tone) {
    case LabelTone::Primary:     return t.labelPrimary;
    case LabelTone::Secondary:   return t.labelSecondary;
    case LabelTone::Tertiary:    return t.labelTertiary;
    case LabelTone::Quaternary:  return t.labelQuaternary;
    case LabelTone::Accent:      return t.accent;
    case LabelTone::Success:     return t.success;
    case LabelTone::Warning:     return t.warning;
    case LabelTone::Destructive: return t.destructive;
    case LabelTone::Custom:      return custom;
    }
    // Unknown enum value (corrupted state): primary text is the safest default.
    return t.labelPrimary;
}

} // namespace

/**
 * @brief Builds a label with text, a type style and a colour tone.
 */
Label::Label(std::wstring text, TextStyle style, LabelTone tone)
    : text_(std::move(text)), style_(style), tone_(tone) {
    // Styles that wrap by construction (bodyWrap etc.) imply "unlimited lines"
    // unless the caller narrows it afterwards with setMaxLines().
    if (style_.wrap) {
        maxLines_ = 0;
    }
}

/**
 * @brief Replaces the text; a no-op when unchanged so callers can refresh freely.
 */
void Label::setText(std::wstring text) {
    if (text_ == text) {
        return;
    }
    text_ = std::move(text);
    // Width changes with the text, so the parent needs to re-measure.
    invalidateLayout();
}

/**
 * @brief Replaces the type style (size/weight/family) and re-measures.
 */
void Label::setStyle(const TextStyle& style) {
    if (style_ == style) {
        return;
    }
    style_ = style;
    // A wrapping style on a single-line label would never wrap (maxLines 1
    // forces single-line measurement), so lift the cap like the constructor.
    if (style_.wrap && maxLines_ == 1) {
        maxLines_ = 0;
    }
    invalidateLayout();
}

/**
 * @brief Changes the semantic colour tone (repaint only, the size is unaffected).
 */
void Label::setTone(LabelTone tone) {
    if (tone_ == tone) {
        return;
    }
    tone_ = tone;
    invalidate();
}

/**
 * @brief Paints the text in an explicit colour (switches the tone to Custom).
 */
void Label::setCustomColor(const Color& c) {
    custom_ = c;
    tone_ = LabelTone::Custom;
    invalidate();
}

/**
 * @brief Horizontal alignment of the text inside the label's frame.
 */
void Label::setAlign(HAlign align) {
    if (align_ == align) {
        return;
    }
    align_ = align;
    invalidate();
}

/**
 * @brief Vertical alignment of the text inside the label's frame.
 */
void Label::setVAlign(VAlign align) {
    if (valign_ == align) {
        return;
    }
    valign_ = align;
    invalidate();
}

/**
 * @brief How overflowing single-line text is shortened (End / Middle / None).
 */
void Label::setTrimming(Trimming t) {
    if (trimming_ == t) {
        return;
    }
    trimming_ = t;
    invalidate();
}

/**
 * @brief Caps the number of lines; 0 means unlimited. Anything other than 1 wraps.
 */
void Label::setMaxLines(int lines) {
    // Negative counts make no sense; treat them as "single line".
    const int clamped = std::max(0, lines);
    if (maxLines_ == clamped) {
        return;
    }
    maxLines_ = clamped;
    invalidateLayout();
}

/**
 * @brief Measures the text through the shared cache.
 *
 * Single-line labels measure unbounded (the parent trims them). Wrapping
 * labels measure against the available width when the constraints bound it,
 * which yields the wrapped height the stack layout needs.
 */
Size Label::measure(const Constraints& c) {
    // Wrapping is implied whenever more than one line is allowed.
    TextStyle style = style_;
    const bool wraps = (maxLines_ != 1);
    style.wrap = wraps;

    // Even an empty label reserves one line so rows do not collapse and
    // later text changes do not shift neighbouring widgets around.
    if (text_.empty()) {
        const TextCache::LineMetrics lm = lineMetricsShared(style);
        const float lineH = lm.lineHeight > 0.0f ? lm.lineHeight : 1.3f * style.size;
        return c.constrain({0.0f, std::ceil(lineH)});
    }

    // Wrapped text needs a width to wrap against; unbounded means one line.
    const float maxW = (wraps && c.hasBoundedWidth()) ? std::max(0.0f, c.maxW) : 0.0f;
    Size s = measureTextShared(text_, style, maxW, maxLines_);

    // Round up so a fractional glyph advance never gets trimmed to "..." by
    // a parent that hands the measured width straight back as a frame.
    s.w = std::ceil(std::max(0.0f, s.w));
    s.h = std::ceil(std::max(0.0f, s.h));
    return c.constrain(s);
}

/**
 * @brief Draws the text in the resolved tone colour.
 */
void Label::paintSelf(Canvas& c) {
    if (text_.empty()) {
        return;
    }
    const Rect b = bounds();
    if (b.isEmpty()) {
        return;
    }

    // The canvas carries the live palette for this frame; prefer it over the
    // root lookup so detached previews still get sensible colours.
    const Color color = colorForTone(c.theme(), tone_, custom_);

    TextStyle style = style_;
    style.wrap = (maxLines_ != 1);

    // DirectWrite only knows end-trimming; middle ellipsis is applied to the
    // string up front and the layout is then drawn untrimmed.
    if (trimming_ == Trimming::Middle && maxLines_ == 1) {
        const std::wstring shortened = c.text().ellipsizeMiddle(text_, style, b.w);
        c.drawText(shortened, style, b, color, align_, valign_, Trimming::None, 1);
        return;
    }

    c.drawText(text_, style, b, color, align_, valign_, trimming_, maxLines_);
}

/**
 * @brief The colour this label paints with under the current theme.
 */
Color Label::resolvedColor() const {
    const Theme* t = theme();
    return colorForTone(t ? *t : fallbackTheme(), tone_, custom_);
}

} // namespace hh::ui
