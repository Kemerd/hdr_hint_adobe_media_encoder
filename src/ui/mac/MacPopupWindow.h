// ---------------------------------------------------------------------------
// MacPopupWindow.h - menus in their own borderless, non-activating NSPanel.
//
// The macOS counterpart of the Win32 PopupWindow: a transparent panel at pop-
// up-menu level that hosts a PopupSurface (shadow + card + menu), so a menu
// can hang over the window edge and never steals key status from the main
// window, which forwards its keys here while the popup is open.
//
// Outside clicks (in this app or any other), the app losing focus and the
// owner moving all dismiss the popup, like an NSMenu.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/RootView.h"
#include "ui/core/WindowServices.h"
#include "ui/gfx/Canvas.h"
#include "ui/gfx/TextCache.h"
#include "ui/mac/MacView.h"
#include "ui/theme/ThemeManager.h"
#include "ui/window/FramePacer.h"

#include <functional>
#include <memory>

namespace hh::ui {

class MacPopupWindow final : public IWindowServices, private mac::IViewSink {
public:
    MacPopupWindow(TextCache& text, ThemeManager& themes);
    ~MacPopupWindow() override;
    MacPopupWindow(const MacPopupWindow&) = delete;
    MacPopupWindow& operator=(const MacPopupWindow&) = delete;

    /// Creates the (hidden) panel; @p ownerWindow is the NSWindow* it belongs to.
    bool create(void* ownerWindow);
    void destroy();

    /**
     * @brief Shows @p content next to an anchor.
     * @param anchorScreen  opener rect in Cocoa screen points (origin bottom-left)
     * @param placement     Below/Above/Auto flip; AtPoint = top-left at the anchor's top-left
     * @param sizeDips      content size (the panel adds the shadow margin)
     */
    void show(std::unique_ptr<Widget> content, CGRect anchorScreen, PopupPlacement placement, Size sizeDips,
              std::function<void()> onDismiss);
    void dismiss();
    [[nodiscard]] bool visible() const noexcept { return visible_; }

    [[nodiscard]] RootView& root() noexcept { return *root_; }
    /// Keys forwarded from the owner while the popup is open.
    bool forwardKeyDown(UINT vk, Modifiers mods, bool repeat);
    bool forwardChar(char32_t ch);

    // ---- IWindowServices ----------------------------------------------------
    void requestFrame() override;
    void setCursor(CursorKind cursor) override;
    void captureMouse(bool capture) override { static_cast<void>(capture); }
    [[nodiscard]] DipScale dipScale() const override { return scale_; }
    [[nodiscard]] Size clientSizeDips() const override;
    [[nodiscard]] Size workAreaSizeDips() const override;
    [[nodiscard]] bool isActiveWindow() const override { return true; }
    void showPopupWindow(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                         Size contentSize, std::function<void()> onDismiss) override;
    void dismissPopupWindow() override { dismiss(); }
    [[nodiscard]] bool popupWindowVisible() const override { return visible_; }
    void setImeCaret(const Rect& caretRoot) override { static_cast<void>(caretRoot); }
    void setImeEnabled(bool enabled) override { static_cast<void>(enabled); }
    bool clipboardSetText(const std::wstring& text) override;
    std::wstring clipboardGetText() override;

private:
    // ---- mac::IViewSink ------------------------------------------------------
    void viewDraw(CGContextRef ctx, Size sizeDips) override;
    void viewMouseMove(Point pt, Modifiers mods) override;
    bool viewMouseDown(Point pt, MouseButton button, Modifiers mods, int clickCount) override;
    void viewMouseUp(Point pt, MouseButton button, Modifiers mods) override;
    void viewMouseExited() override;
    void viewWheel(Point pt, float delta, float deltaX, bool precise, Modifiers mods) override;
    bool viewKeyDown(UINT vk, Modifiers mods, bool repeat) override;
    bool viewKeyUp(UINT vk, Modifiers mods) override;
    void viewChar(char32_t ch) override;
    [[nodiscard]] Rect viewImeCaret() const override { return {}; }
    [[nodiscard]] CursorKind viewCursor() const override;
    void viewBackingChanged(float backingScale) override;

    /// Timer callback: samples animations and schedules the next frame.
    void pump();
    /// True when a root point lies on the card (the shadow margin is "outside").
    [[nodiscard]] bool insideCard(Point rootPt) const;
    void installMonitors();
    void removeMonitors();

    struct Objc;                          ///< the Cocoa objects (panel, view, monitors)
    std::unique_ptr<Objc> objc_;
    TextCache& text_;
    ThemeManager& themes_;
    Canvas canvas_;
    std::unique_ptr<RootView> root_;
    FramePacer pacer_;
    std::unique_ptr<mac::FrameClock> clock_;
    DipScale scale_;
    std::function<void()> onDismiss_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);   ///< guards deferred work
    bool visible_ = false;
    float shadowMargin_ = 24.0f;
};

} // namespace hh::ui
