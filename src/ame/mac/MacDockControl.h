// ---------------------------------------------------------------------------
// MacDockControl.h - IDockControl for macOS: docking is not available.
//
// Windows glues the HWND onto Media Encoder's panel frame through cross-
// process window ownership. macOS has no equivalent (another app's windows
// cannot own ours), so the Mac build always floats: supported() is false,
// which hides every dock toggle, and the user actions are harmless no-ops.
// ---------------------------------------------------------------------------
#pragma once

#include "ame/DockControl.h"
#include "core/Logger.h"

namespace hh::ame {

class MacDockControl final : public IDockControl {
public:
    /// Never docked.
    [[nodiscard]] bool docked() const noexcept override { return false; }
    /// The dock toggles stay hidden.
    [[nodiscard]] bool supported() const noexcept override { return false; }

    /// Dock requests (panel command, forwarded --dock) are logged and ignored.
    void userDock() override {
        HH_LOG_INFO(L"Dock", L"docking into Media Encoder is not available on macOS; the window stays floating");
    }
    void userUndock() override {}
};

} // namespace hh::ame
