// ---------------------------------------------------------------------------
// RootView.h - the top of a widget tree: owns the Timeline, routes input,
// runs layout and paints. One per HWND (main window and popup window).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/anim/Timeline.h"
#include "ui/core/FocusManager.h"
#include "ui/core/OverlayHost.h"
#include "ui/core/Widget.h"
#include "ui/core/WindowServices.h"
#include "ui/theme/ThemeManager.h"

#include <functional>
#include <memory>
#include <vector>

namespace hh::ui {

/// Non-client hit result for custom chrome (floating window caption/buttons).
enum class ChromeHit { None, Caption, Minimize, Maximize, Close };

/// Implemented by the content widget that draws the title bar.
class IChromeProvider {
public:
    virtual ~IChromeProvider() = default;
    virtual ChromeHit chromeHitTest(Point rootPt) const = 0;
    /// The maximize button rect in root dips (for Snap Layouts hover), empty if none.
    virtual Rect chromeMaximizeRect() const = 0;
    virtual void setChromeMaximizeHover(bool hover) = 0;
};

class RootView final : public Widget {
public:
    RootView(IWindowServices& window, ThemeManager& themes);
    ~RootView() override;

    // ---- composition --------------------------------------------------------
    /// Replaces the content widget (below the overlay).
    void setContent(std::unique_ptr<Widget> content);
    [[nodiscard]] Widget* content() const noexcept { return content_; }
    void setChromeProvider(IChromeProvider* p) noexcept { chrome_ = p; }
    [[nodiscard]] OverlayHost& overlay() noexcept { return *overlay_; }
    [[nodiscard]] Timeline& timeline() noexcept { return timeline_; }
    [[nodiscard]] const Theme& theme() const noexcept { return themes_.current(); }
    [[nodiscard]] ThemeManager& themes() noexcept { return themes_; }
    [[nodiscard]] FocusManager& focus() noexcept { return focus_; }
    [[nodiscard]] IWindowServices& window() noexcept { return window_; }

    // ---- frame --------------------------------------------------------------
    /// Runs layout when dirty for the given size (dips).
    void layoutIfNeeded(Size size);
    /// Paints the whole tree.
    void paintAll(Canvas& c);
    /// Samples animations/timers; returns true when a repaint is needed.
    bool tickAnimations();
    void requestFrame();
    /// Widgets registered for per-frame ticks (spinners) - called by tickAnimations.
    void registerTicker(Widget* w, bool on);

    // ---- input (root dips) --------------------------------------------------
    void dispatchMouseMove(Point rootPt, Modifiers mods);
    void dispatchMouseDown(Point rootPt, MouseButton button, Modifiers mods, int clickCount);
    void dispatchMouseUp(Point rootPt, MouseButton button, Modifiers mods);
    void dispatchMouseLeave();
    void dispatchWheel(Point rootPt, float delta, float deltaX, bool precise, int linesPerNotch, Modifiers mods);
    bool dispatchKeyDown(UINT vk, Modifiers mods, bool repeat);
    bool dispatchKeyUp(UINT vk, Modifiers mods);
    bool dispatchChar(char32_t ch);
    /// WM_CANCELMODE / WM_CAPTURECHANGED / deactivate: clears press/hover, dismisses popups.
    void cancelInteraction();
    void setWindowActive(bool active);
    [[nodiscard]] bool windowActive() const noexcept { return windowActive_; }
    [[nodiscard]] CursorKind currentCursor() const;
    [[nodiscard]] Widget* hoveredWidget() const noexcept { return hovered_; }
    [[nodiscard]] Widget* capturedWidget() const noexcept { return captured_; }

    /// Chrome hit-test for WM_NCHITTEST (None when no provider).
    [[nodiscard]] ChromeHit chromeHitTest(Point rootPt) const;
    [[nodiscard]] IChromeProvider* chromeProvider() const noexcept { return chrome_; }

    /// Global shortcuts handled before widgets (registered by the app shell).
    std::function<bool(const KeyEvent&)> onShortcut;
    /// Called when the theme changed (after widgets were notified).
    std::function<void()> onThemeChanged;

    /// Notifies every widget of a theme change and repaints.
    void themeChanged();
    void dpiChanged();
    /// Called by Widget::removeChild so hover/capture/focus never dangle.
    void widgetRemoved(Widget* w);

private:
    friend class Widget;
    void updateHover(Point rootPt, Modifiers mods);
    void deliverEnterLeave(Widget* from, Widget* to);
    Widget* hitAtRoot(Point rootPt);
    void startTooltipTimer(Widget* w, Point rootPt);
    void cancelTooltipTimer();

    IWindowServices& window_;
    ThemeManager& themes_;
    Timeline timeline_;
    FocusManager focus_;
    Widget* content_ = nullptr;
    OverlayHost* overlay_ = nullptr;
    IChromeProvider* chrome_ = nullptr;
    Widget* hovered_ = nullptr;
    Widget* captured_ = nullptr;
    Widget* pressedWidget_ = nullptr;
    Point lastMouseRoot_;
    bool mouseInside_ = false;
    bool windowActive_ = true;
    std::vector<Widget*> tickers_;
    TimerId tooltipTimer_ = 0;
    Widget* tooltipWidget_ = nullptr;
    Size lastSize_;
};

} // namespace hh::ui
