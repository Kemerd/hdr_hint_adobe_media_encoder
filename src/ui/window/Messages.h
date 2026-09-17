// ---------------------------------------------------------------------------
// Messages.h - private window messages used across the app.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

namespace hh::ui {

inline constexpr UINT WM_HH_ENGINE_EVENTS  = WM_APP + 1;   ///< EngineEventQueue kick
inline constexpr UINT WM_HH_DOCK_TICK      = WM_APP + 2;   ///< WinEvent hook coalesced tick
inline constexpr UINT WM_HH_TRAY           = WM_APP + 3;   ///< Shell_NotifyIcon callback
inline constexpr UINT WM_HH_SECOND_INSTANCE= WM_APP + 4;   ///< forwarded argv arrived
inline constexpr UINT WM_HH_DEBUG_DRIVE    = WM_APP + 5;   ///< scripted state changes (--screenshot)
inline constexpr UINT WM_HH_RECREATE       = WM_APP + 6;   ///< recreate the HWND (owner destroyed us)
inline constexpr UINT WM_HH_TICK           = WM_APP + 7;   ///< 1 Hz housekeeping timer
inline constexpr UINT WM_HH_POPUP_DISMISS  = WM_APP + 8;   ///< popup window asks to close

} // namespace hh::ui
