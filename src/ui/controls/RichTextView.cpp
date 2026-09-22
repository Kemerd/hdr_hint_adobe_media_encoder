// ---------------------------------------------------------------------------
// RichTextView.cpp - renders a small markdown subset (the export guide).
//
// Three stages:
//   1. parseMarkdown() turns text into RichBlocks with inline runs
//      (bold / code) and table cells;
//   2. layoutBlocks() stacks the blocks for a width, measuring each one with
//      the shared text helpers (no Canvas exists during layout);
//   3. paintSelf() draws every block at its cached rect.
//
// Inline runs are word-wrapped by a small greedy breaker that measures word
// advances through the text cache. Word widths are memoised process-wide
// (keyed by style + word) so repainting a long guide never re-shapes text.
// ---------------------------------------------------------------------------
#include "ui/controls/RichTextView.h"

#include "core/Logger.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <utility>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"RichText";

// ---- metrics ---------------------------------------------------------------
constexpr float kBulletIndent = 16.0f;
constexpr float kBulletDotRadius = 2.5f;
constexpr float kStepIndent = 24.0f;
constexpr float kCalloutRadius = 8.0f;
constexpr float kCalloutPad = 10.0f;
constexpr float kCalloutBar = 3.0f;
/// The tone bar sits inside the rounded corners, not on the edge.
constexpr float kCalloutBarInset = 6.0f;
constexpr float kCalloutTint = 0.12f;
constexpr float kCodeRadius = 6.0f;
constexpr float kCodePad = 8.0f;
constexpr float kInlineCodePad = 3.0f;
constexpr float kInlineCodeRadius = 4.0f;
constexpr float kTableFirstMin = 160.0f;
constexpr float kTableFirstFrac = 0.35f;
/// The key column never takes more than this share of a narrow view.
constexpr float kTableFirstMaxFrac = 0.6f;
constexpr float kCellPadX = 8.0f;
constexpr float kCellPadY = 6.0f;
constexpr float kSpacerHeight = 8.0f;
constexpr float kRuleHeight = 1.0f;
/// Width assumed when measured without any bound and no cap.
constexpr float kUnboundedWidth = 640.0f;
/// Layout treats anything larger than this as unbounded.
constexpr float kUnbounded = 1e8f;
/// Memoised word widths are dropped past this many entries.
constexpr size_t kWordMemoCap = 8192;

/**
 * @brief Vertical margins around a block kind.
 */
struct Margins {
    float top = 0.0f;
    float bottom = 0.0f;
};

/**
 * @brief Margins per block kind.
 */
Margins marginsFor(RichBlock::Kind kind) {
    using Kind = RichBlock::Kind;
    switch (kind) {
    case Kind::H1:          return {8.0f, 6.0f};
    case Kind::H2:          return {18.0f, 6.0f};
    case Kind::H3:          return {12.0f, 4.0f};
    case Kind::Paragraph:   return {0.0f, 8.0f};
    case Kind::Bullet:      return {0.0f, 4.0f};
    case Kind::Step:        return {0.0f, 4.0f};
    case Kind::Callout:     return {4.0f, 10.0f};
    case Kind::Code:        return {4.0f, 10.0f};
    case Kind::TableHeader: return {0.0f, 0.0f};
    case Kind::TableRow:    return {0.0f, 0.0f};
    case Kind::Rule:        return {8.0f, 8.0f};
    case Kind::Spacer:      return {0.0f, 0.0f};
    }
    return {};
}

/**
 * @brief Heading style for H1/H2/H3 (wrapping).
 */
TextStyle headingStyle(RichBlock::Kind kind) {
    TextStyle s;
    switch (kind) {
    case RichBlock::Kind::H1: s = typography::largeTitle(); break;
    case RichBlock::Kind::H2: s = typography::title(); break;
    default:                  s = typography::headline(); break;
    }
    s.wrap = true;
    return s;
}

/**
 * @brief Theme colour for a callout tone.
 */
Color toneColor(const Theme& t, RichBlock::Tone tone) {
    switch (tone) {
    case RichBlock::Tone::Info:    return t.info;
    case RichBlock::Tone::Success: return t.success;
    case RichBlock::Tone::Warning: return t.warning;
    case RichBlock::Tone::Error:   return t.destructive;
    }
    return t.info;
}

/**
 * @brief Concatenated text of a block's runs.
 */
std::wstring runsText(const std::vector<RichRun>& runs) {
    std::wstring out;
    for (const RichRun& r : runs) {
        out += r.text;
    }
    return out;
}

/**
 * @brief Strips spaces and tabs from both ends.
 */
std::wstring_view trimView(std::wstring_view s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) {
        s.remove_suffix(1);
    }
    return s;
}

/**
 * @brief Appends runs, merging into the previous run when the style matches.
 */
