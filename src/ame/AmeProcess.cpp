// ---------------------------------------------------------------------------
// AmeProcess.cpp - finding Adobe Media Encoder's process and main window.
//
// AME's main frame is an ordinary captioned, thick-framed top-level window
// whose class name is version-suffixed ("Adobe Media Encoder 2026"), so the
// finder never looks at the class. It keys on the owning process's image
// name plus the window title and picks the largest survivor: the splash
// screen and floating panel frames are always smaller than the main frame.
// ---------------------------------------------------------------------------
#include "ame/AmeProcess.h"

#include "core/Logger.h"
#include "platform/Process.h"
#include "platform/Utf.h"

#include <algorithm>
#include <format>
#include <unordered_map>

namespace hh::ame {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"AmeProcess";

/// Image file name of the Media Encoder process (compared case-insensitively).
constexpr std::wstring_view kAmeImageName = L"Adobe Media Encoder.exe";

/// Substring every AME main-frame title carries ("Adobe Media Encoder 2026").
constexpr std::wstring_view kAmeTitleNeedle = L"Adobe Media Encoder";

/// Class name AME gives every docked/floating panel frame.
constexpr const wchar_t* kDroverLordClass = L"DroverLord - Window Class";

/// Windows smaller than this (in either dimension) are never the main frame.
constexpr LONG kMinMainFrameEdge = 200;

/// Upper bound on the title text we read per window.
constexpr int kMaxTitleChars = 512;

/**
 * @brief Everything the EnumWindows callback needs while looking for the
 *        main frame.
 *
 * Process image names are cached per pid because a single AME process owns
 * dozens of top-level windows and OpenProcess is comparatively expensive.
 */
struct MainWindowSearch {
    std::unordered_map<DWORD, bool> isAmeByPid;   ///< pid -> "image name is AME"
    std::optional<AmeWindow> best;                ///< largest match so far
    long long bestArea = 0;                       ///< area of @c best in px^2
};

/**
 * @brief True when @p pid belongs to "Adobe Media Encoder.exe", memoised.
 */
bool pidIsAme(MainWindowSearch& search, DWORD pid)
{
    // A zero pid means GetWindowThreadProcessId failed; never a match.
    if (pid == 0) {
        return false;
    }

    // Reuse the answer when we have seen this pid already.
    const auto cached = search.isAmeByPid.find(pid);
    if (cached != search.isAmeByPid.end()) {
        return cached->second;
    }

    // First sighting: ask the process for its image name and remember it.
    const std::wstring image = platform::processImageName(pid);
    const bool isAme = !image.empty() && platform::iequals(image, kAmeImageName);
    search.isAmeByPid.emplace(pid, isAme);
    return isAme;
}

/**
 * @brief Reads a window's title into a wide string (empty on failure).
 */
std::wstring windowTitle(HWND hwnd)
{
    if (hwnd == nullptr) {
        return {};
    }

    // GetWindowTextW returns the copied length; zero covers "no title" and
    // "window gone" alike, both of which we treat as an empty title.
    wchar_t buffer[kMaxTitleChars]{};
    const int copied = ::GetWindowTextW(hwnd, buffer, kMaxTitleChars);
    if (copied <= 0) {
        return {};
    }
    return std::wstring(buffer, static_cast<size_t>(std::min(copied, kMaxTitleChars - 1)));
}

/**
 * @brief EnumWindows callback for findAmeMainWindow().
 *
 * Every filter is cheap-first: visibility and styles come from the window
 * manager without touching the other process; the image-name check (which
 * opens a process handle) runs last.
 */
BOOL CALLBACK enumMainWindowProc(HWND hwnd, LPARAM lparam)
{
    auto* search = reinterpret_cast<MainWindowSearch*>(lparam);
    if (search == nullptr || hwnd == nullptr) {
        return TRUE;
    }

    // Only visible, non-tool windows can be the frame the user sees.
    if (!::IsWindowVisible(hwnd)) {
        return TRUE;
    }
    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }

