// ---------------------------------------------------------------------------
// TextField.cpp - single-line text input.
//
// The text is a UTF-16 string with a caret and a selection anchor. Hit
// testing and caret placement go through the same cached IDWriteTextLayout
// that paints the text, so what the user clicks is what the user gets. When
// no DirectWrite cache is registered (tests, early measurement) the control
// falls back to proportional estimates instead of failing.
// ---------------------------------------------------------------------------
#include "ui/controls/TextField.h"

#include "core/Logger.h"
#include "platform/Time.h"
#include "ui/core/RootView.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <vector>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"TextField";

// Control metrics (dips).
constexpr float kHeight = 28.0f;
constexpr float kRadius = 6.0f;
constexpr float kPadX = 8.0f;
constexpr float kDefaultWidth = 160.0f;

// The clip is a touch wider than the text rect so a caret at either edge is not cut.
constexpr float kClipSlack = 2.0f;

// Undo history depth.
constexpr size_t kUndoDepth = 64;

// Selection tints: accent while focused in an active window, neutral otherwise.
constexpr float kSelectionAlphaFocused = 0.30f;
constexpr float kSelectionAlphaBlurred = 0.12f;

// Default caret blink half-period when the system value is unavailable.
constexpr double kDefaultBlink = 0.53;

/**
 * @brief Picks the theme to paint with: the root's when attached, else the canvas's.
 */
const Theme& paintTheme(const Widget& w, const Canvas& c) {
    const Theme* t = w.theme();
    return t ? *t : c.theme();
}

/**
 * @brief True for a UTF-16 high surrogate.
 */
bool isHighSurrogate(wchar_t ch) {
    return ch >= 0xD800 && ch <= 0xDBFF;
}

/**
 * @brief True for a UTF-16 low surrogate.
 */
bool isLowSurrogate(wchar_t ch) {
    return ch >= 0xDC00 && ch <= 0xDFFF;
}

/**
 * @brief Letter/digit test on a UTF-16 unit (surrogates count as punctuation).
 */
bool isWordChar(wchar_t ch) {
    return std::iswalnum(static_cast<wint_t>(ch)) != 0;
}

/**
 * @brief Whitespace test on a UTF-16 unit.
 */
bool isSpaceChar(wchar_t ch) {
    return std::iswspace(static_cast<wint_t>(ch)) != 0;
}

/**
 * @brief Fetches the shared cache only when it can actually build layouts.
 */
TextCache* usableCache() {
    TextCache* cache = sharedTextCache();
    return (cache && cache->factory()) ? cache : nullptr;
}

/**
 * @brief Average advance for the proportional fallback (never zero).
 */
float estimatedAdvance(const std::wstring& text, const TextStyle& style) {
    if (text.empty()) {
        return std::max(1.0f, 0.55f * style.size);
    }
    const float w = measureTextShared(text, style).w;
    return std::max(1.0f, w / static_cast<float>(text.size()));
}

/**
 * @brief X of a caret position inside a layout (0 on failure).
 */
float layoutCaretX(IDWriteTextLayout* layout, size_t pos) {
    if (!layout) {
        return 0.0f;
    }
    FLOAT x = 0.0f;
    FLOAT y = 0.0f;
    DWRITE_HIT_TEST_METRICS m{};
    const HRESULT hr = layout->HitTestTextPosition(static_cast<UINT32>(pos), FALSE, &x, &y, &m);
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"HitTestTextPosition({}) failed: 0x{:08X}", pos, static_cast<unsigned>(hr));
        return 0.0f;
    }
    return x;
}

/**
 * @brief Expands @p pos to the word (or run of non-word characters) around it.
 */
void wordRangeAt(const std::wstring& text, size_t pos, size_t& start, size_t& end) {
    const size_t n = text.size();
    pos = std::min(pos, n);
    start = pos;
    end = pos;
    if (n == 0) {
        return;
    }

    // Decide which class of character we are sitting on (prefer the one to the left at the end).
    const size_t probe = (pos < n) ? pos : pos - 1;
    const bool word = isWordChar(text[probe]);
    const bool space = !word && isSpaceChar(text[probe]);
    auto sameClass = [&](wchar_t ch) {
        if (word) {
            return isWordChar(ch);
        }
        if (space) {
            return isSpaceChar(ch);
        }
        return !isWordChar(ch) && !isSpaceChar(ch);
    };

    // Grow left and right while the class matches.
    start = probe;
    while (start > 0 && sameClass(text[start - 1])) {
        --start;
    }
    end = probe;
    while (end < n && sameClass(text[end])) {
        ++end;
    }
}

} // namespace

