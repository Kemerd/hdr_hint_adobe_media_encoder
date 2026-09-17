// ---------------------------------------------------------------------------
// PopupMenu.h - the menu content shown inside a PopupWindow (selection lists
// for PopupButton, context menus for cards).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace hh::ui {

struct PopupItem {
    std::wstring label;
    std::wstring subtitle;        ///< optional second line (smaller, secondary)
    std::wstring value;           ///< opaque id returned to the caller
    bool enabled = true;
    bool separatorAfter = false;
    bool destructive = false;     ///< red label (context menus)
    IconId icon = IconId::None;
};

class PopupMenu : public Widget {
public:
    PopupMenu() = default;
    PopupMenu(std::vector<PopupItem> items, int selected);

    void setItems(std::vector<PopupItem> items);
    [[nodiscard]] const std::vector<PopupItem>& items() const noexcept { return items_; }
    /// Shows a checkmark at @p index (-1 = none).
    void setSelected(int index);
    /// Called with the chosen index; the popup then dismisses itself.
    std::function<void(int)> onSelect;
    std::function<void()> onCancel;
    /// Minimum width (the opener's width for PopupButton).
    void setMinWidth(float w) { minWidth_ = w; }

    /// Size needed for the items (no window shadow margin).
    Size measure(const Constraints& c) override;
    void onLayout() override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] bool interactive() const override { return true; }
    bool onMouseDown(const MouseEvent& e) override;
    bool onMouseUp(const MouseEvent& e) override;
    bool onMouseMove(const MouseEvent& e) override;
    void onMouseLeave() override;
    bool onWheel(const WheelEvent& e) override;
    bool onKeyDown(const KeyEvent& e) override;
    bool onChar(char32_t ch) override;
    [[nodiscard]] bool focusable() const override { return true; }
    [[nodiscard]] CursorKind cursor() const override { return CursorKind::Arrow; }
    void onAttached() override;

    static constexpr float kRowHeight = 28.0f;
    static constexpr float kRowHeightTwoLine = 40.0f;
    static constexpr float kPadding = 5.0f;
    static constexpr float kSeparatorHeight = 9.0f;

private:
    [[nodiscard]] float rowHeight(int i) const;
    [[nodiscard]] Rect rowRect(int i) const;
    [[nodiscard]] int indexAt(Point local) const;
    void moveHighlight(int delta);
    void choose(int index);
    void ensureVisible(int index);

    std::vector<PopupItem> items_;
    int selected_ = -1;
    int highlight_ = -1;
    float minWidth_ = 0.0f;
    float scroll_ = 0.0f;
    float contentHeight_ = 0.0f;
    std::wstring typeAhead_;
    double typeAheadAt_ = 0.0;
    Animatable<float> open_{0.0f};
};

} // namespace hh::ui
