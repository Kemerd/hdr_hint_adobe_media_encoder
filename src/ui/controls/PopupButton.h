// ---------------------------------------------------------------------------
// PopupButton.h - macOS pop-up button (current value + chevron) opening a
// PopupMenu in the popup window.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/controls/PopupMenu.h"
#include "ui/core/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace hh::ui {

class PopupButton : public Widget {
public:
    PopupButton() = default;
    explicit PopupButton(std::vector<PopupItem> items, int selected = -1);

    void setItems(std::vector<PopupItem> items);
    [[nodiscard]] const std::vector<PopupItem>& items() const noexcept { return items_; }
    void setSelectedIndex(int index);
    /// Selects the item whose value matches (no-op when absent).
    bool setSelectedValue(std::wstring_view value);
    [[nodiscard]] int selectedIndex() const noexcept { return selected_; }
    [[nodiscard]] std::wstring selectedValue() const;
    [[nodiscard]] std::wstring selectedLabel() const;
    void setPlaceholder(std::wstring text) { placeholder_ = std::move(text); invalidate(); }
    void setCompact(bool compact) { compact_ = compact; invalidateLayout(); }
    void setTooltipText(std::wstring tip) { tooltip_ = std::move(tip); }
    /// Optional leading caption inside the control ("Preset").
    void setCaption(std::wstring caption) { caption_ = std::move(caption); invalidateLayout(); }

    std::function<void(int index)> onChanged;

    void open();
    void close();
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] bool interactive() const override { return true; }
    bool onMouseDown(const MouseEvent& e) override;
    bool onMouseUp(const MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    bool onKeyDown(const KeyEvent& e) override;
    [[nodiscard]] bool focusable() const override { return enabled(); }
    [[nodiscard]] CursorKind cursor() const override { return CursorKind::Hand; }
    [[nodiscard]] std::wstring tooltip() const override { return tooltip_; }

private:
    std::vector<PopupItem> items_;
    int selected_ = -1;
    std::wstring placeholder_ = L"Choose…";
    std::wstring caption_;
    std::wstring tooltip_;
    bool compact_ = false;
    bool open_ = false;
    Animatable<float> hover_{0.0f};
};

} // namespace hh::ui