// ---- construction ------------------------------------------------------------------

/**
 * @brief Empty field with no placeholder.
 */
TextField::TextField() {
    focusRing_.setOwner(this);
}

/**
 * @brief Field with initial text and a placeholder shown while empty.
 */
TextField::TextField(std::wstring text, std::wstring placeholder)
    : text_(std::move(text)), placeholder_(std::move(placeholder)) {
    focusRing_.setOwner(this);
    caret_ = text_.size();
    anchor_ = caret_;
    validate();
}

// ---- public API ---------------------------------------------------------------------

/**
 * @brief Replaces the text programmatically; history is reset, dirty state untouched.
 */
void TextField::setText(std::wstring text, bool notify) {
    text_ = std::move(text);
    caret_ = text_.size();
    anchor_ = caret_;
    scrollX_ = 0.0f;
    undo_.clear();
    redo_.clear();

    // A programmatic value becomes the new baseline for Escape while focused.
    if (focused()) {
        textAtFocus_ = text_;
    }
    validate();
    ensureCaretVisible();
    updateImeCaret();
    restartBlink();
    invalidate();

    if (notify && onChanged) {
        onChanged(text_);
    }
}

/**
 * @brief Switches between body and mono typography.
 */
void TextField::setMonospace(bool mono) {
    if (mono_ == mono) {
        return;
    }
    mono_ = mono;
    scrollX_ = 0.0f;
    invalidateLayout();
}

/**
 * @brief Installs the validator and runs it against the current text.
 */
void TextField::setValidator(std::function<std::wstring(const std::wstring&)> validator) {
    validator_ = std::move(validator);
    validate();
}

/**
 * @brief Selects everything, caret at the end.
 */
void TextField::selectAll() {
    anchor_ = 0;
    caret_ = text_.size();
    ensureCaretVisible();
    updateImeCaret();
    restartBlink();
    invalidate();
}

// ---- geometry / text metrics ---------------------------------------------------------

/**
 * @brief The area the text scrolls inside (8 dip horizontal padding).
 */
Rect TextField::textRect() const {
    const float w = std::max(0.0f, frame_.w - 2.0f * kPadX);
    return {kPadX, 0.0f, w, std::max(0.0f, frame_.h)};
}

/**
 * @brief Caret index nearest to @p x (layout space, scroll already applied).
 */
size_t TextField::hitTestPosition(float x) {
    if (text_.empty()) {
        return 0;
    }
    const TextStyle style = mono_ ? typography::mono() : typography::body();

    // Real hit test through the shared layout when available.
    if (TextCache* cache = usableCache()) {
        auto layout = cache->layout(text_, style, 0.0f, Trimming::None, 1);
        if (layout) {
            BOOL trailing = FALSE;
            BOOL inside = FALSE;
            DWRITE_HIT_TEST_METRICS m{};
            const float lh = lineMetricsShared(style).lineHeight;
            const HRESULT hr = layout->HitTestPoint(x, lh * 0.5f, &trailing, &inside, &m);
            if (SUCCEEDED(hr)) {
                const size_t pos = static_cast<size_t>(m.textPosition) + (trailing ? static_cast<size_t>(m.length) : 0);
                return std::min(pos, text_.size());
            }
            HH_LOG_WARN(kLog, L"HitTestPoint failed: 0x{:08X}", static_cast<unsigned>(hr));
        }
    }

    // Fallback: proportional estimate from the average advance.
    const float adv = estimatedAdvance(text_, style);
    const long idx = std::lround(std::max(0.0f, x) / adv);
    return std::min(static_cast<size_t>(std::max(0L, idx)), text_.size());
}

