// ---------------------------------------------------------------------------
// PopupWindow.h - a separate, non-activating HWND for menus.
//
// Owned by the main window, WS_EX_NOACTIVATE so keyboard focus stays in the
// main window (which forwards keys here while a popup is open).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/RootView.h"
#include "ui/core/WindowServices.h"
#include "ui/gfx/Canvas.h"
#include "ui/gfx/GraphicsDevice.h"
#include "ui/gfx/SwapChainSurface.h"
#include "ui/gfx/TextCache.h"
#include "ui/theme/ThemeManager.h"
#include "ui/window/FramePacer.h"

#include <functional>
#include <memory>

namespace hh::ui {

class PopupWindow final : public IWindowServices {
public:
    PopupWindow(GraphicsDevice& device, TextCache& text, ThemeManager& themes);
    ~PopupWindow() override;

    /// Creates the (hidden) HWND owned by @p owner.
    bool create(HWND owner);
    void destroy();

    /**
     * @brief Shows @p content at a screen position.
     * @param anchorPx   opener rect in screen pixels
     * @param placement  Below/Above/Auto flip; AtPoint = top-left at anchor origin
     * @param sizeDips   content size (the window adds a shadow margin)
     */
    void show(std::unique_ptr<Widget> content, const RECT& anchorPx, PopupPlacement placement, Size sizeDips,
              std::function<void()> onDismiss);
    void dismiss();
    [[nodiscard]] bool visible() const noexcept { return visible_; }

    void renderFrame();
    [[nodiscard]] RootView& root() noexcept { return *root_; }
    [[nodiscard]] FramePacer& pacer() noexcept { return pacer_; }
    /// Keys forwarded from the owner while the popup is open.
    bool forwardKeyDown(UINT vk, Modifiers mods, bool repeat);
    bool forwardChar(char32_t ch);
    /// The owner reports a click outside our rect (screen px) -> dismiss.
    void ownerClickedAt(POINT screenPx);

    // IWindowServices
    void requestFrame() override { pacer_.requestFrame(); }
    void setCursor(CursorKind cursor) override;
    void captureMouse(bool capture) override;
    [[nodiscard]] HWND hwnd() const override { return hwnd_; }
    [[nodiscard]] DipScale dipScale() const override { return scale_; }
    [[nodiscard]] Size clientSizeDips() const override;
    [[nodiscard]] Size workAreaSizeDips() const override;
    [[nodiscard]] POINT rootToScreenPx(Point rootPt) const override;
    [[nodiscard]] Point screenPxToRoot(POINT pt) const override;
    [[nodiscard]] bool isActiveWindow() const override { return true; }
    void showPopupWindow(std::unique_ptr<Widget>, const Rect&, PopupPlacement, Size, std::function<void()>) override;
    void dismissPopupWindow() override { dismiss(); }
    [[nodiscard]] bool popupWindowVisible() const override { return visible_; }
    void setImeCaret(const Rect&) override {}
    void setImeEnabled(bool) override {}
    bool clipboardSetText(const std::wstring& text) override;
    std::wstring clipboardGetText() override;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void resizeSurface();

    GraphicsDevice& device_;
    TextCache& text_;
    ThemeManager& themes_;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    SwapChainSurface surface_;
    Canvas canvas_;
    std::unique_ptr<RootView> root_;
    FramePacer pacer_;
    DipScale scale_;
    std::function<void()> onDismiss_;
    bool visible_ = false;
    float shadowMargin_ = 24.0f;
};

} // namespace hh::ui
