// ---------------------------------------------------------------------------
// ToggleSwitch.h - macOS-style switch (38x22), drag-to-toggle, knob stretch.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <functional>

namespace hh::ui {

class ToggleSwitch : public Widget {
public:
    ToggleSwitch() ;
    explicit ToggleSwitch(bool on, std::function<void(bool)> onChanged = {});

    void setOn(bool on, bool animated = true);
    [[nodiscard]] bool isOn() const noexcept { return on_; }
    /// Green (macOS System Settings) by default; accent blue when false.
    void setUseSuccessColor(bool green) { green_ = green; invalidate(); }
    void setTooltipText(std::wstring tip) { tooltip_ = std::move(tip); }

    std::function<void(bool)> onChanged;

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] bool interactive() const override { return true; }
    bool onMouseDown(const MouseEvent& e) override;
    bool onMouseUp(const MouseEvent& e) override;
    bool onMouseMove(const MouseEvent& e) override;
    void onMouseLeave() override;
    bool onKeyDown(const KeyEvent& e) override;
    [[nodiscard]] bool focusable() const override { return enabled(); }
    [[nodiscard]] CursorKind cursor() const override { return CursorKind::Hand; }
    [[nodiscard]] std::wstring tooltip() const override { return tooltip_; }
    void onThemeChanged() override;

private:
    void animateToState(bool animated);
    void commit(bool on);

    bool on_ = false;
    bool green_ = true;
    bool dragging_ = false;
    bool dragMoved_ = false;
    float dragStartX_ = 0.0f;
    std::wstring tooltip_;
    Animatable<float> knobX_{0.0f};     ///< 0..1 normalised position
    Animatable<float> knobStretch_{0.0f};
    Animatable<Color> track_;
};

} // namespace hh::ui
