// ---------------------------------------------------------------------------
// WinVersion.h - OS build detection without the deprecated GetVersionEx.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

namespace hh::platform {

/// Windows build number (e.g. 22631), via RtlGetVersion.
DWORD windowsBuildNumber();
/// Build >= 22000.
bool isWindows11();
/// Build >= 22621: DWMWA_SYSTEMBACKDROP_TYPE (Mica) is available.
bool supportsSystemBackdrop();
/// True inside a Remote Desktop session.
bool isRemoteSession();

} // namespace hh::platform