void appendRuns(std::vector<RichRun>& target, const std::vector<RichRun>& more) {
    for (const RichRun& r : more) {
        if (r.text.empty()) {
            continue;
        }
        if (!target.empty() && target.back().bold == r.bold && target.back().code == r.code) {
            target.back().text += r.text;
        } else {
            target.push_back(r);
        }
    }
}

// ---- measurement ------------------------------------------------------------

/**
 * @brief Measures through the frame's canvas when painting, else the shared cache.
 */
Size measureVia(Canvas* c, std::wstring_view text, const TextStyle& style, float maxWidth, int maxLines) {
    if (c) {
        return c->measureText(text, style, maxWidth, maxLines);
    }
    return measureTextShared(text, style, maxWidth, maxLines);
}

/**
 * @brief Line height for a style from the real font metrics (or the estimate).
 */
float lineHeightOf(const TextStyle& style) {
    const TextCache::LineMetrics lm = lineMetricsShared(style);
    return std::max(lm.lineHeight, 1.3f * std::max(1.0f, style.size));
}

/**
 * @brief Process-wide memo of word advances per style.
 *
 * Only real measurements are memoised (never the estimates used before the
 * window registers its cache), and the memo is emptied once it grows past
 * its cap so a pathological document cannot hoard memory.
 */
struct WordMemo {
    std::unordered_map<uint64_t, std::unordered_map<std::wstring, float>> byStyle;
    size_t count = 0;
};

/**
 * @brief The one memo.
 */
WordMemo& wordMemo() {
    static WordMemo memo;
    return memo;
}

/**
 * @brief Advance of a token (including its trailing spaces) in a style.
 */
float tokenWidth(Canvas* c, const std::wstring& token, const TextStyle& style) {
    if (token.empty()) {
        return 0.0f;
    }
    const bool real = (c != nullptr) || (sharedTextCache() != nullptr);
    WordMemo& memo = wordMemo();
    if (real) {
        auto styleIt = memo.byStyle.find(style.key());
        if (styleIt != memo.byStyle.end()) {
            auto wordIt = styleIt->second.find(token);
            if (wordIt != styleIt->second.end()) {
                return wordIt->second;
            }
        }
    }

    const float width = std::max(0.0f, measureVia(c, token, style, 0.0f, 1).w);
    if (real) {
        if (memo.count >= kWordMemoCap) {
            memo.byStyle.clear();
            memo.count = 0;
        }
        memo.byStyle[style.key()][token] = width;
        ++memo.count;
    }
    return width;
}

// ---- inline wrapping --------------------------------------------------------

/**
 * @brief One drawable stretch of a line: part of a run at an x position.
 */
struct Piece {
    std::wstring text;
    TextStyle style;
    bool code = false;
    size_t run = 0;
    float x = 0.0f;          ///< left edge (of the pill for code)
    float w = 0.0f;          ///< advance including trailing spaces
    float wVisible = 0.0f;   ///< advance without trailing spaces
};

/**
 * @brief One wrapped line.
 */
struct Line {
    std::vector<Piece> pieces;
    float height = 0.0f;
};

/**
 * @brief The wrapped form of a block's runs.
 */
struct Wrapped {
    std::vector<Line> lines;
    float height = 0.0f;
};

/**
 * @brief Styles a run may take for a base body style.
 */
struct RunStyles {
    TextStyle normal;
    TextStyle bold;
    TextStyle code;
};

/**
 * @brief Derives the bold and code styles from a base style.
 */
RunStyles stylesFor(const TextStyle& base) {
    RunStyles s;
    s.normal = base;
    s.normal.wrap = false;
    s.bold = s.normal;
    s.bold.weight = FontWeight::SemiBold;
    s.code = typography::mono();
    return s;
}

/**
 * @brief Greedy word wrap across runs.
 *
 * Tokens are words with their trailing spaces attached. A token moves to the
 * next line when its visible part (without those spaces) no longer fits;
 * tokens of the same run on the same line merge into one piece so drawing
 * stays cheap. Code runs reserve pill padding on both sides.
 */