/**
 * @brief X of caret position @p pos in layout space.
 */
float TextField::caretX(size_t pos) {
    pos = std::min(pos, text_.size());
    if (text_.empty() || pos == 0) {
        return 0.0f;
    }
    const TextStyle style = mono_ ? typography::mono() : typography::body();

    // Real metrics through the shared layout when available.
    if (TextCache* cache = usableCache()) {
        auto layout = cache->layout(text_, style, 0.0f, Trimming::None, 1);
        if (layout) {
            return layoutCaretX(layout.Get(), pos);
        }
    }

    // Fallback: measure the prefix (estimated when no cache exists).
    return measureTextShared(std::wstring_view(text_).substr(0, pos), style).w;
}

/**
 * @brief Scrolls horizontally so the caret stays inside the text rect.
 */
void TextField::ensureCaretVisible() {
    const Rect tr = textRect();
    if (tr.w <= 0.0f || text_.empty()) {
        scrollX_ = 0.0f;
        return;
    }

    // Keep the caret between the left and right edge of the text rect.
    const float cx = caretX(caret_);
    const float textW = caretX(text_.size());
    float s = scrollX_;
    if (cx - s < 0.0f) {
        s = cx;
    } else if (cx - s > tr.w) {
        s = cx - tr.w;
    }

    // Never scroll past the end, and never leave a gap on the left.
    const float maxScroll = std::max(0.0f, textW - tr.w);
    s = std::clamp(s, 0.0f, maxScroll);
    if (s != scrollX_) {
        scrollX_ = s;
        invalidate();
    }
}

/**
 * @brief Tells the IME where the caret is (root dips) so candidates dock to it.
 */
void TextField::updateImeCaret() {
    RootView* rv = root();
    if (!rv || !focused()) {
        return;
    }
    const TextStyle style = mono_ ? typography::mono() : typography::body();
    const float lh = lineMetricsShared(style).lineHeight;
    const Rect tr = textRect();
    const Point local{tr.x - scrollX_ + caretX(caret_), tr.y + (tr.h - lh) * 0.5f};
    const Point rootPt = toRoot(local);
    rv->window().setImeCaret({rootPt.x, rootPt.y, 1.0f, std::max(1.0f, lh)});
}

// ---- editing primitives -------------------------------------------------------------------

/**
 * @brief Removes the selected range (no-op without a selection).
 */
void TextField::deleteSelection() {
    if (!hasSelection()) {
        return;
    }
    const size_t lo = std::min(caret_, anchor_);
    const size_t hi = std::min(std::max(caret_, anchor_), text_.size());
    if (lo >= hi) {
        caret_ = anchor_ = std::min(lo, text_.size());
        return;
    }
    text_.erase(lo, hi - lo);
    caret_ = anchor_ = lo;
}

/**
 * @brief Inserts @p s at the caret, replacing the selection.
 *
 * Typed characters are grouped into one undo step per word: a snapshot is
 * pushed when the insertion starts a new word, is punctuation/space, or is a
 * multi-character paste.
 */
void TextField::insertText(std::wstring_view s) {
    if (readOnly_ || s.empty()) {
        return;
    }
    caret_ = std::min(caret_, text_.size());
    anchor_ = std::min(anchor_, text_.size());

    // Word-boundary detection for undo grouping.
    const bool boundary = s.size() != 1 || !isWordChar(s[0]) || caret_ == 0 || !isWordChar(text_[caret_ - 1]);

    if (hasSelection()) {
        pushUndo();
        deleteSelection();
    } else if (boundary || undo_.empty()) {
        pushUndo();
    }

    text_.insert(caret_, s.data(), s.size());
    caret_ += s.size();
    anchor_ = caret_;
    changed();
}

/**
 * @brief Moves the caret (optionally extending the selection from the anchor).
 */
void TextField::moveCaret(size_t pos, bool extend) {
    pos = std::min(pos, text_.size());
    caret_ = pos;
    if (!extend) {
        anchor_ = pos;
    }
    restartBlink();
    ensureCaretVisible();
    updateImeCaret();
    invalidate();
}

/**
 * @brief Start of the cluster before @p pos (surrogate pairs move as one).
 */
