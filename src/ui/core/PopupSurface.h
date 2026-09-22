// ---------------------------------------------------------------------------
// PopupSurface.h - the root content of a popup window: shadow + card + menu.
//
// Shared by the Win32 PopupWindow and the macOS popup panel so menus look the
// same everywhere: the window itself is transparent, this widget paints a soft
// shadow, an opaque rounded card and a hairline outline, and hosts the menu
// inset by the shadow margin.
//
// Not interactive itself, so clicks on the shadow margin fall through to the
// window, which treats them as "outside" and dismisses.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <memory>
#include <vector>

namespace hh::ui {

/// Corner radius of the popup card (design language: 10 for popups).
inline constexpr float kPopupCardRadius = 10.0f;
/// Shadow blur in dips (kept below the shadow margin so nothing is cut off).
inline constexpr float kPopupShadowBlur = 18.0f;
/// Gap between the anchor and the popup card.
inline constexpr float kPopupAnchorGap = 4.0f;
/// Transparent margin around the card that the shadow draws into.
inline constexpr float kPopupShadowMargin = 24.0f;

class PopupSurface final : public Widget {
public:
    explicit PopupSurface(float shadowMargin);

    /**
     * @brief Replaces the hosted menu. The previous one is retired (kept
     *        alive until purgeRetired()) because it may be on the call stack.
     */
    Widget* setMenu(std::unique_ptr<Widget> menu);
    /// Detaches the current menu without destroying it yet.
    void retireMenu();
    /// Destroys retired menus (call from a posted message / later run-loop turn, never inline).
    void purgeRetired() { retired_.clear(); }

    [[nodiscard]] Widget* menu() const noexcept { return menu_; }
    [[nodiscard]] float margin() const noexcept { return margin_; }
    /// The card rect in local dips (window bounds inset by the margin).
    [[nodiscard]] Rect cardRect() const { return bounds().inset(margin_); }

    /// Menu size plus the shadow margin on every side.
    Size measure(const Constraints& c) override;
    /// Pins the menu exactly to the card rect after the stack pass.
    void onLayout() override;
    /// Shadow, opaque card and a hairline outline.
    void paintSelf(Canvas& c) override;

private:
    float margin_ = kPopupShadowMargin;
    Widget* menu_ = nullptr;
    std::vector<std::unique_ptr<Widget>> retired_;
};

} // namespace hh::ui