Wrapped wrapRuns(const std::vector<RichRun>& runs, float width, const TextStyle& base, Canvas* c) {
    Wrapped out;
    out.lines.emplace_back();
    width = std::max(1.0f, width);
    const RunStyles st = stylesFor(base);
    float x = 0.0f;

    for (size_t ri = 0; ri < runs.size(); ++ri) {
        const RichRun& run = runs[ri];
        const TextStyle& style = run.code ? st.code : (run.bold ? st.bold : st.normal);
        const float lineH = lineHeightOf(style);
        const float pad = run.code ? kInlineCodePad : 0.0f;
        const float spaceW = tokenWidth(c, L" ", style);

        // Newlines and tabs inside a run behave like spaces.
        std::wstring text = run.text;
        for (wchar_t& ch : text) {
            if (ch == L'\n' || ch == L'\r' || ch == L'\t') {
                ch = L' ';
            }
        }

        size_t i = 0;
        const size_t n = text.size();
        while (i < n) {
            // word, then its trailing spaces
            size_t j = i;
            while (j < n && text[j] != L' ') {
                ++j;
            }
            size_t k = j;
            while (k < n && text[k] == L' ') {
                ++k;
            }
            const bool pureSpace = (j == i);
            const size_t trailing = k - j;
            std::wstring token = text.substr(i, k - i);
            i = k;

            const float wIncl = tokenWidth(c, token, style);
            const float wVisible = std::max(0.0f, wIncl - static_cast<float>(trailing) * spaceW);

            Line* line = &out.lines.back();
            bool lineEmpty = line->pieces.empty();
            // Spaces never open a line.
            if (lineEmpty && pureSpace) {
                continue;
            }
            const bool continuesPiece = !lineEmpty && line->pieces.back().run == ri;
            const float cursor = continuesPiece ? x - pad : x;
            const float extra = continuesPiece ? 0.0f : pad * 2.0f;

            // Break when the visible part would overflow a non-empty line.
            if (!lineEmpty && cursor + extra + wVisible > width + 0.01f) {
                if (pureSpace) {
                    continue;   // trailing spaces at a line end vanish
                }
                out.lines.emplace_back();
                line = &out.lines.back();
                x = 0.0f;
                lineEmpty = true;
            }

            if (!lineEmpty && line->pieces.back().run == ri) {
                // Same run, same line: extend the piece.
                Piece& p = line->pieces.back();
                p.wVisible = p.w + wVisible;
                p.w += wIncl;
                p.text += token;
                x = p.x + pad + p.w + pad;
            } else {
                Piece p;
                p.text = std::move(token);
                p.style = style;
                p.code = run.code;
                p.run = ri;
                p.x = x;
                p.w = wIncl;
                p.wVisible = wVisible;
                x = p.x + pad + p.w + pad;
                line->pieces.push_back(std::move(p));
            }
            line->height = std::max(line->height, lineH);
        }
    }

    // Empty lines (an empty paragraph, or nothing but spaces) still take a line.
    const float baseLineH = lineHeightOf(st.normal);
    for (Line& line : out.lines) {
        if (line.height <= 0.0f) {
            line.height = baseLineH;
        }
        out.height += line.height;
    }
    return out;
}

/**
 * @brief Draws wrapped runs at an origin; code pieces get a pill behind them.
 */
void paintWrapped(Canvas& c, const Wrapped& w, float x0, float y0, float width, const Color& textColor,
                  const Color& pillFill) {
    float y = y0;
    for (const Line& line : w.lines) {
        for (const Piece& piece : line.pieces) {
            const float pad = piece.code ? kInlineCodePad : 0.0f;
            if (piece.code && piece.wVisible > 0.0f) {
                // Pill centred on the line, one dip taller than the mono line.
                const float lh = lineHeightOf(piece.style);
                const Rect pill{x0 + piece.x, y + (line.height - lh) * 0.5f - 1.0f, piece.wVisible + pad * 2.0f, lh + 2.0f};
                c.fillRoundedRect(c.scale().snap(pill), kInlineCodeRadius, pillFill);
            }
            // The box is generous so the canvas clip never cuts a fitted piece.
            const float tx = x0 + piece.x + pad;
            const float avail = std::max(piece.w + 2.0f, x0 + width - tx);
            c.drawText(piece.text, piece.style, {tx, y, avail, line.height}, textColor, HAlign::Left, VAlign::Center,
                       Trimming::None, 1);
        }
        y += line.height;
    }
}

// ---- tables ------------------------------------------------------------------

/**
 * @brief Column widths for a row with @p n cells across @p width.
 */
std::vector<float> tableColumns(size_t n, float width) {
    width = std::max(0.0f, width);
    if (n <= 1) {
        return {width};
    }
    const float first = std::clamp(std::max(kTableFirstMin, width * kTableFirstFrac), 0.0f, width * kTableFirstMaxFrac);
    const float rest = std::max(0.0f, (width - first) / static_cast<float>(n - 1));
    std::vector<float> cols(n, rest);
    cols[0] = first;
    return cols;
}

/**
 * @brief Height of a table row: the tallest wrapped cell plus padding.
 */