size_t TextField::prevCluster(size_t pos) const {
    pos = std::min(pos, text_.size());
    if (pos == 0) {
        return 0;
    }
    size_t p = pos - 1;
    if (p > 0 && isLowSurrogate(text_[p]) && isHighSurrogate(text_[p - 1])) {
        --p;
    }
    return p;
}

/**
 * @brief Start of the cluster after @p pos (surrogate pairs move as one).
 */
size_t TextField::nextCluster(size_t pos) const {
    const size_t n = text_.size();
    if (pos >= n) {
        return n;
    }
    size_t p = pos + 1;
    if (p < n && isHighSurrogate(text_[pos]) && isLowSurrogate(text_[p])) {
        ++p;
    }
    return p;
}

/**
 * @brief Start of the word before @p pos (skips whitespace first).
 */
size_t TextField::prevWord(size_t pos) const {
    size_t p = std::min(pos, text_.size());
    while (p > 0 && isSpaceChar(text_[p - 1])) {
        --p;
    }
    if (p == 0) {
        return 0;
    }

    // Consume a run of the same class (word characters or punctuation).
    if (isWordChar(text_[p - 1])) {
        while (p > 0 && isWordChar(text_[p - 1])) {
            --p;
        }
    } else {
        while (p > 0 && !isWordChar(text_[p - 1]) && !isSpaceChar(text_[p - 1])) {
            --p;
        }
    }
    return p;
}

/**
 * @brief Start of the word after @p pos (Windows-style: lands after trailing spaces).
 */
size_t TextField::nextWord(size_t pos) const {
    const size_t n = text_.size();
    size_t p = std::min(pos, n);
    if (p < n) {
        if (isWordChar(text_[p])) {
            while (p < n && isWordChar(text_[p])) {
                ++p;
            }
        } else if (!isSpaceChar(text_[p])) {
            while (p < n && !isWordChar(text_[p]) && !isSpaceChar(text_[p])) {
                ++p;
            }
        }
    }
    while (p < n && isSpaceChar(text_[p])) {
        ++p;
    }
    return p;
}

// ---- undo / redo ---------------------------------------------------------------------------

/**
 * @brief Records the current state before a destructive edit (deduplicated).
 */
void TextField::pushUndo() {
    if (!undo_.empty() && undo_.back().text == text_) {
        return;
    }
    undo_.push_back({text_, caret_, anchor_});
    while (undo_.size() > kUndoDepth) {
        undo_.pop_front();
    }
    redo_.clear();
}

/**
 * @brief Restores the previous snapshot, pushing the current one onto redo.
 */
void TextField::undo() {
    if (readOnly_ || undo_.empty()) {
        return;
    }
    redo_.push_back({text_, caret_, anchor_});
    while (redo_.size() > kUndoDepth) {
        redo_.pop_front();
    }

    const Snapshot s = undo_.back();
    undo_.pop_back();
    text_ = s.text;
    caret_ = std::min(s.caret, text_.size());
    anchor_ = std::min(s.anchor, text_.size());
    changed();
}

/**
 * @brief Re-applies the last undone snapshot.
 */
void TextField::redo() {
    if (readOnly_ || redo_.empty()) {
        return;
    }
    undo_.push_back({text_, caret_, anchor_});
    while (undo_.size() > kUndoDepth) {
        undo_.pop_front();
    }

    const Snapshot s = redo_.back();
    redo_.pop_back();
    text_ = s.text;
    caret_ = std::min(s.caret, text_.size());
    anchor_ = std::min(s.anchor, text_.size());
    changed();
}

// ---- change plumbing --------------------------------------------------------------------------

/**
 * @brief After every edit: validate, keep the caret visible, notify.
 */
void TextField::changed() {
    dirty_ = true;
    validate();
    restartBlink();
    ensureCaretVisible();
    updateImeCaret();
    invalidate();
    if (onChanged) {
        onChanged(text_);
    }
}

/**
 * @brief Runs the validator; a non-empty result paints the destructive outline.
 */
void TextField::validate() {
    std::wstring next;
    if (validator_) {
        next = validator_(text_);
    }
    if (next != error_) {
        error_ = std::move(next);
        invalidate();
    }
}

