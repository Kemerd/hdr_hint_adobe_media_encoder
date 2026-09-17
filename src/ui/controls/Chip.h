// ---------------------------------------------------------------------------
// Chip.h - small status/badge capsule (uppercase 11 pt, tinted).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <string>

namespace hh::ui {

enum class ChipTone { Neutral, Accent, Success, Warning, Destructive, Info, PQ, HLG, SDR };

class Chip : public Widget {
public:
    Chip() = default;
    explicit Chip(std::wstring text, ChipTone tone = ChipTone::Neutral);

    void setText(std::wstring text);
    void setTone(ChipTone tone);
    [[nodiscard]] ChipTone tone() const noexcept { return tone_; }
    /// Leading 6 dip dot.
    void setShowDot(bool on) { dot_ = on; invalidateLayout(); }
    /// Leading 12 dip rotating arc (in-progress states).
    void setSpinner(bool on);
    void setTooltipText(std::wstring tip) { tooltip_ = std::move(tip); }

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    void onFrame(double now) override;
    [[nodiscard]] std::wstring tooltip() const override { return tooltip_; }
    /// Colour for a tone from the theme.
    static Color toneColor(const Theme& t, ChipTone tone);

private:
    std::wstring text_;
    ChipTone tone_ = ChipTone::Neutral;
    bool dot_ = false;
    bool spinner_ = false;
    float angle_ = 0.0f;
    std::wstring tooltip_;
};

} // namespace hh::ui