float tableRowHeight(const RichBlock& b, float width, Canvas* c) {
    const bool header = b.kind == RichBlock::Kind::TableHeader;
    const std::vector<float> cols = tableColumns(b.cells.size(), width);
    TextStyle headerStyle = typography::captionStrong();
    headerStyle.wrap = true;

    float h = 0.0f;
    for (size_t i = 0; i < b.cells.size() && i < cols.size(); ++i) {
        const float cellW = std::max(1.0f, cols[i] - kCellPadX * 2.0f);
        float cellH = 0.0f;
        if (header) {
            cellH = b.cells[i].empty() ? lineHeightOf(headerStyle) : measureVia(c, b.cells[i], headerStyle, cellW, 0).h;
        } else {
            cellH = wrapRuns(RichTextView::parseInline(b.cells[i]), cellW, typography::body(), c).height;
        }
        h = std::max(h, cellH);
    }
    if (h <= 0.0f) {
        h = lineHeightOf(header ? headerStyle : typography::body());
    }
    return h + kCellPadY * 2.0f;
}

// ---- markdown helpers ----------------------------------------------------------

/**
 * @brief True for a horizontal rule line: three or more of - * _ (spaces allowed).
 */
bool isRuleLine(std::wstring_view s) {
    wchar_t mark = 0;
    size_t count = 0;
    for (wchar_t ch : s) {
        if (ch == L' ') {
            continue;
        }
        if (ch != L'-' && ch != L'*' && ch != L'_') {
            return false;
        }
        if (mark == 0) {
            mark = ch;
        } else if (ch != mark) {
            return false;
        }
        ++count;
    }
    return count >= 3;
}

/**
 * @brief Splits a "| a | b |" line into trimmed cells.
 */
std::vector<std::wstring> splitCells(std::wstring_view line) {
    std::vector<std::wstring> cells;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t bar = line.find(L'|', start);
        const std::wstring_view part = (bar == std::wstring_view::npos) ? line.substr(start) : line.substr(start, bar - start);
        cells.emplace_back(trimView(part));
        if (bar == std::wstring_view::npos) {
            break;
        }
        start = bar + 1;
    }
    // Leading and trailing pipes produce empty edge cells.
    if (!cells.empty() && cells.front().empty()) {
        cells.erase(cells.begin());
    }
    if (!cells.empty() && cells.back().empty()) {
        cells.pop_back();
    }
    return cells;
}

/**
 * @brief True for the "---|:--:|---" line under a table header.
 */
bool isSeparatorRow(const std::vector<std::wstring>& cells) {
    if (cells.empty()) {
        return false;
    }
    for (const std::wstring& cell : cells) {
        bool dash = false;
        for (wchar_t ch : cell) {
            if (ch == L'-') {
                dash = true;
            } else if (ch != L':' && ch != L' ') {
                return false;
            }
        }
        if (!dash) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Reads a "Note:" / "Warning:" style prefix off a callout line.
 * @param text  the callout text; the prefix is removed when recognised
 */
RichBlock::Tone takeCalloutTone(std::wstring_view& text) {
    const size_t colon = text.find(L':');
    if (colon == std::wstring_view::npos || colon == 0 || colon > 12) {
        return RichBlock::Tone::Info;
    }
    std::wstring word;
    for (size_t i = 0; i < colon; ++i) {
        word.push_back(static_cast<wchar_t>(std::towlower(text[i])));
    }

    RichBlock::Tone tone = RichBlock::Tone::Info;
    bool known = true;
    if (word == L"note" || word == L"info") {
        tone = RichBlock::Tone::Info;
    } else if (word == L"tip" || word == L"success") {
        tone = RichBlock::Tone::Success;
    } else if (word == L"warning" || word == L"caution" || word == L"important") {
        tone = RichBlock::Tone::Warning;
    } else if (word == L"error" || word == L"danger") {
        tone = RichBlock::Tone::Error;
    } else {
        known = false;
    }
    if (known) {
        text = trimView(text.substr(colon + 1));
    }
    return tone;
}

} // namespace

// ===========================================================================
// content
// ===========================================================================

/**
 * @brief Replaces the blocks and forces a fresh layout.
 */
void RichTextView::setBlocks(std::vector<RichBlock> blocks) {
    blocks_ = std::move(blocks);
    laid_.clear();
    measuredWidth_ = 0.0f;
    totalHeight_ = 0.0f;
    invalidateLayout();
}

/**
 * @brief Parses markdown and shows it.
 */
void RichTextView::setMarkdown(std::wstring_view markdown) {
    setBlocks(parseMarkdown(markdown));
}

/**
 * @brief Plain-text rendering of the blocks (for "Copy summary").
 */
std::wstring RichTextView::plainText() const {
    using Kind = RichBlock::Kind;
    std::wstring out;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const RichBlock& b = blocks_[i];
        const std::wstring text = runsText(b.runs);
        switch (b.kind) {
        case Kind::H1:
        case Kind::H2:
        case Kind::H3:
        case Kind::Paragraph:
        case Kind::Callout:
        case Kind::Code:
            out += text;
            out += L"\n\n";
            break;
        case Kind::Bullet:
            out += L"- ";
            out += text;
            out += L'\n';
            break;
        case Kind::Step:
            out += std::to_wstring(b.number);
            out += L". ";
            out += text;
            out += L'\n';
            break;
        case Kind::TableHeader:
        case Kind::TableRow:
            // Two columns read as "key: value"; wider tables keep their pipes.
            if (b.cells.size() == 2) {
                out += b.cells[0];
                out += L": ";
                out += b.cells[1];
            } else {
                for (size_t ci = 0; ci < b.cells.size(); ++ci) {
                    if (ci > 0) {
                        out += L" | ";
                    }
                    out += b.cells[ci];
                }
            }
            out += L'\n';
            break;
        case Kind::Rule:
            out += L"---\n";
            break;
        case Kind::Spacer:
            out += L'\n';
            break;
        }

        // A blank line after a list or table once it ends.
        const bool listLike = b.kind == Kind::Bullet || b.kind == Kind::Step || b.kind == Kind::TableHeader || b.kind == Kind::TableRow;
        if (listLike) {
            const bool last = (i + 1 >= blocks_.size());
            const bool sameNext = !last && blocks_[i + 1].kind == b.kind;
            const bool tableNext = !last && (b.kind == Kind::TableHeader && blocks_[i + 1].kind == Kind::TableRow);
            if (last || (!sameNext && !tableNext)) {
                out += L'\n';
            }
        }
    }
    return out;
}

