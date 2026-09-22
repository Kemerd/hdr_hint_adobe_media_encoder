// ---------------------------------------------------------------------------
// MacWindowHost.h - the main window on macOS.
//
// A standard titled NSWindow with a full-size content view and a transparent
// title bar: the toolkit draws the whole window (top bar included) over an
// NSVisualEffectView backdrop, while the system traffic lights stay native
// and are re-seated vertically centred in the 44 pt top bar - the layout of
// Apple's own unified-toolbar windows. The caption area of the top bar drags
// the window through the window server, and double-clicks follow the user's
// "double-click a window's title bar to" preference.
//
// Frames are demand-driven (see mac::FrameClock): nothing runs while the UI
// is idle, animations tick at 60 Hz, and timeline timers wake the loop
// exactly when they are due.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/RootView.h"
#include "ui/core/WindowServices.h"
#include "ui/gfx/Canvas.h"
#include "ui/gfx/TextCache.h"
#include "ui/mac/MacPopupWindow.h"
#include "ui/mac/MacView.h"
#include "ui/theme/ThemeManager.h"
#include "ui/window/FramePacer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hh::ui {

/**
 * @brief How the main window starts out.
 */
struct MacWindowSpec {
    std::wstring title = L"HDR Hint";
    Size initialDips{520, 680};      ///< content size; the window is centred on the main screen
    Size minSizeDips{360, 420};
    bool alwaysOnTop = true;         ///< floating window level
    float topBarHeight = 44.0f;      ///< the content's top bar: the traffic lights are centred in it
    bool acceptsFileDrops = true;
};

class MacWindowHost final : public IWindowServices, private mac::IViewSink {
public:
    MacWindowHost(TextCache& text, ThemeManager& themes);
    ~MacWindowHost() override;
    MacWindowHost(const MacWindowHost&) = delete;
    MacWindowHost& operator=(const MacWindowHost&) = delete;

    /// Creates the (hidden) window, its backdrop, canvas view and popup panel.
    bool create(const MacWindowSpec& spec);
    void destroy();

    [[nodiscard]] RootView& root() noexcept { return *root_; }
    [[nodiscard]] MacPopupWindow& popup() noexcept { return *popup_; }
    /// The NSWindow* (opaque; nullptr before create()).
    [[nodiscard]] void* nsWindow() const noexcept;

    // ---- window state -------------------------------------------------------
    void show(bool activate);
    void hide();
    void minimize();
    /// Zoom (the green button's "fill" behaviour).
    void toggleZoom();
    /// Visible on screen or in the Dock (miniaturised) - IsWindowVisible parity.
    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool minimized() const;
    [[nodiscard]] bool zoomed() const;
    void bringToFront();
    void setAlwaysOnTop(bool on);
    /// Re-evaluates translucency (Reduce Transparency) and the window appearance.
    void refreshBackdrop();
    /// Room (dips) the content leaves for the traffic lights (0 in full screen).
    [[nodiscard]] float trafficLightsInset() const noexcept { return trafficLightsInset_ > 0.0f ? trafficLightsInset_ : 0.0f; }

    // ---- placement ----------------------------------------------------------
    /// "mac:<NSWindow saved-frame string>".
    [[nodiscard]] std::wstring placementString() const;
    /// Restores a placement string; unusable ones centre the window on the main screen.
    void applyPlacement(const std::wstring& placement);

    // ---- frames -------------------------------------------------------------
    /// Starts the 1 Hz housekeeping tick (onTick).
    void startTicking();
    /**
     * @brief Renders the current UI offscreen and returns premultiplied BGRA.
     * @param backingScale  pixels per dip; 0 = the window's own backing scale
     */
    bool captureFrame(std::vector<uint8_t>& bgra, UINT& w, UINT& h, float backingScale = 0.0f);

    // ---- callbacks ----------------------------------------------------------
    std::function<void()> onCloseRequested;                  ///< red button / Cmd+W (the app decides hide vs quit)
    std::function<void()> onSystemSettingsChanged;           ///< appearance / accent / accessibility changes
    std::function<void(const std::vector<std::wstring>&)> onFilesDropped;
    std::function<void(bool)> onActivate;
    std::function<void()> onDpiChanged;
    std::function<void()> onTick;                            ///< 1 Hz
    /// The content must reserve @p leadingInset dips for the traffic lights (0 in full screen).
    std::function<void(float leadingInset)> onTrafficLightsChanged;

    // ---- IWindowServices ----------------------------------------------------
    void requestFrame() override;
    void setCursor(CursorKind cursor) override;
    void captureMouse(bool capture) override { static_cast<void>(capture); }
    [[nodiscard]] DipScale dipScale() const override { return scale_; }
    [[nodiscard]] Size clientSizeDips() const override;
    [[nodiscard]] Size workAreaSizeDips() const override;
    [[nodiscard]] bool isActiveWindow() const override;
    void showPopupWindow(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                         Size contentSize, std::function<void()> onDismiss) override;
    void dismissPopupWindow() override;
    [[nodiscard]] bool popupWindowVisible() const override;
    void setImeCaret(const Rect& caretRoot) override;
    void setImeEnabled(bool enabled) override;
    bool clipboardSetText(const std::wstring& text) override;
    std::wstring clipboardGetText() override;

    // ---- called by the Cocoa delegate (not for app code) --------------------
    void handleCloseRequest();
    void handleActivation(bool active);
    void handleResize();
    void handleMiniaturized(bool miniaturized);
    void handleFullScreen(bool fullScreen);
    void handleSystemSettingsChanged();
    /// Re-seats the traffic lights in the top bar (AppKit resets them on resize).
    void layoutTrafficLights();

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
    [[nodiscard]] Rect viewImeCaret() const override { return imeCaret_; }
    [[nodiscard]] CursorKind viewCursor() const override;
    void viewFilesDropped(const std::vector<std::wstring>& paths) override;
    [[nodiscard]] bool viewAcceptsFileDrops() const override { return spec_.acceptsFileDrops; }
    void viewBackingChanged(float backingScale) override;
    void viewResized(Size sizeDips) override;
    void viewWillDraw() override { layoutTrafficLights(); }

    /// Paints the whole tree into @p ctx (window and offscreen captures share it).
    void paint(CGContextRef ctx, Size sizeDips);
    /// Timer callback: samples animations and schedules the next frame.
    void pump();
    /// Title-bar double-click, as System Settings > Desktop & Dock says.
    void titleBarDoubleClick();
    /// Centres the window on the main screen at the spec size.
    void centreOnMainScreen();

    struct Objc;                          ///< window, views, delegate, observers
    std::unique_ptr<Objc> objc_;
    TextCache& text_;
    ThemeManager& themes_;
    MacWindowSpec spec_;
    Canvas canvas_;
    std::unique_ptr<RootView> root_;
    std::unique_ptr<MacPopupWindow> popup_;
    FramePacer pacer_;
    std::unique_ptr<mac::FrameClock> clock_;
    mac::RepeatingTimer tickTimer_;
    DipScale scale_;
    Rect imeCaret_;
    float trafficLightsInset_ = 0.0f;
    bool fullScreen_ = false;
    bool alwaysOnTop_ = true;
};

} // namespace hh::ui
