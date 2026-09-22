// ---------------------------------------------------------------------------
// AmeProcess.h - finding Adobe Media Encoder's process and main window.
//
// isAmeRunning() is portable (AmeProcess.cpp on Windows, mac/AmeProcessMac.cpp
// on macOS); the window lookups feed the HWND-level docking and exist on
// Windows only.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <optional>
#include <string>
#include <vector>

namespace hh::ame {

/// True when the process is alive.
bool isAmeRunning();

#if defined(_WIN32)
struct AmeWindow {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    RECT rect{};
    std::wstring title;
};

/// AME's visible main frame (largest captioned+thick-framed window of "Adobe Media Encoder.exe").
std::optional<AmeWindow> findAmeMainWindow();
/// All top-level windows of AME's process (main frame + floating panel frames).
std::vector<HWND> ameTopLevelWindows(DWORD pid);
/// The "DroverLord - Window Class" name AME uses for panel frames.
const wchar_t* droverLordClassName() noexcept;
#endif

} // namespace hh::ame