// ===========================================================================
// parsing
// ===========================================================================

/**
 * @brief Parses the markdown subset into blocks.
 *
 * Paragraph lines without a blank line between them merge; so do consecutive
 * "> " lines. Two blank lines in a row insert a Spacer. Table rows are
 * normalised to the header's column count.
 */
std::vector<RichBlock> RichTextView::parseMarkdown(std::wstring_view markdown) {
    using Kind = RichBlock::Kind;
    enum class Prev { None, Paragraph, Callout, Table };

    std::vector<RichBlock> out;
    Prev prev = Prev::None;
    bool inCode = false;
    std::wstring code;
    size_t tableCols = 0;
    int blankRun = 0;

    const size_t n = markdown.size();
    size_t pos = 0;
    while (pos <= n) {
        // Next line (the final line may lack a newline).
        const size_t nl = markdown.find(L'\n', pos);
        std::wstring_view line = (nl == std::wstring_view::npos) ? markdown.substr(pos) : markdown.substr(pos, nl - pos);
        pos = (nl == std::wstring_view::npos) ? n + 1 : nl + 1;
        if (!line.empty() && line.back() == L'\r') {
            line.remove_suffix(1);
        }
        const std::wstring_view trimmed = trimView(line);

        // Fenced code: everything between the fences is verbatim.
        if (trimmed.starts_with(L"```")) {
            if (inCode) {
                RichBlock b;
                b.kind = Kind::Code;
                if (!code.empty() && code.back() == L'\n') {
                    code.pop_back();
                }
                b.runs.push_back(RichRun{code, false, true});
                out.push_back(std::move(b));
                code.clear();
                inCode = false;
            } else {
                inCode = true;
                code.clear();
            }
            prev = Prev::None;
            blankRun = 0;
            continue;
        }
        if (inCode) {
            code.append(line);
            code.push_back(L'\n');
            continue;
        }

        // Blank lines end paragraphs/callouts/tables; two of them add space.
        if (trimmed.empty()) {
            ++blankRun;
            if (blankRun == 2) {
                RichBlock b;
                b.kind = Kind::Spacer;
                out.push_back(std::move(b));
            }
            prev = Prev::None;
            continue;
        }
        blankRun = 0;

        // Headings: one to three hashes and a space.
        size_t hashes = 0;
        while (hashes < trimmed.size() && trimmed[hashes] == L'#') {
            ++hashes;
        }
        if (hashes >= 1 && hashes <= 3 && hashes < trimmed.size() && trimmed[hashes] == L' ') {
            RichBlock b;
            b.kind = (hashes == 1) ? Kind::H1 : (hashes == 2) ? Kind::H2 : Kind::H3;
            b.runs = parseInline(trimView(trimmed.substr(hashes + 1)));
            out.push_back(std::move(b));
            prev = Prev::None;
            continue;
        }

        // Horizontal rule.
        if (isRuleLine(trimmed)) {
            RichBlock b;
            b.kind = Kind::Rule;
            out.push_back(std::move(b));
            prev = Prev::None;
            continue;
        }

        // Bullet.
        if (trimmed.size() >= 2 && (trimmed[0] == L'-' || trimmed[0] == L'*' || trimmed[0] == L'•') && trimmed[1] == L' ') {
            RichBlock b;
            b.kind = Kind::Bullet;
            b.runs = parseInline(trimView(trimmed.substr(2)));
            out.push_back(std::move(b));
            prev = Prev::None;
            continue;
        }

        // Numbered step: "3. text" or "3) text".
        {
            size_t digits = 0;
            while (digits < trimmed.size() && digits < 9 && std::iswdigit(trimmed[digits])) {
                ++digits;
            }
            if (digits > 0 && digits + 1 < trimmed.size() && (trimmed[digits] == L'.' || trimmed[digits] == L')') &&
                trimmed[digits + 1] == L' ') {
                int number = 0;
                for (size_t d = 0; d < digits; ++d) {
                    number = number * 10 + static_cast<int>(trimmed[d] - L'0');
                }
                RichBlock b;
                b.kind = Kind::Step;
                b.number = number;
                b.runs = parseInline(trimView(trimmed.substr(digits + 2)));
                out.push_back(std::move(b));
                prev = Prev::None;
                continue;
            }
        }

        // Callout: consecutive "> " lines merge into one block.
        if (trimmed[0] == L'>') {
            std::wstring_view rest = trimView(trimmed.substr(1));
            if (prev == Prev::Callout && !out.empty() && out.back().kind == Kind::Callout) {
                std::vector<RichRun> more;
                more.push_back(RichRun{L" ", false, false});
                appendRuns(more, parseInline(rest));
                appendRuns(out.back().runs, more);
            } else {
                RichBlock b;
                b.kind = Kind::Callout;
                b.tone = takeCalloutTone(rest);
                b.runs = parseInline(rest);
                out.push_back(std::move(b));
            }
            prev = Prev::Callout;
            continue;
        }

        // Table: the first row of a run is the header; the dashed line is skipped.
        if (trimmed[0] == L'|') {
            std::vector<std::wstring> cells = splitCells(trimmed);
            if (isSeparatorRow(cells)) {
                continue;
            }
            RichBlock b;
            if (prev != Prev::Table) {
                b.kind = Kind::TableHeader;
                tableCols = cells.size();
            } else {
                b.kind = Kind::TableRow;
                if (tableCols > 0) {
                    cells.resize(tableCols);
                }
            }
            b.cells = std::move(cells);
            out.push_back(std::move(b));
            prev = Prev::Table;
            continue;
        }

        // Paragraph (soft-wrapped continuation lines join with a space).
        std::vector<RichRun> runs = parseInline(trimmed);
        if (prev == Prev::Paragraph && !out.empty() && out.back().kind == Kind::Paragraph) {
            std::vector<RichRun> more;
            more.push_back(RichRun{L" ", false, false});
            appendRuns(more, runs);
            appendRuns(out.back().runs, more);
        } else {
            RichBlock b;
            b.kind = Kind::Paragraph;
            b.runs = std::move(runs);
            out.push_back(std::move(b));
        }
        prev = Prev::Paragraph;
    }

    // An unterminated fence still shows what it had.
    if (inCode && !code.empty()) {
        HH_LOG_DEBUG(kLog, L"parseMarkdown: unterminated code fence");
        RichBlock b;
        b.kind = Kind::Code;
        if (code.back() == L'\n') {
            code.pop_back();
        }
        b.runs.push_back(RichRun{code, false, true});
        out.push_back(std::move(b));
    }
    return out;
}

