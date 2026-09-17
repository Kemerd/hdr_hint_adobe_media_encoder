// ---------------------------------------------------------------------------
// RichTextView.h - renders a small markdown subset (the export guide).
//
// Supported: "# ", "## ", "### " headings; paragraphs; "- " bullets;
// "1. " numbered steps; "> " callouts (with leading "Note:"/"Warning:"/
// "Tip:" choosing the tone); fenced ``` code blocks; `inline code`;
// **bold**; pipe tables (rendered as key/value rows or grids); "---" rules.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <string>
#include <vector>

namespace hh::ui {

struct RichRun {
    std::wstring text;
    bool bold = false;
    bool code = false;
};

struct RichBlock {
    enum class Kind { H1, H2, H3, Paragraph, Bullet, Step, Callout, Code, TableRow, TableHeader, Rule, Spacer };
    enum class Tone { Info, Success, Warning, Error };
    Kind kind = Kind::Paragraph;
    Tone tone = Tone::Info;
    int number = 0;                      ///< for Step
    std::vector<RichRun> runs;           ///< inline content
    std::vector<std::wstring> cells;     ///< for table rows
};

class RichTextView : public Widget {
public:
    RichTextView() = default;

    void setBlocks(std::vector<RichBlock> blocks);
    /// Parses the markdown subset and sets the blocks.
    void setMarkdown(std::wstring_view markdown);
    [[nodiscard]] const std::vector<RichBlock>& blocks() const noexcept { return blocks_; }
    /// Plain-text rendering (for "Copy summary").
    [[nodiscard]] std::wstring plainText() const;
    /// Content width cap for readability (0 = fill).
    void setMaxContentWidth(float w) { maxWidth_ = w; invalidateLayout(); }

    static std::vector<RichBlock> parseMarkdown(std::wstring_view markdown);
    static std::vector<RichRun> parseInline(std::wstring_view line);

    Size measure(const Constraints& c) override;
    void onLayout() override;
    void paintSelf(Canvas& c) override;

private:
    struct Laid { Rect rect; };
    void layoutBlocks(float width);
    [[nodiscard]] float blockHeight(const RichBlock& b, float width, Canvas* c);

    std::vector<RichBlock> blocks_;
    std::vector<Laid> laid_;
    float maxWidth_ = 640.0f;
    float measuredWidth_ = 0.0f;
    float totalHeight_ = 0.0f;
};

} // namespace hh::ui
