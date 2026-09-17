// ---------------------------------------------------------------------------
// Label.h - static text.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <string>

namespace hh::ui {

enum class LabelTone { Primary, Secondary, Tertiary, Quaternary, Accent, Success, Warning, Destructive, Custom };

class Label : public Widget {
public:
    Label() = default;
    explicit Label(std::wstring text, TextStyle style = typography::body(), LabelTone tone = LabelTone::Primary);

    void setText(std::wstring text);
    [[nodiscard]] const std::wstring& text() const noexcept { return text_; }
    void setStyle(const TextStyle& style);
    [[nodiscard]] const TextStyle& style() const noexcept { return style_; }
    void setTone(LabelTone tone);
    void setCustomColor(const Color& c);
    void setAlign(HAlign align);
    void setVAlign(VAlign align);
    void setTrimming(Trimming t);
    /// 0 = unlimited (implies wrap).
    void setMaxLines(int lines);
    void setTooltipText(std::wstring tip) { tooltip_ = std::move(tip); }

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] std::wstring tooltip() const override { return tooltip_; }
    [[nodiscard]] Color resolvedColor() const;

private:
    std::wstring text_;
    TextStyle style_ = typography::body();
    LabelTone tone_ = LabelTone::Primary;
    Color custom_;
    HAlign align_ = HAlign::Left;
    VAlign valign_ = VAlign::Center;
    Trimming trimming_ = Trimming::End;
    int maxLines_ = 1;
    std::wstring tooltip_;
};

} // namespace hh::ui