/**
 * @brief Splits a line into runs: **bold** and `code` (code wins inside backticks).
 *
 * Unmatched markers are kept as literal text.
 */
std::vector<RichRun> RichTextView::parseInline(std::wstring_view line) {
    std::vector<RichRun> runs;
    std::wstring current;
    bool bold = false;
    bool code = false;

    const auto flush = [&] {
        if (!current.empty()) {
            runs.push_back(RichRun{current, bold, code});
            current.clear();
        }
    };

    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        const wchar_t ch = line[i];
        if (code) {
            // Inside code only the closing backtick is special.
            if (ch == L'`') {
                flush();
                code = false;
                ++i;
                continue;
            }
            current.push_back(ch);
            ++i;
            continue;
        }
        if (ch == L'`') {
            // Only open when there is a closing backtick somewhere ahead.
            if (line.find(L'`', i + 1) != std::wstring_view::npos) {
                flush();
                code = true;
                ++i;
                continue;
            }
        } else if (ch == L'*' && i + 1 < n && line[i + 1] == L'*') {
            // Toggle bold; an opener needs a closer to count.
            if (bold || line.find(L"**", i + 2) != std::wstring_view::npos) {
                flush();
                bold = !bold;
                i += 2;
                continue;
            }
        }
        current.push_back(ch);
        ++i;
    }
    flush();
    return runs;
}

// ===========================================================================
// layout
// ===========================================================================

