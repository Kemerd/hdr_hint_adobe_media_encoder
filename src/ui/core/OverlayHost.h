// ---------------------------------------------------------------------------
// OverlayHost.h - popups (separate HWND), tooltips and toasts above content.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"
#include "ui/core/WindowServices.h"

#include <functional>
#include <memory>
#include <string>

namespace hh::ui {

class ToastHost;

/**
 * @brief The layer above the content of a RootView.
 *
 * Popups (menus) are hosted in a separate popup HWND so a small docked panel
 * cannot clip them; the OverlayHost only tracks that one is open and routes
 * keyboard input to it. Tooltips and toasts are ordinary children of this
 * widget and paint inside the window.
 */
class OverlayHost : public Widget {
public:
    OverlayHost();

    /// Opens a popup window with @p content anchored to a root-space rect.
    void showPopup(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                   std::function<void()> onDismiss = {});
    void dismissPopup();
    [[nodiscard]] bool hasPopup() const noexcept { return popupOpen_; }

    /// Tooltip near a root-space point (shown after the hover delay by RootView).
    void showTooltip(const std::wstring& text, Point rootAnchor);
    void hideTooltip();
    [[nodiscard]] bool tooltipVisible() const noexcept;

    /// The toast stack (bottom-centre).
    [[nodiscard]] ToastHost& toasts() noexcept { return *toasts_; }

    Size measure(const Constraints& c) override;
    void layout(const Rect& frame) override;
    Widget* hitTest(Point local) override;

private:
    class TooltipView;
    bool popupOpen_ = false;
    TooltipView* tooltip_ = nullptr;
    ToastHost* toasts_ = nullptr;
    std::function<void()> popupDismiss_;
};

} // namespace hh::ui
