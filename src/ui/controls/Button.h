// ---------------------------------------------------------------------------
// Button.h - push buttons (Primary / Secondary / Destructive / Plain) and
// IconButton (window buttons, card overflow).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <functional>
#include <string>

namespace hh::ui {

enum class ButtonKind { Primary, Secondary, Destructive, Plain };

class Button : public Widget {
public:
    Button() = default;
    Button(std::wstring text, ButtonKind kind = ButtonKind::Secondary, std::function<void()> onClick = {});

    void setText(std::wstring text);
    [[nodiscard]] const std::wstring& text() const noexcept { return text_; }
    void setKind(ButtonKind kind);
    void setIcon(IconId icon);
    /// 24 dip tall instead of 28.
    void setCompact(bool compact);
    void setTooltipText(std::wstring tip) { tooltip_ = std::move(tip); }
    /// Fills the available width instead of hugging the label.
    void setStretch(bool stretch) { stretch_ = stretch; invalidateLayout(); }

    std::function<void()> onClick;

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] bool interactive() const override { return true; }
    bool onMouseDown(const MouseEvent& e) override;
    bool onMouseUp(const MouseEvent& e) override;
    bool onMouseMove(const MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    bool onKeyDown(const KeyEvent& e) override;
    [[nodiscard]] bool focusable() const override { return enabled(); }
    [[nodiscard]] CursorKind cursor() const override { return CursorKind::Hand; }
    [[nodiscard]] std::wstring tooltip() const override { return tooltip_; }
    void onThemeChanged() override;

private:
    void updateFill(bool animated);
    void click();

    std::wstring text_;
    ButtonKind kind_ = ButtonKind::Secondary;
    IconId icon_ = IconId::None;
    bool compact_ = false;
    bool stretch_ = false;
    std::wstring tooltip_;
    Animatable<Color> fill_;
    Animatable<float> pressScale_{1.0f};
};

class IconButton : public Widget {
public:
    IconButton() = default;
    explicit IconButton(IconId icon, std::function<void()> onClick = {});

    void setIcon(IconId icon);
    /// Square size in dips (default 28). Window buttons use 46x32 via setSize(w,h).
    void setSize(float w, float h);
    /// Close-button treatment: red fill + white glyph on hover.
    void setDestructiveHover(bool on) { destructiveHover_ = on; }
    void setTooltipText(std::wstring tip) { tooltip_ = std::move(tip); }
    void setStrokeWidth(float w) { stroke_ = w; }
    /// Glyph colour override (default: labelSecondary, labelPrimary on hover).
    void setCustomColor(const Color& c) { custom_ = c; hasCustom_ = true; invalidate(); }

    std::function<void()> onClick;

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] bool interactive() const override { return true; }
    bool onMouseDown(const MouseEvent& e) override;
    bool onMouseUp(const MouseEvent& e) override;
    bool onMouseMove(const MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    bool onKeyDown(const KeyEvent& e) override;
    [[nodiscard]] bool focusable() const override { return enabled(); }
    [[nodiscard]] CursorKind cursor() const override { return CursorKind::Hand; }
    [[nodiscard]] std::wstring tooltip() const override { return tooltip_; }

private:
    IconId icon_ = IconId::None;
    float w_ = 28.0f, h_ = 28.0f;
    float stroke_ = 1.5f;
    bool destructiveHover_ = false;
    bool hasCustom_ = false;
    Color custom_;
    std::wstring tooltip_;
    Animatable<float> hoverAmount_{0.0f};
};

} // namespace hh::ui
