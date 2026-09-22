// ---------------------------------------------------------------------------
// MacTextLayout.h - one laid-out block of text, CoreText edition.
//
// The DirectWrite IDWriteTextLayout counterpart for the macOS build. It is
// built by TextCache (TextCacheMac.mm) and drawn by Canvas (CanvasMac.mm),
// and it answers the text field's caret / selection questions.
//
// Rules shared with the Windows layout:
//   * uniform line height: every line box is lineHeight tall, the baseline
//     sits `baseline` below the top of its box (DWRITE_LINE_SPACING_METHOD_UNIFORM);
//   * hard line breaks always break; soft wrapping only when asked;
//   * a line cap trims the last visible line with an ellipsis;
//   * colour is not part of the layout: glyphs take the context's fill
//     colour at draw time (kCTForegroundColorFromContextAttributeName).
//
// Indices: the engine's strings are UTF-32 (wchar_t on macOS) while CoreText
// counts UTF-16 code units. The layout keeps both mappings so callers only
// ever see positions in the string they passed in.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/gfx/Geometry.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <cstddef>
#include <string>
#include <vector>

namespace hh::ui {

class MacTextLayout {
public:
    /// One visual line.
    struct Line {
        CTLineRef line = nullptr;   ///< owned (released in the destructor)
        CFIndex start = 0;          ///< first UTF-16 index covered by this line
        CFIndex length = 0;         ///< UTF-16 units covered (before truncation)
        float width = 0.0f;         ///< typographic width incl. trailing whitespace
    };

    MacTextLayout() = default;
    ~MacTextLayout();
    MacTextLayout(const MacTextLayout&) = delete;
    MacTextLayout& operator=(const MacTextLayout&) = delete;

    /**
     * @brief Lays out @p text.
     * @param text           the (already upper-cased when the style asks) text
     * @param font           the resolved font (retained by the layout)
     * @param letterSpacing  dips added after every character (kerning)
     * @param lineHeight     uniform line box height
     * @param baseline       baseline offset from the top of a line box
     * @param maxWidth       wrap / trim width (<= 0: unbounded)
     * @param wrap           soft-wrap at maxWidth (hard breaks always break)
     * @param trim           end-ellipsis for lines wider than maxWidth and for the capped last line
     * @param maxLines       0 = unlimited
     * @return false when CoreText refused the text (the layout stays empty)
     */
    bool build(std::wstring_view text, CTFontRef font, float letterSpacing, float lineHeight, float baseline,
               float maxWidth, bool wrap, bool trim, int maxLines);

    /// Draws every line with its top-left corner at @p origin (flipped context).
    void draw(CGContextRef ctx, Point origin) const;

    // ---- metrics / queries (layout-local coordinates) ------------------------------
    [[nodiscard]] float width() const noexcept { return width_; }
    [[nodiscard]] float height() const noexcept { return lineHeight_ * static_cast<float>(lines_.size()); }
    [[nodiscard]] float maxWidth() const noexcept { return maxWidth_; }
    [[nodiscard]] size_t lineCount() const noexcept { return lines_.size(); }

    /// X of the caret in front of character @p pos (UTF-32 index).
    [[nodiscard]] float caretX(size_t pos) const;
    /// Character position nearest to (x, y), trailing halves rounding up.
    [[nodiscard]] size_t hitTest(float x, float y) const;
    /// Highlight rects for [pos, pos + length).
    [[nodiscard]] std::vector<Rect> rangeRects(size_t pos, size_t length) const;

private:
    /// UTF-32 index -> UTF-16 index (clamped).
    [[nodiscard]] CFIndex toUtf16(size_t pos) const;
    /// UTF-16 index -> UTF-32 index (clamped, rounded to the code point start).
    [[nodiscard]] size_t fromUtf16(CFIndex index) const;
    /// Line that contains a UTF-16 index (the last line for the end position).
    [[nodiscard]] size_t lineForUtf16(CFIndex index) const;

    std::vector<Line> lines_;
    std::vector<CFIndex> utf16Of_;   ///< UTF-16 start of every UTF-32 index, plus one entry for the end
    float width_ = 0.0f;
    float lineHeight_ = 0.0f;
    float baseline_ = 0.0f;
    float maxWidth_ = 0.0f;
};

} // namespace hh::ui