/**
 * @brief Shows the caret and restarts the blink phase.
 */
void TextField::restartBlink() {
    caretVisible_ = true;
    Timeline* tl = timeline();
    blinkAt_ = tl ? tl->now() : 0.0;
    invalidate();
}

// ---- layout / paint ---------------------------------------------------------------------------

/**
 * @brief Preferred size: explicit width when given, else a sensible default; 28 tall.
 */
Size TextField::measure(const Constraints& c) {
    float w = kDefaultWidth;
    if (layoutParams_.width.has_value()) {
        w = *layoutParams_.width;
    } else if (preferredSize_.w > 0.0f) {
        w = preferredSize_.w;
    }
    w = std::max(0.0f, w);
    return c.constrain({w, kHeight});
}

/**
 * @brief Paints the field: fill, outline, selection, text/placeholder, caret, focus ring.
 */
void TextField::paintSelf(Canvas& c) {
    const Theme& t = paintTheme(*this, c);
    const Rect b = bounds();
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return;
    }

    RootView* rv = root();
    const bool isFocused = focused();
    const bool windowActive = rv ? rv->windowActive() : true;
    const float hairline = c.scale().hairline();

    // Background pill and outline (destructive when the validator complains).
    c.fillRoundedRect(b, kRadius, t.fillSecondary);
    if (error_.empty()) {
        c.strokeRoundedRect(b.inset(hairline * 0.5f), kRadius, t.separator, 0.0f);
    } else {
        c.strokeRoundedRect(b.inset(0.5f), kRadius, t.destructive, 1.0f);
    }

    // Focus ring springs in/out just outside the pill.
    const float ring = std::clamp(focusRing_.value(), 0.0f, 1.0f);
    if (ring > 0.01f) {
        c.drawFocusRing(b, kRadius, t.focusRing.scaledAlpha(ring));
    }

    // Everything textual is clipped to the padded area (plus a little slack for the caret).
    const Rect tr = textRect();
    const Rect clip = b.inset(Insets{0.0f, kPadX - kClipSlack, 0.0f, kPadX - kClipSlack});
    if (clip.w <= 0.0f) {
        return;
    }
    c.pushClip(clip);

    const TextStyle style = mono_ ? typography::mono() : typography::body();
    const Color textColour = enabled() ? t.labelPrimary : t.labelTertiary;

    if (text_.empty()) {
        // Placeholder while empty.
        if (!placeholder_.empty()) {
            c.drawText(placeholder_, style, tr, t.labelTertiary, HAlign::Left, VAlign::Center, Trimming::End, 1);
        }
    } else {
        // Paint through the same layout that hit-testing uses.
        auto layout = c.text().layout(text_, style, 0.0f, Trimming::None, 1);
        float lh = lineMetricsShared(style).lineHeight;
        if (layout) {
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(layout->GetMetrics(&m)) && m.height > 0.0f) {
                lh = m.height;
            }
        }
        const Point origin{tr.x - scrollX_, tr.y + (tr.h - lh) * 0.5f};

        // Selection rects behind the glyphs.
        if (hasSelection()) {
            const Color sel = (isFocused && windowActive) ? t.accent.withAlpha(kSelectionAlphaFocused)
                                                          : t.labelPrimary.withAlpha(kSelectionAlphaBlurred);
            const size_t lo = std::min(caret_, anchor_);
            const size_t hi = std::min(std::max(caret_, anchor_), text_.size());
            bool painted = false;
            if (layout && hi > lo) {
                // Ask DirectWrite for the exact glyph-run rectangles.
                UINT32 count = 0;
                HRESULT hr = layout->HitTestTextRange(static_cast<UINT32>(lo), static_cast<UINT32>(hi - lo), origin.x, origin.y, nullptr, 0, &count);
                if ((hr == E_NOT_SUFFICIENT_BUFFER || SUCCEEDED(hr)) && count > 0) {
                    std::vector<DWRITE_HIT_TEST_METRICS> runs(count);
                    hr = layout->HitTestTextRange(static_cast<UINT32>(lo), static_cast<UINT32>(hi - lo), origin.x, origin.y, runs.data(), count, &count);
                    if (SUCCEEDED(hr)) {
                        for (UINT32 i = 0; i < count && i < runs.size(); ++i) {
                            const DWRITE_HIT_TEST_METRICS& r = runs[i];
                            c.fillRect({r.left, r.top, std::max(0.0f, r.width), std::max(0.0f, r.height)}, sel);
                        }
                        painted = true;
                    }
                }
            }
            if (!painted && hi > lo) {
                // Fallback: one rect from the start to the end caret position.
                const float x0 = origin.x + caretX(lo);
                const float x1 = origin.x + caretX(hi);
                c.fillRect({std::min(x0, x1), origin.y, std::abs(x1 - x0), lh}, sel);
            }
        }

        // The text itself.
        if (layout) {
            c.drawTextLayout(layout.Get(), origin, textColour);
        } else {
            c.drawText(text_, style, {origin.x, tr.y, 1.0e6f, tr.h}, textColour, HAlign::Left, VAlign::Center, Trimming::None, 1);
        }

        // Caret: 1 dip (at least one pixel), only while focused in the active window.
        if (isFocused && windowActive && caretVisible_ && !readOnly_ && enabled()) {
            const float cx = layout ? layoutCaretX(layout.Get(), caret_) : caretX(caret_);
            const float x = c.scale().snap(origin.x + cx);
            const float cw = std::max(1.0f, hairline);
            c.fillRect({x, origin.y, cw, lh}, t.labelPrimary);
        }
    }

    // Caret in an empty field sits at the left edge of the text rect.
    if (text_.empty() && isFocused && windowActive && caretVisible_ && !readOnly_ && enabled()) {
        const float lh = lineMetricsShared(style).lineHeight;
        const float x = c.scale().snap(tr.x);
        const float cw = std::max(1.0f, hairline);
        c.fillRect({x, tr.y + (tr.h - lh) * 0.5f, cw, lh}, t.labelPrimary);
    }

    c.pop();
}

