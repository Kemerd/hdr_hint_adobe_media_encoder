// ---------------------------------------------------------------------------
// TextField.h - single-line text input (caret, selection, clipboard, undo,
// IME via the window services, validation outline).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <deque>
#include <functional>
#include <string>

namespace hh::ui {

class TextField : public Widget {
public:
    TextField();
    explicit TextField(std::wstring text, std::wstring placeholder = L"");

    void setText(std::wstring text, bool notify = false);
    [[nodiscard]] const std::wstring& text() const noexcept { return text_; }
    void setPlaceholder(std::wstring text) { placeholder_ = std::move(text); invalidate(); }
    void setMonospace(bool mono);
    void setReadOnly(bool ro) { readOnly_ = ro; invalidate(); }
    /// Returns an error message (shown as tooltip + red outline) or empty when valid.
    void setValidator(std::function<std::wstring(const std::wstring&)> validator);
    [[nodiscard]] bool valid() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::wstring& validationError() const noexcept { return error_; }
    void selectAll();
    void setWidthDips(float w) { layoutParams().width = w; invalidateLayout(); }

    std::function<void(const std::wstring&)> onChanged;   ///< every edit
    std::function<void(const std::wstring&)> onSubmit;    ///< Enter or focus loss after edits

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] bool interactive() const override { return true; }
    bool onMouseDown(const MouseEvent& e) override;
    bool onMouseUp(const MouseEvent& e) override;
    bool onMouseMove(const MouseEvent& e) override;
    bool onDoubleClick(const MouseEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;
    bool onChar(char32_t ch) override;
    [[nodiscard]] bool focusable() const override { return enabled(); }
    void onFocusChanged(bool focused) override;
    [[nodiscard]] CursorKind cursor() const override { return CursorKind::IBeam; }
    [[nodiscard]] std::wstring tooltip() const override { return error_; }
    void onFrame(double now) override;
    void onDetached() override;

private:
    struct Snapshot { std::wstring text; size_t caret; size_t anchor; };

    [[nodiscard]] Rect textRect() const;
    [[nodiscard]] size_t hitTestPosition(float x);
    [[nodiscard]] float caretX(size_t pos);
    [[nodiscard]] bool hasSelection() const noexcept { return caret_ != anchor_; }
    void deleteSelection();
    void insertText(std::wstring_view s);
    void moveCaret(size_t pos, bool extend);
    [[nodiscard]] size_t prevCluster(size_t pos) const;
    [[nodiscard]] size_t nextCluster(size_t pos) const;
    [[nodiscard]] size_t prevWord(size_t pos) const;
    [[nodiscard]] size_t nextWord(size_t pos) const;
    void pushUndo();
    void undo();
    void redo();
    void ensureCaretVisible();
    void changed();
    void validate();
    void restartBlink();
    void updateImeCaret();

    std::wstring text_;
    std::wstring placeholder_;
    std::wstring textAtFocus_;
    std::wstring error_;
    std::function<std::wstring(const std::wstring&)> validator_;
    size_t caret_ = 0;
    size_t anchor_ = 0;
    float scrollX_ = 0.0f;
    bool mono_ = false;
    bool readOnly_ = false;
    bool dragging_ = false;
    bool caretVisible_ = true;
    double blinkAt_ = 0.0;
    double blinkPeriod_ = 0.53;
    bool dirty_ = false;
    std::deque<Snapshot> undo_;
    std::deque<Snapshot> redo_;
    Animatable<float> focusRing_{0.0f};
};

} // namespace hh::ui
