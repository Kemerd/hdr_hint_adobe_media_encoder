// ---------------------------------------------------------------------------
// WindowHost.h - the main borderless window (floating or docked in AME).
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
#include "ui/window/PopupWindow.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hh::ui {

enum class WindowMode { Floating, Docked };

struct WindowSpec {
    std::wstring title = L"HDR Hint";
    std::wstring className = L"HdrHint.MainWindow";
    Rect initialDips{-1, -1, 520, 680};     ///< x/y < 0 = centre on the primary monitor
    Size minSizeDips{360, 420};
    bool alwaysOnTop = true;
    bool startHidden = false;
    HICON icon = nullptr;
};

/// Backdrop actually in use (for the theme's translucency decision).
enum class Backdrop { None, Mica, MicaLegacy };

class WindowHost final : public IWindowServices {
public:
    WindowHost(GraphicsDevice& device, TextCache& text, ThemeManager& themes);
    ~WindowHost() override;

    bool create(const WindowSpec& spec);
    void destroy();
    /// Destroys and re-creates the HWND (used after an owner destroyed it).
    bool recreate(WindowMode mode);

    [[nodiscard]] HWND hwnd() const override { return hwnd_; }
    [[nodiscard]] RootView& root() noexcept { return *root_; }
    [[nodiscard]] PopupWindow& popup() noexcept { return *popup_; }

    void show(bool activate);
    void hide();
    void minimize();
    void toggleMaximize();
    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool minimized() const;
    void bringToFront();

    // ---- modes --------------------------------------------------------------
    /// Floating -> Docked: owner = AME root window, bounds in screen pixels.
    void setMode(WindowMode mode, HWND owner, const RECT& dockBoundsPx);
    [[nodiscard]] WindowMode mode() const noexcept { return mode_; }
    void setDockBounds(const RECT& px);
    void setDockVisible(bool visible);
    [[nodiscard]] HWND owner() const noexcept { return owner_; }
    void setAlwaysOnTop(bool on);
    void setMinSize(Size dips);
    [[nodiscard]] Backdrop backdrop() const noexcept { return backdrop_; }
    /// Re-evaluates Mica availability (composition / session / transparency changes).
    void refreshBackdrop();

    // ---- placement ----------------------------------------------------------
    /// "x,y,w,h,dpi,max" (px) of the floating placement.
    [[nodiscard]] std::wstring placementString() const;
    void applyPlacement(const std::wstring& placement);

    // ---- frame --------------------------------------------------------------
    void renderFrame();
    /// Runs the message loop until WM_QUIT. @p onTick is called ~1 Hz.
    int runLoop(const std::function<void()>& onTick);
    /// Posts WM_QUIT.
    void quit(int code = 0);
    /// Captures the current frame as BGRA pixels (for --screenshot).
    bool captureFrame(std::vector<uint8_t>& bgra, UINT& w, UINT& h);

    // ---- callbacks ----------------------------------------------------------
    std::function<void()> onCloseRequested;                  ///< X / Alt+F4 (the app decides hide vs quit)
    std::function<void()> onDestroyedByOwner;                ///< unsolicited WM_DESTROY (AME quit)
    std::function<bool(UINT, WPARAM, LPARAM)> onAppMessage;  ///< WM_APP+ messages; return true when handled
    std::function<void()> onSystemSettingsChanged;           ///< theme/colour/animation setting changes
    std::function<void(const std::vector<std::wstring>&)> onFilesDropped;
    std::function<void(bool)> onActivate;
    std::function<void()> onDpiChanged;
    std::function<void()> onTick;                            ///< 1 Hz

    // ---- IWindowServices ----------------------------------------------------
    void requestFrame() override { pacer_.requestFrame(); }
    void setCursor(CursorKind cursor) override;
    void captureMouse(bool capture) override;
    [[nodiscard]] DipScale dipScale() const override { return scale_; }
    [[nodiscard]] Size clientSizeDips() const override;
    [[nodiscard]] Size workAreaSizeDips() const override;
    [[nodiscard]] POINT rootToScreenPx(Point rootPt) const override;
    [[nodiscard]] Point screenPxToRoot(POINT pt) const override;
    [[nodiscard]] bool isActiveWindow() const override;
    void showPopupWindow(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                         Size contentSize, std::function<void()> onDismiss) override;
    void dismissPopupWindow() override;
    [[nodiscard]] bool popupWindowVisible() const override;
    void setImeCaret(const Rect& caretRoot) override;
    void setImeEnabled(bool enabled) override;
    bool clipboardSetText(const std::wstring& text) override;
    std::wstring clipboardGetText() override;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT onNcCalcSize(WPARAM wp, LPARAM lp);
    LRESULT onNcHitTest(LPARAM lp);
    void onSize(UINT pxW, UINT pxH);
    void applyDwmAttributes();
    void applyStylesForMode();
    bool createHwnd(WindowMode mode, HWND owner);
    void handleDeviceLost();
    void savePlacementIfFloating();
    void setMaximizeHover(bool on);

    GraphicsDevice& device_;
    TextCache& text_;
    ThemeManager& themes_;
    WindowSpec spec_;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    WindowMode mode_ = WindowMode::Floating;
    Backdrop backdrop_ = Backdrop::None;
    SwapChainSurface surface_;
    Canvas canvas_;
    std::unique_ptr<RootView> root_;
    std::unique_ptr<PopupWindow> popup_;
    FramePacer pacer_;
    DipScale scale_;
    RECT dockBounds_{};
    bool dockVisible_ = true;
    bool destroyingSelf_ = false;
    bool inSizeMove_ = false;
    bool trackingMouse_ = false;
    bool trackingNcMouse_ = false;
    bool maximizeHover_ = false;
    bool alwaysOnTop_ = true;
    bool firstFramePresented_ = false;
    std::wstring floatingPlacement_;
    UINT_PTR sizeMoveTimer_ = 0;
    UINT_PTR tickTimer_ = 0;
    HCURSOR currentCursor_ = nullptr;
    wchar_t highSurrogate_ = 0;
};

} // namespace hh::ui