// ---- mouse ---------------------------------------------------------------------------------------

/**
 * @brief Places the caret (Shift extends); double/triple clicks select word/all.
 */
bool TextField::onMouseDown(const MouseEvent& e) {
    if (!enabled() || e.button != MouseButton::Left) {
        return false;
    }
    const float x = e.pos.x - textRect().x + scrollX_;
    const size_t pos = hitTestPosition(x);

    if (e.clickCount >= 3) {
        selectAll();
        dragging_ = false;
    } else if (e.clickCount == 2) {
        size_t start = 0;
        size_t end = 0;
        wordRangeAt(text_, pos, start, end);
        anchor_ = start;
        moveCaret(end, true);
        dragging_ = false;
    } else {
        moveCaret(pos, e.mods.shift);
        dragging_ = true;
    }
    return true;
}

/**
 * @brief Ends a drag selection.
 */
bool TextField::onMouseUp(const MouseEvent&) {
    dragging_ = false;
    return true;
}

/**
 * @brief Extends the selection while dragging (auto-scrolls via ensureCaretVisible).
 */
bool TextField::onMouseMove(const MouseEvent& e) {
    if (!dragging_) {
        return false;
    }
    const float x = e.pos.x - textRect().x + scrollX_;
    const size_t pos = hitTestPosition(x);
    if (pos != caret_) {
        moveCaret(pos, true);
    }
    return true;
}

/**
 * @brief Double-click selects the word under the pointer.
 */
bool TextField::onDoubleClick(const MouseEvent& e) {
    if (!enabled()) {
        return false;
    }
    const float x = e.pos.x - textRect().x + scrollX_;
    const size_t pos = hitTestPosition(x);
    size_t start = 0;
    size_t end = 0;
    wordRangeAt(text_, pos, start, end);
    anchor_ = start;
    moveCaret(end, true);
    dragging_ = false;
    return true;
}

// ---- keyboard ------------------------------------------------------------------------------------

/**
 * @brief Navigation, selection, clipboard, deletion, undo/redo, submit and revert.
 *
 * Tab is deliberately not consumed so focus traversal keeps working.
 */
