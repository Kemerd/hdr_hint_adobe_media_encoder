// ---------------------------------------------------------------------------
// PanelWindowLocator.cpp - finds our CEP panel's HWND inside AME's window tree.
//
// One CEP panel is several CEPHtmlEngine.exe processes: a browser process
// plus "--type=renderer" / "--type=gpu-process" children. Node runs in the
// renderer, so the pid the panel reports owns no window at all. The browser
// (its parent) owns the HWNDs, and AME re-parents those into a
// "DroverLord - Window Class" panel host. We find the outermost browser-owned
// HWND under any AME frame and dock to that.
// ---------------------------------------------------------------------------
#include "ame/PanelWindowLocator.h"

#include "ame/AmeProcess.h"
#include "core/Logger.h"
#include "platform/Process.h"
#include "platform/Utf.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <vector>

namespace hh::ame {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"PanelLocator";

/// Image name of every CEF process CEP spawns.
constexpr std::wstring_view kCefImageName = L"CEPHtmlEngine.exe";

/// Switch that marks a CEF subprocess (renderer, gpu, utility ...).
constexpr std::wstring_view kSubprocessSwitch = L"--type=";

/// Tolerance when matching the JS-reported client size against a HWND.
constexpr LONG kSizeTolerancePx = 4;

/// Guard against pathological parent chains.
constexpr int kMaxParentWalk = 64;

/// Longest class name we read (the longest real one is ~50 chars).
constexpr int kMaxClassChars = 256;

/**
 * @brief Class name of a window (empty on failure).
 */
std::wstring className(HWND hwnd)
{
    if (hwnd == nullptr) {
        return {};
    }
    wchar_t buffer[kMaxClassChars]{};
    const int copied = ::GetClassNameW(hwnd, buffer, kMaxClassChars);
    if (copied <= 0) {
        return {};
    }
    return std::wstring(buffer, static_cast<size_t>(std::min(copied, kMaxClassChars - 1)));
}

/**
 * @brief Owning pid of a window (0 when the window is gone).
 */
DWORD windowPid(HWND hwnd)
{
    if (hwnd == nullptr) {
        return 0;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

/**
 * @brief True when @p pid is a CEPHtmlEngine.exe *browser* process, i.e. the
 *        image name matches and its command line carries no "--type=".
 *
 * An unreadable command line (different session, process just exited) is
 * treated as "not a subprocess": the caller cross-checks with the window
 * tree anyway, and the browser is the only CEF process that owns windows.
 */
bool isCefBrowserProcess(DWORD pid)
{
    if (pid == 0) {
        return false;
    }
    const std::wstring image = platform::processImageName(pid);
    if (image.empty() || !platform::iequals(image, kCefImageName)) {
        return false;
    }
    const std::wstring commandLine = platform::processCommandLine(pid);
    if (commandLine.empty()) {
        HH_LOG_DEBUG(kLog, L"command line of pid {} unreadable; assuming browser process", pid);
        return true;
    }
    return !platform::icontains(commandLine, kSubprocessSwitch);
}

/**
 * @brief Collected during EnumChildWindows: every descendant owned by the
 *        browser pid.
 */
struct ChildSearch {
    DWORD browserPid = 0;
    std::vector<HWND> owned;
};

/**
 * @brief EnumChildWindows callback (the API already recurses for us).
 */
BOOL CALLBACK enumChildProc(HWND hwnd, LPARAM lparam)
{
    auto* search = reinterpret_cast<ChildSearch*>(lparam);
    if (search == nullptr || hwnd == nullptr) {
        return TRUE;
    }
    if (windowPid(hwnd) == search->browserPid) {
        search->owned.push_back(hwnd);
    }
    return TRUE;
}

/**
 * @brief Walks GetParent() upwards while the parent still belongs to the
 *        browser pid and returns the outermost browser-owned window.
 */
HWND outermostOwnedBy(HWND hwnd, DWORD browserPid)
{
    HWND current = hwnd;
    for (int depth = 0; depth < kMaxParentWalk && current != nullptr; ++depth) {
        const HWND parent = ::GetParent(current);
        // Stop at the first parent that AME (or nobody) owns.
        if (parent == nullptr || windowPid(parent) != browserPid) {
            break;
        }
        current = parent;
    }
    return current;
}

/**
 * @brief Nearest ancestor (excluding the window itself) whose class is
 *        "DroverLord - Window Class", or nullptr.
 */
HWND nearestDroverLord(HWND hwnd)
{
    HWND current = hwnd;
    for (int depth = 0; depth < kMaxParentWalk && current != nullptr; ++depth) {
        const HWND parent = ::GetAncestor(current, GA_PARENT);
        if (parent == nullptr || parent == current) {
            break;
        }
        // GetAncestor(GA_PARENT) yields the desktop at the top; that is not
        // a panel host, so the loop simply ends there.
        if (className(parent) == droverLordClassName()) {
            return parent;
        }
        current = parent;
    }
    return nullptr;
}

/**
 * @brief Client size of a window in physical pixels ({0,0} on failure).
 */
SIZE clientSize(HWND hwnd)
{
    RECT client{};
    if (hwnd == nullptr || !::GetClientRect(hwnd, &client)) {
        return SIZE{0, 0};
    }
    return SIZE{client.right - client.left, client.bottom - client.top};
}

/**
 * @brief Area of a window rect (0 for an empty or unreadable rect).
 */
long long windowArea(HWND hwnd)
{
    RECT rect{};
    if (hwnd == nullptr || !::GetWindowRect(hwnd, &rect)) {
        return 0;
    }
    const LONG width = std::max(0L, rect.right - rect.left);
    const LONG height = std::max(0L, rect.bottom - rect.top);
    return static_cast<long long>(width) * static_cast<long long>(height);
}

/**
 * @brief Picks one HWND out of several outermost candidates.
 *
 * Order of preference: a client size matching the JS-reported panel size
 * (within 4 px), then visible windows, then the largest.
 */
HWND chooseCandidate(const std::vector<HWND>& candidates, std::optional<SIZE> expectedSizePx)
{
    if (candidates.empty()) {
        return nullptr;
    }
    if (candidates.size() == 1) {
        return candidates.front();
    }

    // Size matching against the panel's own measurement is the strongest
    // signal we have when several CEP panels of the same host are open.
    if (expectedSizePx.has_value() && expectedSizePx->cx > 0 && expectedSizePx->cy > 0) {
        std::vector<HWND> matching;
        for (HWND hwnd : candidates) {
            const SIZE size = clientSize(hwnd);
            if (std::abs(size.cx - expectedSizePx->cx) <= kSizeTolerancePx &&
                std::abs(size.cy - expectedSizePx->cy) <= kSizeTolerancePx) {
                matching.push_back(hwnd);
            }
        }
        if (matching.size() == 1) {
            return matching.front();
        }
        if (matching.size() > 1) {
            HH_LOG_WARN(kLog, L"{} panel windows match the expected size {}x{}; picking the visible/largest",
                        matching.size(), expectedSizePx->cx, expectedSizePx->cy);
            return chooseCandidate(matching, std::nullopt);
        }
        HH_LOG_DEBUG(kLog, L"no panel window matches the expected size {}x{}", expectedSizePx->cx, expectedSizePx->cy);
    }

    // Prefer visible windows, then the biggest area.
    HWND best = nullptr;
    long long bestScore = -1;
    for (HWND hwnd : candidates) {
        const long long visibleBonus = ::IsWindowVisible(hwnd) ? (1LL << 40) : 0;
        const long long score = visibleBonus + windowArea(hwnd);
        if (score > bestScore) {
            bestScore = score;
            best = hwnd;
        }
    }
    return best;
}

/**
 * @brief The AME process id: from the main frame if visible, otherwise from
 *        the browser process's parent (AME spawns the CEF browser itself).
 */
DWORD amePidFor(DWORD browserPid)
{
    if (const auto main = findAmeMainWindow(); main.has_value() && main->pid != 0) {
        return main->pid;
    }
    const DWORD parent = platform::parentProcessId(browserPid);
    if (parent != 0 && platform::iequals(platform::processImageName(parent), L"Adobe Media Encoder.exe")) {
        return parent;
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * @brief Resolves the CEF browser pid for the renderer pid the panel reports.
 *
 * Walks up from the renderer: the direct parent is normally the browser; if
 * the parent is itself a "--type=" subprocess (zygote-style spawning) one
 * more hop is taken. As a last resort the reported pid itself is accepted
 * when it already is a browser process.
 */
DWORD browserPidForRenderer(DWORD rendererPid)
{
    if (rendererPid == 0) {
        HH_LOG_WARN(kLog, L"browserPidForRenderer called with pid 0");
        return 0;
    }

    // First hop: the renderer's parent.
    const DWORD parent = platform::parentProcessId(rendererPid);
    if (parent != 0) {
        if (isCefBrowserProcess(parent)) {
            HH_LOG_DEBUG(kLog, L"renderer {} -> browser {}", rendererPid, parent);
            return parent;
        }

        // Second hop: the parent was a subprocess; try its parent once more.
        const bool parentIsCef = platform::iequals(platform::processImageName(parent), kCefImageName);
        if (parentIsCef) {
            const DWORD grandParent = platform::parentProcessId(parent);
            if (grandParent != 0 && isCefBrowserProcess(grandParent)) {
                HH_LOG_DEBUG(kLog, L"renderer {} -> subprocess {} -> browser {}", rendererPid, parent, grandParent);
                return grandParent;
            }
        }
    }

    // The panel might have reported the browser pid directly.
    if (isCefBrowserProcess(rendererPid)) {
        HH_LOG_DEBUG(kLog, L"pid {} is already the CEF browser process", rendererPid);
        return rendererPid;
    }

    HH_LOG_WARN(kLog, L"no CEPHtmlEngine browser process found for renderer pid {} (parent {})", rendererPid, parent);
    return 0;
}

/**
 * @brief Locates the panel's outermost CEF window inside AME's frames.
 */
std::optional<PanelWindows> locatePanelWindows(DWORD rendererPid, std::optional<SIZE> expectedSizePx)
{
    // Resolve the process that actually owns windows.
    const DWORD browserPid = browserPidForRenderer(rendererPid);
    if (browserPid == 0) {
        return std::nullopt;
    }

    // Every AME frame (main + floating) can host the panel.
    const DWORD amePid = amePidFor(browserPid);
    if (amePid == 0) {
        HH_LOG_WARN(kLog, L"cannot determine the AME process for browser pid {}", browserPid);
        return std::nullopt;
    }
    const std::vector<HWND> frames = ameTopLevelWindows(amePid);
    if (frames.empty()) {
        HH_LOG_DEBUG(kLog, L"AME pid {} has no top-level windows yet", amePid);
        return std::nullopt;
    }

    // Collect every browser-owned descendant of every AME frame.
    ChildSearch search;
    search.browserPid = browserPid;
    for (HWND frame : frames) {
        if (frame == nullptr || !::IsWindow(frame)) {
            continue;
        }
        ::EnumChildWindows(frame, &enumChildProc, reinterpret_cast<LPARAM>(&search));
    }
    if (search.owned.empty()) {
        HH_LOG_DEBUG(kLog, L"no windows of browser pid {} found under {} AME frame(s)", browserPid, frames.size());
        return std::nullopt;
    }

    // Reduce to the distinct outermost windows.
    std::vector<HWND> outermost;
    for (HWND hwnd : search.owned) {
        const HWND top = outermostOwnedBy(hwnd, browserPid);
        if (top != nullptr && std::find(outermost.begin(), outermost.end(), top) == outermost.end()) {
            outermost.push_back(top);
        }
    }

    // Choose one and fill in the surrounding AME windows.
    const HWND cef = chooseCandidate(outermost, expectedSizePx);
    if (cef == nullptr) {
        return std::nullopt;
    }

    PanelWindows panel;
    panel.rendererPid = rendererPid;
    panel.browserPid = browserPid;
    panel.cefWindow = cef;
    panel.panelHost = nearestDroverLord(cef);
    panel.ownerRoot = ::GetAncestor(cef, GA_ROOT);
    if (!::GetWindowRect(cef, &panel.rectPx)) {
        HH_LOG_WARN(kLog, L"GetWindowRect failed for panel window {:#x} (error {})",
                    reinterpret_cast<uintptr_t>(cef), ::GetLastError());
        return std::nullopt;
    }
    panel.visible = ::IsWindowVisible(cef) != FALSE;

    // A window without a root is not inside AME at all (detached mid-move).
    if (panel.ownerRoot == nullptr) {
        HH_LOG_WARN(kLog, L"panel window {:#x} has no root window", reinterpret_cast<uintptr_t>(cef));
        return std::nullopt;
    }

    HH_LOG_INFO(kLog, L"panel located: cef {:#x} host {:#x} root {:#x} rect {},{} {}x{} visible {} ({} candidate(s))",
                reinterpret_cast<uintptr_t>(panel.cefWindow), reinterpret_cast<uintptr_t>(panel.panelHost),
                reinterpret_cast<uintptr_t>(panel.ownerRoot), panel.rectPx.left, panel.rectPx.top,
                panel.rectPx.right - panel.rectPx.left, panel.rectPx.bottom - panel.rectPx.top, panel.visible,
                outermost.size());
    return panel;
}

/**
 * @brief Refreshes the volatile parts of a located panel.
 */
bool refreshPanelWindows(PanelWindows& panel)
{
    // The CEF window is the anchor; without it nothing else is meaningful.
    if (panel.cefWindow == nullptr || !::IsWindow(panel.cefWindow)) {
        return false;
    }

    // Rect and visibility change constantly while AME lays out.
    RECT rect{};
    if (!::GetWindowRect(panel.cefWindow, &rect)) {
        return false;
    }
    panel.rectPx = rect;
    panel.visible = ::IsWindowVisible(panel.cefWindow) != FALSE;

    // The root changes when the panel is torn off into a floating frame.
    const HWND root = ::GetAncestor(panel.cefWindow, GA_ROOT);
    if (root != nullptr) {
        panel.ownerRoot = root;
    } else if (panel.ownerRoot != nullptr && !::IsWindow(panel.ownerRoot)) {
        panel.ownerRoot = nullptr;
    }

    // Re-resolve the panel host only when the cached one died or moved.
    if (panel.panelHost == nullptr || !::IsWindow(panel.panelHost)) {
        panel.panelHost = nearestDroverLord(panel.cefWindow);
    }
    return panel.ownerRoot != nullptr;
}

} // namespace hh::ame
