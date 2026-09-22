// ---------------------------------------------------------------------------
// DockControl.h - what the UI needs from "the thing that docks us into AME".
//
// Windows: DockController glues the HWND onto Media Encoder's panel frame
// (owned-window overlay, WinEvent tracking). macOS has no cross-process
// window ownership, so the Mac build uses a controller that reports "never
// docked" and the screens hide their dock toggles (supported() == false).
// ---------------------------------------------------------------------------
#pragma once

#include <functional>

namespace hh::ame {

enum class DockState { Undocked, Searching, Docked, Suspended, Picking };

/**
 * @brief The docking surface shared by both platforms' controllers.
 */
class IDockControl {
public:
    virtual ~IDockControl() = default;

    /// True while the window sits inside Media Encoder.
    [[nodiscard]] virtual bool docked() const noexcept = 0;
    /// True when this platform can dock at all (drives the dock toggles' visibility).
    [[nodiscard]] virtual bool supported() const noexcept = 0;
    /// User pressed Dock / Undock (panel, footer switch, tray or command line).
    virtual void userDock() = 0;
    virtual void userUndock() = 0;

    /// Fired on every state change (UI thread).
    std::function<void(DockState)> onStateChanged;
};

} // namespace hh::ame