bool TextField::onKeyDown(const KeyEvent& e) {
    if (!enabled()) {
        return false;
    }
    const bool ctrl = e.mods.ctrl;
    const bool shift = e.mods.shift;
    const size_t n = text_.size();
    caret_ = std::min(caret_, n);
    anchor_ = std::min(anchor_, n);

    switch (e.vk) {
    case VK_TAB:
        return false;

    case VK_LEFT:
        if (hasSelection() && !shift && !ctrl) {
            moveCaret(std::min(caret_, anchor_), false);
        } else {
            moveCaret(ctrl ? prevWord(caret_) : prevCluster(caret_), shift);
        }
        return true;

    case VK_RIGHT:
        if (hasSelection() && !shift && !ctrl) {
            moveCaret(std::max(caret_, anchor_), false);
        } else {
            moveCaret(ctrl ? nextWord(caret_) : nextCluster(caret_), shift);
        }
        return true;

    case VK_HOME:
        moveCaret(0, shift);
        return true;

    case VK_END:
        moveCaret(n, shift);
        return true;

    case VK_BACK:
        if (readOnly_) {
            return true;
        }
        if (hasSelection()) {
            pushUndo();
            deleteSelection();
            changed();
        } else if (caret_ > 0) {
            const size_t from = ctrl ? prevWord(caret_) : prevCluster(caret_);
            pushUndo();
            text_.erase(from, caret_ - from);
            caret_ = anchor_ = from;
            changed();
        }
        return true;

    case VK_DELETE:
        if (readOnly_) {
            return true;
        }
        if (hasSelection()) {
            pushUndo();
            deleteSelection();
            changed();
        } else if (caret_ < n) {
            const size_t to = ctrl ? nextWord(caret_) : nextCluster(caret_);
            pushUndo();
            text_.erase(caret_, to - caret_);
            anchor_ = caret_;
            changed();
        }
        return true;

    case VK_RETURN: {
        // Submit commits the value; Escape afterwards reverts to it, not the older text.
        dirty_ = false;
        textAtFocus_ = text_;
        auto cb = onSubmit;
        if (cb) {
            cb(text_);
        }
        return true;
    }

    case VK_ESCAPE: {
        // Revert to the value the field had when it gained focus, then blur.
        if (text_ != textAtFocus_) {
            text_ = textAtFocus_;
            caret_ = anchor_ = text_.size();
            scrollX_ = 0.0f;
            validate();
            ensureCaretVisible();
            if (onChanged) {
                onChanged(text_);
            }
        }
        dirty_ = false;
        invalidate();
        if (RootView* rv = root()) {
            rv->focus().focus(nullptr);
        }
        return true;
    }

    case 'A':
        if (ctrl) {
            selectAll();
            return true;
        }
        return false;

    case 'C':
        if (ctrl) {
            if (hasSelection()) {
                const size_t lo = std::min(caret_, anchor_);
                const size_t hi = std::max(caret_, anchor_);
                if (RootView* rv = root()) {
                    if (!rv->window().clipboardSetText(text_.substr(lo, hi - lo))) {
                        HH_LOG_WARN(kLog, L"clipboardSetText failed (copy)");
                    }
                }
            }
            return true;
        }
        return false;

    case 'X':
        if (ctrl) {
            if (hasSelection()) {
                const size_t lo = std::min(caret_, anchor_);
                const size_t hi = std::max(caret_, anchor_);
                bool copied = false;
                if (RootView* rv = root()) {
                    copied = rv->window().clipboardSetText(text_.substr(lo, hi - lo));
                    if (!copied) {
                        HH_LOG_WARN(kLog, L"clipboardSetText failed (cut)");
                    }
                }
                // Only remove the text once it is safely on the clipboard.
                if (copied && !readOnly_) {
                    pushUndo();
                    deleteSelection();
                    changed();
                }
            }
            return true;
        }
        return false;

    case 'V':
        if (ctrl) {
            if (!readOnly_) {
                std::wstring clip;
                if (RootView* rv = root()) {
                    clip = rv->window().clipboardGetText();
                }
                // Single line only: line breaks and tabs are dropped.
                std::wstring cleaned;
                cleaned.reserve(clip.size());
                for (wchar_t ch : clip) {
                    if (ch != L'\r' && ch != L'\n' && ch != L'\t') {
                        cleaned.push_back(ch);
                    }
                }
                if (!cleaned.empty()) {
                    insertText(cleaned);
                }
            }
            return true;
        }
        return false;

    case 'Z':
        if (ctrl) {
            if (shift) {
                redo();
            } else {
                undo();
            }
            return true;
        }
        return false;

    case 'Y':
        if (ctrl) {
            redo();
            return true;
        }
        return false;

    default:
        return false;
    }
}