/**
 * @brief Stacks the blocks for @p width and caches their rects.
 */
void RichTextView::layoutBlocks(float width) {
    width = std::max(0.0f, width);
    laid_.clear();
    laid_.reserve(blocks_.size());

    float y = 0.0f;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const RichBlock& b = blocks_[i];
        const Margins m = marginsFor(b.kind);
        // The first block sits flush with the top.
        const float top = (i == 0) ? 0.0f : m.top;
        const float h = std::max(0.0f, blockHeight(b, width, nullptr));
        laid_.push_back(Laid{Rect{0.0f, y + top, width, h}});
        y += top + h + m.bottom;
    }
    // ... and the last one flush with the bottom.
    if (!blocks_.empty()) {
        y -= marginsFor(blocks_.back().kind).bottom;
    }
    totalHeight_ = std::max(0.0f, y);
    measuredWidth_ = width;
}

/**
 * @brief Height of one block at @p width, measured through @p c when painting.
 */
float RichTextView::blockHeight(const RichBlock& b, float width, Canvas* c) {
    using Kind = RichBlock::Kind;
    width = std::max(1.0f, width);
    switch (b.kind) {
    case Kind::H1:
    case Kind::H2:
    case Kind::H3: {
        const TextStyle style = headingStyle(b.kind);
        const std::wstring text = runsText(b.runs);
        if (text.empty()) {
            return lineHeightOf(style);
        }
        return std::max(0.0f, measureVia(c, text, style, width, 0).h);
    }
    case Kind::Paragraph:
        return wrapRuns(b.runs, width, typography::body(), c).height;
    case Kind::Bullet:
        return wrapRuns(b.runs, width - kBulletIndent, typography::body(), c).height;
    case Kind::Step:
        return wrapRuns(b.runs, width - kStepIndent, typography::body(), c).height;
    case Kind::Callout: {
        const float textW = width - (kCalloutBarInset + kCalloutBar + kCalloutPad * 2.0f);
        return wrapRuns(b.runs, textW, typography::body(), c).height + kCalloutPad * 2.0f;
    }
    case Kind::Code: {
        TextStyle mono = typography::mono();
        mono.wrap = true;
        const std::wstring text = runsText(b.runs);
        const float textH = text.empty() ? lineHeightOf(mono)
                                         : std::max(0.0f, measureVia(c, text, mono, width - kCodePad * 2.0f, 0).h);
        return textH + kCodePad * 2.0f;
    }
    case Kind::TableHeader:
    case Kind::TableRow:
        return tableRowHeight(b, width, c);
    case Kind::Rule:
        return kRuleHeight;
    case Kind::Spacer:
        return kSpacerHeight;
    }
    return 0.0f;
}

/**
 * @brief Lays the blocks out for the available width (capped by maxWidth_).
 */
Size RichTextView::measure(const Constraints& c) {
    const bool bounded = c.maxW < kUnbounded;
    float width = bounded ? std::max(0.0f, c.maxW) : (maxWidth_ > 0.0f ? maxWidth_ : kUnboundedWidth);
    if (maxWidth_ > 0.0f) {
        width = std::min(width, maxWidth_);
    }
    if (width != measuredWidth_ || laid_.size() != blocks_.size()) {
        layoutBlocks(width);
    }
    return c.constrain({width, totalHeight_});
}

/**
 * @brief Re-lays the blocks when the parent handed over a different width.
 */
void RichTextView::onLayout() {
    float width = std::max(0.0f, frame_.w);
    if (maxWidth_ > 0.0f) {
        width = std::min(width, maxWidth_);
    }
    if (width != measuredWidth_ || laid_.size() != blocks_.size()) {
        layoutBlocks(width);
    }
}

// ===========================================================================
// painting
// ===========================================================================

/**
 * @brief Draws every block at its cached rect.
 */