    // The main frame is captioned; AME's tooltips and drag images are not.
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != WS_CAPTION) {
        return TRUE;
    }

    // Tiny windows (splash fragments, helpers) are never the main frame.
    RECT rect{};
    if (!::GetWindowRect(hwnd, &rect)) {
        return TRUE;
    }
    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (width < kMinMainFrameEdge || height < kMinMainFrameEdge) {
        return TRUE;
    }

    // The window must belong to the Media Encoder process.
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (!pidIsAme(*search, pid)) {
        return TRUE;
    }

    // And carry the product name in its title (class names are versioned).
    const std::wstring title = windowTitle(hwnd);
    if (!platform::icontains(title, kAmeTitleNeedle)) {
        return TRUE;
    }

    // Keep the largest survivor: floating panel frames are smaller.
    const long long area = static_cast<long long>(width) * static_cast<long long>(height);
    if (!search->best.has_value() || area > search->bestArea) {
        AmeWindow found;
        found.hwnd = hwnd;
        found.pid = pid;
        found.rect = rect;
        found.title = title;
        search->best = std::move(found);
        search->bestArea = area;
    }
    return TRUE;
}

/**
 * @brief State for the per-pid top-level window enumeration.
 */
struct TopLevelSearch {
    DWORD pid = 0;
    std::vector<HWND> windows;
};

/**
 * @brief EnumWindows callback for ameTopLevelWindows().
 */
BOOL CALLBACK enumTopLevelProc(HWND hwnd, LPARAM lparam)
{
    auto* search = reinterpret_cast<TopLevelSearch*>(lparam);
    if (search == nullptr || hwnd == nullptr) {
        return TRUE;
    }

    // Keep every top-level window owned by the requested process, visible
    // or not: a hidden floating frame can still host our panel briefly.
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0 && pid == search->pid) {
        search->windows.push_back(hwnd);
    }
    return TRUE;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * @brief Finds AME's visible main frame.
 *
 * Enumerates every top-level window and keeps the largest one that is
 * visible, captioned, at least 200x200 px, owned by "Adobe Media Encoder.exe"
 * and titled "Adobe Media Encoder ...".
 */
std::optional<AmeWindow> findAmeMainWindow()
{
    MainWindowSearch search;

    // EnumWindows only fails when the callback returns FALSE (ours never
    // does) or the desktop is unavailable; log and report "not found".
    if (!::EnumWindows(&enumMainWindowProc, reinterpret_cast<LPARAM>(&search))) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_SUCCESS) {
            HH_LOG_WARN(kLog, L"EnumWindows failed while looking for the AME frame (error {})", error);
        }
    }

    if (search.best.has_value()) {
        HH_LOG_DEBUG(kLog, L"AME main window {:#x} pid {} '{}' {}x{}",
                     reinterpret_cast<uintptr_t>(search.best->hwnd), search.best->pid, search.best->title,
                     search.best->rect.right - search.best->rect.left,
                     search.best->rect.bottom - search.best->rect.top);
    }
    return search.best;
}

/**
 * @brief Lists all top-level windows of a process (main frame + floating
 *        panel frames + helpers).
 */
std::vector<HWND> ameTopLevelWindows(DWORD pid)
{
    // A zero pid would match every window whose lookup fails; refuse it.
    if (pid == 0) {
        HH_LOG_WARN(kLog, L"ameTopLevelWindows called with pid 0");
        return {};
    }

    TopLevelSearch search;
    search.pid = pid;
    if (!::EnumWindows(&enumTopLevelProc, reinterpret_cast<LPARAM>(&search))) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_SUCCESS) {
            HH_LOG_WARN(kLog, L"EnumWindows failed for pid {} (error {})", pid, error);
        }
    }
    return search.windows;
}

/**
 * @brief True while at least one "Adobe Media Encoder.exe" process exists.
 */
bool isAmeRunning()
{
    return !platform::findProcessesByImageName(kAmeImageName).empty();
}

/**
 * @brief Class name of AME's panel frames.
 */
const wchar_t* droverLordClassName() noexcept
{
    return kDroverLordClass;
}

} // namespace hh::ame