/**
 * @brief Inserts printable characters (surrogate pairs arrive already combined).
 */
bool TextField::onChar(char32_t ch) {
    if (!enabled()) {
        return false;
    }
    if (ch < 0x20 || ch == 0x7F) {
        return false;
    }
    if (readOnly_) {
        return true;
    }

    // Lone surrogate code units would corrupt the UTF-16 string.
    if (ch >= 0xD800 && ch <= 0xDFFF) {
        HH_LOG_DEBUG(kLog, L"ignoring lone surrogate U+{:04X}", static_cast<unsigned>(ch));
        return true;
    }

    wchar_t buf[2] = {0, 0};
    size_t len = 1;
    if (ch > 0xFFFF) {
        const char32_t v = ch - 0x10000;
        buf[0] = static_cast<wchar_t>(0xD800 + (v >> 10));
        buf[1] = static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
        len = 2;
    } else {
        buf[0] = static_cast<wchar_t>(ch);
    }
    insertText(std::wstring_view(buf, len));
    return true;
}

// ---- focus / ticks ---------------------------------------------------------------------------------

/**
 * @brief Focus gain: baseline for Escape, blink timing, IME; loss: submit when dirty.
 */
void TextField::onFocusChanged(bool focused) {
    RootView* rv = root();
    focusRing_.setOwner(this);

    if (focused) {
        textAtFocus_ = text_;
        dirty_ = false;

        // Blink rate from the system (INFINITE = never blink, 0 = query failed).
        const UINT ms = ::GetCaretBlinkTime();
        if (ms == INFINITE) {
            blinkPeriod_ = 0.0;
        } else if (ms == 0) {
            HH_LOG_DEBUG(kLog, L"GetCaretBlinkTime failed ({}); using default", ::GetLastError());
            blinkPeriod_ = kDefaultBlink;
        } else {
            blinkPeriod_ = static_cast<double>(ms) / 1000.0;
        }

        // Keyboard focus selects everything (a click collapses it right after).
        anchor_ = 0;
        caret_ = text_.size();
        restartBlink();
        setWantsFrameTicks(true);
        focusRing_.animateTo(1.0f, springs::snappy);
        if (rv) {
            rv->window().setImeEnabled(true);
        }
        ensureCaretVisible();
        updateImeCaret();
    } else {
        dragging_ = false;
        setWantsFrameTicks(false);
        focusRing_.animateTo(0.0f, springs::snappy);
        if (rv) {
            rv->window().setImeEnabled(false);
        }

        // Leaving a field with edits commits them.
        if (dirty_) {
            dirty_ = false;
            textAtFocus_ = text_;
            auto cb = onSubmit;
            if (cb) {
                cb(text_);
            }
        }
    }
    invalidate();
}

/**
 * @brief Toggles the caret at the system blink rate while focused.
 */
void TextField::onFrame(double now) {
    if (!focused() || blinkPeriod_ <= 0.0) {
        return;
    }
    if (std::isnan(now)) {
        return;
    }

    // Guard against clock resets (paused timelines) so the caret never sticks hidden.
    if (now < blinkAt_) {
        blinkAt_ = now;
    }
    if (now - blinkAt_ >= blinkPeriod_) {
        caretVisible_ = !caretVisible_;
        blinkAt_ = now;
        invalidate();
    }
}

/**
 * @brief Leaving the tree stops the blink ticks and any drag.
 */
void TextField::onDetached() {
    dragging_ = false;
    setWantsFrameTicks(false);
}

} // namespace hh::ui
