// ---------------------------------------------------------------------------
// SegmentedControl.h - tabs with a sliding pill (spring animated).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace hh::ui {

class SegmentedControl : public Widget {
public:
    SegmentedControl() = default;
    explicit SegmentedControl(std::vector<std::wstring> items, int selected = 0);

    void setItems(std::vector<std::wstring> items);
    [[nodiscard]] const std::vector<std::wstring>& items() const noexcept { return items_; }
    void setSelected(int index, bool animated = true);
    [[nodiscard]] int selected() const noexcept { return selected_; }
    /// Fixed width per segment (0 = size to content, equal widths).
    void setSegmentWidth(float w) { segmentWidth_ = w; invalidateLayout(); }

    std::function<void(int)> onChanged;

    Size measure(const Constraints& c) override;
    void onLayout() override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] bool interactive() const override { return true; }
    bool onMouseDown(const MouseEvent& e) override;
    bool onMouseUp(const MouseEvent& e) override;
    bool onMouseMove(const MouseEvent& e) override;
    void onMouseLeave() override;
    bool onKeyDown(const KeyEvent& e) override;
    [[nodiscard]] bool focusable() const override { return enabled() && !items_.empty(); }
    [[nodiscard]] CursorKind cursor() const override { return CursorKind::Hand; }

private:
    [[nodiscard]] Rect segmentRect(int index) const;
    [[nodiscard]] int indexAt(Point local) const;
    void select(int index, bool animated, bool notify);

    std::vector<std::wstring> items_;
    int selected_ = 0;
    int hoverIndex_ = -1;
    int pressIndex_ = -1;
    float segmentWidth_ = 0.0f;
    Animatable<Rect> pill_;
};

} // namespace hh::ui
