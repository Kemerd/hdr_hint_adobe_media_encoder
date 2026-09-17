// ---------------------------------------------------------------------------
// PanelWindowLocator.h - finds our CEP panel's HWND inside AME's window tree.
//
// The panel's JS sends its Node process id (the CEF *renderer*). The CEF
// browser process is its parent; the browser's HWNDs are re-parented into
// AME's "DroverLord - Window Class" panel host. We dock to that host.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <optional>
#include <string>

namespace hh::ame {

struct PanelWindows {
    DWORD rendererPid = 0;
    DWORD browserPid = 0;
    HWND cefWindow = nullptr;     ///< outermost CEPHtmlEngine-owned HWND inside AME
    HWND panelHost = nullptr;     ///< nearest DroverLord ancestor (AME process)
    HWND ownerRoot = nullptr;     ///< GetAncestor(GA_ROOT): AME main frame or a floating frame
    RECT rectPx{};                ///< screen rect of cefWindow (physical pixels)
    bool visible = false;         ///< IsWindowVisible(cefWindow)
};

/**
 * @brief Locates the panel windows given the renderer pid from "hello".
 * @param rendererPid   process.pid reported by panel.js
 * @param expectedSizePx optional client size hint (w,h) to disambiguate several panels
 */
std::optional<PanelWindows> locatePanelWindows(DWORD rendererPid, std::optional<SIZE> expectedSizePx = std::nullopt);

/// Refreshes rect/visibility/ownerRoot of a previously located panel. False when the HWND died.
bool refreshPanelWindows(PanelWindows& panel);

/// The CEF browser pid for a renderer pid (parent that is CEPHtmlEngine.exe without "--type="), or 0.
DWORD browserPidForRenderer(DWORD rendererPid);

} // namespace hh::ame