void RichTextView::paintSelf(Canvas& c) {
    if (blocks_.empty()) {
        return;
    }
    // Defensive: never index laid_ past what layoutBlocks produced.
    if (laid_.size() != blocks_.size()) {
        layoutBlocks(measuredWidth_ > 0.0f ? measuredWidth_ : std::max(0.0f, frame_.w));
        if (laid_.size() != blocks_.size()) {
            return;
        }
    }
    using Kind = RichBlock::Kind;
    const Theme& t = c.theme();
    const TextStyle body = typography::body();

    for (size_t i = 0; i < blocks_.size(); ++i) {
        const RichBlock& b = blocks_[i];
        const Rect r = laid_[i].rect;
        if (r.w <= 0.0f) {
            continue;
        }

        switch (b.kind) {
        case Kind::H1:
        case Kind::H2:
        case Kind::H3: {
            const std::wstring text = runsText(b.runs);
            if (!text.empty()) {
                c.drawText(text, headingStyle(b.kind), r, t.labelPrimary, HAlign::Left, VAlign::Top, Trimming::None, 0);
            }
            break;
        }
        case Kind::Paragraph: {
            const Wrapped w = wrapRuns(b.runs, r.w, body, &c);
            paintWrapped(c, w, r.x, r.y, r.w, t.labelPrimary, t.fillTertiary);
            break;
        }
        case Kind::Bullet: {
            const Wrapped w = wrapRuns(b.runs, r.w - kBulletIndent, body, &c);
            // Dot centred on the first line.
            const float firstH = w.lines.empty() ? lineHeightOf(body) : w.lines.front().height;
            c.fillCircle({r.x + kBulletIndent * 0.5f - 2.0f, r.y + firstH * 0.5f}, kBulletDotRadius, t.labelSecondary);
            paintWrapped(c, w, r.x + kBulletIndent, r.y, r.w - kBulletIndent, t.labelPrimary, t.fillTertiary);
            break;
        }
        case Kind::Step: {
            const Wrapped w = wrapRuns(b.runs, r.w - kStepIndent, body, &c);
            const float firstH = w.lines.empty() ? lineHeightOf(body) : w.lines.front().height;
            const std::wstring number = std::to_wstring(b.number) + L".";
            c.drawText(number, typography::bodyStrong(), {r.x, r.y, kStepIndent - 2.0f, firstH}, t.accent, HAlign::Left,
                       VAlign::Center, Trimming::None, 1);
            paintWrapped(c, w, r.x + kStepIndent, r.y, r.w - kStepIndent, t.labelPrimary, t.fillTertiary);
            break;
        }
        case Kind::Callout: {
            const Color tone = toneColor(t, b.tone);
            const Rect box = c.scale().snap(r);
            c.fillRoundedRect(box, kCalloutRadius, tone.withAlpha(kCalloutTint));
            // Tone bar inside the padding so it never pokes past the rounded corners.
            const Rect bar{box.x + kCalloutBarInset, box.y + kCalloutPad, kCalloutBar, std::max(0.0f, box.h - kCalloutPad * 2.0f)};
            c.fillRoundedRect(bar, kCalloutBar * 0.5f, tone);
            const float textX = r.x + kCalloutBarInset + kCalloutBar + kCalloutPad;
            const float textW = r.w - (kCalloutBarInset + kCalloutBar + kCalloutPad * 2.0f);
            const Wrapped w = wrapRuns(b.runs, textW, body, &c);
            paintWrapped(c, w, textX, r.y + kCalloutPad, textW, t.labelPrimary, t.fillTertiary);
            break;
        }
        case Kind::Code: {
            TextStyle mono = typography::mono();
            mono.wrap = true;
            c.fillRoundedRect(c.scale().snap(r), kCodeRadius, t.fillTertiary);
            const std::wstring text = runsText(b.runs);
            const Rect inner = r.inset(kCodePad);
            if (!text.empty() && !inner.isEmpty()) {
                c.drawText(text, mono, inner, t.labelPrimary, HAlign::Left, VAlign::Top, Trimming::None, 0);
            }
            break;
        }
        case Kind::TableHeader:
        case Kind::TableRow: {
            const bool header = b.kind == Kind::TableHeader;
            const std::vector<float> cols = tableColumns(b.cells.size(), r.w);
            TextStyle headerStyle = typography::captionStrong();
            headerStyle.wrap = true;
            float x = r.x;
            for (size_t ci = 0; ci < b.cells.size() && ci < cols.size(); ++ci) {
                const float cellW = std::max(0.0f, cols[ci] - kCellPadX * 2.0f);
                const Rect cell{x + kCellPadX, r.y + kCellPadY, cellW, std::max(0.0f, r.h - kCellPadY * 2.0f)};
                if (header) {
                    if (!b.cells[ci].empty() && !cell.isEmpty()) {
                        c.drawText(b.cells[ci], headerStyle, cell, t.labelSecondary, HAlign::Left, VAlign::Top, Trimming::None, 0);
                    }
                } else {
                    const Wrapped w = wrapRuns(parseInline(b.cells[ci]), cellW, body, &c);
                    paintWrapped(c, w, cell.x, cell.y, cellW, t.labelPrimary, t.fillTertiary);
                }
                x += cols[ci];
            }
            // Divider under the row (stronger under the header).
            c.drawHairline({r.x, r.bottom()}, {r.right(), r.bottom()}, header ? t.separatorStrong : t.separator);
            break;
        }
        case Kind::Rule: {
            const float y = r.y + r.h * 0.5f;
            c.drawHairline({r.x, y}, {r.right(), y}, t.separator);
            break;
        }
        case Kind::Spacer:
            break;
        }
    }
}

} // namespace hh::ui
