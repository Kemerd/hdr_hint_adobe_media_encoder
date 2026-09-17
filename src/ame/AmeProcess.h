// ---------------------------------------------------------------------------
// AmeProcess.h - finding Adobe Media Encoder's process and main window.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <optional>
#include <string>
#include <vector>

namespace hh::ame {

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
/// True when the process is alive.
bool isAmeRunning();
/// The "DroverLord - Window Class" name AME uses for panel frames.
const wchar_t* droverLordClassName() noexcept;

} // namespace hh::ame
