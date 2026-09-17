// ---------------------------------------------------------------------------
// DockController.cpp - keeps the HdrHint window glued to the CEP panel in AME.
//
// The window is never a WS_CHILD of AME; it is a top-level popup *owned* by
// AME's frame (GWLP_HWNDPARENT), positioned over the panel's CEF HWND. Three
// scoped WinEvent hooks tell us when AME or the CEF browser move, resize,
// hide, re-parent or destroy windows; every callback only posts one
// coalesced WM_HH_DOCK_TICK and the UI thread re-applies geometry.
//
// Ownership across processes is a two-edged sword (see the design review):
// when AME destroys its frame, our HWND dies with it, so every "AME is going
// away" signal un-owns the window *before* anything else happens.
// ---------------------------------------------------------------------------
#include "ame/DockController.h"

#include "ame/AmeProcess.h"
#include "core/Logger.h"
#include "platform/Process.h"
#include "platform/Time.h"
#include "platform/Utf.h"
#include "ui/window/Messages.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <iterator>
#include <vector>

namespace hh::ame {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"Dock";

/// Minimum spacing between two geometry passes driven by WinEvents.
constexpr uint64_t kApplyIntervalMs = 8;

/// Re-search cadence while the panel HWND has not been found yet.
constexpr uint64_t kSearchIntervalMs = 2000;

/// How long we keep searching before giving up and floating.
constexpr uint64_t kSearchTimeoutMs = 60000;

/// Inset (px) of the four corner probes used by the occlusion check.
constexpr LONG kOcclusionInsetPx = 8;

/// EVENT_OBJECT_REPARENT is missing from the SDK header with the project's
/// defines; the value is documented and stable.
#ifndef EVENT_OBJECT_REPARENT
#define EVENT_OBJECT_REPARENT 0x800F
#endif

/// Timer id for the WinEvent throttle (TIMERPROC based, not routed via wndProc).
constexpr UINT_PTR kThrottleTimerId = 0x4844'4B54; // 'HDKT'

/// Pending-event bits set by the WinEvent callback, consumed by onDockTick().
enum PendingBits : uint32_t {
    kPendingGeometry     = 1u << 0,   ///< any location/show/hide/reorder change
    kPendingReparent     = 1u << 1,   ///< EVENT_OBJECT_REPARENT: root may have changed
    kPendingPanelGone    = 1u << 2,   ///< our CEF window or its root was destroyed
    kPendingMoveSizeBegin= 1u << 3,   ///< AME entered a move/size loop
    kPendingMoveSizeEnd  = 1u << 4,   ///< AME left a move/size loop
};

// The controller is a per-process singleton (one main window, one dock);
// the static hook callback and the throttle timer resolve it through here.
DockController* g_current = nullptr;
std::atomic<uint32_t> g_pendingEvents{0};
std::atomic<bool> g_tickPosted{false};
uint64_t g_lastApplyMs = 0;          ///< last WinEvent-driven applyGeometry()
uint64_t g_searchStartedMs = 0;      ///< when the current Searching phase began
bool g_inMoveSize = false;           ///< AME is dragging/resizing its frame
bool g_throttleTimerArmed = false;   ///< a deferred tick is scheduled
bool g_geometryDirty = false;        ///< a throttled tick still needs applying
bool g_fallbackDock = false;         ///< docked on JS bounds, no CEF HWND yet

/**
 * @brief Empty-rect check on a RECT.
 */
bool rectEmpty(const RECT& r) noexcept
{
    return r.right <= r.left || r.bottom <= r.top;
}

/**
 * @brief Client area of a window in screen coordinates.
 */
std::optional<RECT> clientRectOnScreen(HWND hwnd)
{
    if (hwnd == nullptr || !::IsWindow(hwnd)) {
        return std::nullopt;
    }
    RECT client{};
    if (!::GetClientRect(hwnd, &client)) {
        return std::nullopt;
    }
    // ClientToScreen maps a point; do both corners so RTL layouts are fine.
    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    if (!::ClientToScreen(hwnd, &topLeft) || !::ClientToScreen(hwnd, &bottomRight)) {
        return std::nullopt;
    }
    return RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

/**
 * @brief Rounds a double to LONG with saturation (no UB on huge values).
 */
LONG toLong(double v) noexcept
{
    if (!std::isfinite(v)) {
        return 0;
    }
    const double clamped = std::clamp(v, -2147483648.0, 2147483647.0);
    return static_cast<LONG>(std::lround(clamped));
}

/**
 * @brief Human-readable state name for the log.
 */
const wchar_t* stateName(DockState s) noexcept
{
    switch (s) {
    case DockState::Undocked:  return L"Undocked";
    case DockState::Searching: return L"Searching";
    case DockState::Docked:    return L"Docked";
    case DockState::Suspended: return L"Suspended";
    case DockState::Picking:   return L"Picking";
    }
    return L"?";
}

/// Poll cadence of the pick-a-panel mouse watcher.
constexpr UINT kPickPollMs = 30;
/// Pick mode gives up after this long without a click.
constexpr uint64_t kPickTimeoutMs = 30000;
/// Re-dock attempts onto the saved panel happen at most this often.
constexpr uint64_t kRedockIntervalMs = 5000;
/// Minimum overlap (intersection over union) for a saved target to count as "the same panel".
constexpr double kRedockMinIoU = 0.5;

/// Window text of AME's Drover frames / tab panels (the class is the same for both).
constexpr const wchar_t* kDroverTabPanelTitle = L"DroverLord - TabPanel Window";
constexpr const wchar_t* kDroverFrameTitle = L"DroverLord - Frame Window";

/**
 * @brief Class name of a window (empty on failure).
 */
std::wstring windowClass(HWND hwnd)
{
    wchar_t buffer[128]{};
    if (hwnd == nullptr || ::GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer))) == 0) {
        return {};
    }
    return buffer;
}

/**
 * @brief Window text of a window (empty on failure).
 */
std::wstring windowTitle(HWND hwnd)
{
    wchar_t buffer[256]{};
    if (hwnd == nullptr || ::GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer))) == 0) {
        return {};
    }
    return buffer;
}

/**
 * @brief True when the process owning @p hwnd is Adobe Media Encoder.
 */
bool belongsToAme(HWND hwnd, DWORD* pidOut)
{
    DWORD pid = 0;
    if (hwnd == nullptr || ::GetWindowThreadProcessId(hwnd, &pid) == 0 || pid == 0) {
        return false;
    }
    if (pidOut != nullptr) {
        *pidOut = pid;
    }
    return platform::icontains(platform::processImageName(pid), L"Adobe Media Encoder");
}

/**
 * @brief Intersection over union of two rects (0 when either is empty).
 */
double rectIoU(const RECT& a, const RECT& b)
{
    RECT inter{};
    if (!::IntersectRect(&inter, &a, &b)) {
        return 0.0;
    }
    const double ai = static_cast<double>(inter.right - inter.left) * static_cast<double>(inter.bottom - inter.top);
    const double aa = static_cast<double>(a.right - a.left) * static_cast<double>(a.bottom - a.top);
    const double ab = static_cast<double>(b.right - b.left) * static_cast<double>(b.bottom - b.top);
    const double uni = aa + ab - ai;
    return uni > 0.0 ? ai / uni : 0.0;
}

/**
 * @brief Parses "l,t,w,h" into a RECT. False on junk.
 */
bool parseSignature(const std::wstring& text, RECT& out)
{
    const std::vector<std::wstring> parts = platform::split(text, L',');
    if (parts.size() != 4) {
        return false;
    }
    long long v[4]{};
    for (size_t i = 0; i < 4; ++i) {
        const auto n = platform::parseInt(platform::trim(parts[i]));
        if (!n.has_value()) {
            return false;
        }
        v[i] = *n;
    }
    if (v[2] <= 0 || v[3] <= 0) {
        return false;
    }
    out = RECT{static_cast<LONG>(v[0]), static_cast<LONG>(v[1]), static_cast<LONG>(v[0] + v[2]), static_cast<LONG>(v[1] + v[3])};
    return true;
}

/// Collected during the saved-target search.
struct RedockCandidate {
    HWND hwnd = nullptr;
    RECT rect{};
};

/**
 * @brief EnumChildWindows callback: counts visible tab-panel windows.
 *
 * Every real AME panel (docked or torn off) sits inside a
 * "DroverLord - TabPanel Window"; the empty extension frame only holds a
 * frame window and one bare view container.
 */
BOOL CALLBACK countTabPanels(HWND hwnd, LPARAM lParam)
{
    auto* count = reinterpret_cast<int*>(lParam);
    if (count == nullptr) {
        return FALSE;
    }
    if (::IsWindowVisible(hwnd) && windowTitle(hwnd) == kDroverTabPanelTitle) {
        ++(*count);
    }
    return TRUE;
}

/// Result slot for largestViewContainer().
struct ContainerSearch {
    HWND best = nullptr;
    LONGLONG area = 0;
};

/**
 * @brief EnumChildWindows callback: remembers the largest visible
 *        "OS_ViewContainer" - the content area below a frame's tab strip.
 */
BOOL CALLBACK largestViewContainer(HWND hwnd, LPARAM lParam)
{
    auto* search = reinterpret_cast<ContainerSearch*>(lParam);
    if (search == nullptr) {
        return FALSE;
    }
    if (!::IsWindowVisible(hwnd) || windowTitle(hwnd) != L"OS_ViewContainer") {
        return TRUE;
    }
    RECT r{};
    if (!::GetWindowRect(hwnd, &r) || rectEmpty(r)) {
        return TRUE;
    }
    const LONGLONG area = static_cast<LONGLONG>(r.right - r.left) * static_cast<LONGLONG>(r.bottom - r.top);
    if (area > search->area) {
        search->area = area;
        search->best = hwnd;
    }
    return TRUE;
}

/// Saved-target marker for "the extension frame, wherever it is".
constexpr const wchar_t* kExtensionSignature = L"extension";

/**
 * @brief True for a visible, captioned, owned top-level Drover window with
 *        no view containers - the shape of the empty extension frame.
 */
bool isExtensionFrame(HWND hwnd)
{
    if (hwnd == nullptr || !::IsWindow(hwnd) || !::IsWindowVisible(hwnd)) {
        return false;
    }
    if (::GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;   // a child panel, not a frame
    }
    if (windowClass(hwnd) != droverLordClassName()) {
        return false;
    }
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != WS_CAPTION) {
        return false;   // AME's own floating panels draw their own chrome
    }
    if (::GetWindow(hwnd, GW_OWNER) == nullptr) {
        return false;
    }
    int tabPanels = 0;
    ::EnumChildWindows(hwnd, &countTabPanels, reinterpret_cast<LPARAM>(&tabPanels));
    return tabPanels == 0;   // a torn-off real panel has at least one
}

/**
 * @brief The dock target inside the empty "HDR Hint" frame AME creates for
 *        Window > Extensions, or nullptr when that frame is not open.
 *
 * The target is the frame's content view container rather than the frame
 * itself, so the "HDR Hint" tab strip stays visible: the user can still
 * drag the tab into the workspace or close it, exactly like any AME panel.
 * Falls back to the frame when the container cannot be found.
 */
HWND findExtensionFrame(const AmeWindow& ame)
{
    for (HWND top : ameTopLevelWindows(ame.pid)) {
        if (top == ame.hwnd || !isExtensionFrame(top)) {
            continue;
        }
        RECT r{};
        if (!::GetWindowRect(top, &r) || rectEmpty(r)) {
            continue;
        }
        ContainerSearch content;
        ::EnumChildWindows(top, &largestViewContainer, reinterpret_cast<LPARAM>(&content));
        return content.best != nullptr ? content.best : top;
    }
    return nullptr;
}

/**
 * @brief EnumChildWindows callback: keeps visible Drover tab panels and frames.
 */
BOOL CALLBACK collectDroverWindows(HWND hwnd, LPARAM lParam)
{
    auto* out = reinterpret_cast<std::vector<RedockCandidate>*>(lParam);
    if (out == nullptr) {
        return FALSE;
    }
    if (windowClass(hwnd) != droverLordClassName()) {
        return TRUE;
    }
    const std::wstring title = windowTitle(hwnd);
    if (title != kDroverTabPanelTitle && title != kDroverFrameTitle) {
        return TRUE;
    }
    if (!::IsWindowVisible(hwnd)) {
        return TRUE;
    }
    RECT r{};
    if (::GetWindowRect(hwnd, &r) && !rectEmpty(r)) {
        out->push_back(RedockCandidate{hwnd, r});
    }
    return TRUE;
}

/**
 * @brief TIMERPROC for the throttle timer: re-runs the tick once the 8 ms
 *        window has passed. DispatchMessage calls this directly, so it needs
 *        no cooperation from the window procedure.
 */
void CALLBACK throttleTimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD)
{
    // One-shot: always kill first so a slow tick cannot re-enter.
    if (hwnd != nullptr) {
        ::KillTimer(hwnd, id);
    }
    g_throttleTimerArmed = false;
    if (g_current != nullptr && g_geometryDirty) {
        g_pendingEvents.fetch_or(kPendingGeometry, std::memory_order_relaxed);
        g_current->onDockTick();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/**
 * @brief Binds the controller to the main window and registers it as the
 *        instance the static hook callback resolves.
 */
DockController::DockController(ui::WindowHost& window)
    : window_(window)
{
    if (g_current != nullptr) {
        HH_LOG_WARN(kLog, L"a second DockController replaces the current one");
    }
    g_current = this;
    g_pendingEvents.store(0, std::memory_order_relaxed);
    g_tickPosted.store(false, std::memory_order_relaxed);
    g_inMoveSize = false;
    g_geometryDirty = false;
    g_fallbackDock = false;
}

/**
 * @brief Unhooks, disarms the throttle timer and un-owns the window so a
 *        late AME shutdown can never cascade into a destroyed HWND.
 */
DockController::~DockController()
{
    stopPickTimer();
    removeHooks();
    if (g_throttleTimerArmed && window_.hwnd() != nullptr) {
        ::KillTimer(window_.hwnd(), kThrottleTimerId);
    }
    g_throttleTimerArmed = false;
    // Leaving the window owned by AME after we are gone would be unsafe.
    if (state_ == DockState::Docked && window_.hwnd() != nullptr && ::IsWindow(window_.hwnd())) {
        window_.setMode(ui::WindowMode::Floating, nullptr, RECT{});
    }
    if (g_current == this) {
        g_current = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Master switch
// ---------------------------------------------------------------------------

/**
 * @brief Enables or disables docking. Disabling while docked floats the
 *        window immediately; enabling re-docks when a panel is known.
 */
void DockController::setEnabled(bool enabled)
{
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    HH_LOG_INFO(kLog, L"docking {}", enabled ? L"enabled" : L"disabled");

    if (!enabled_) {
        // Un-own first, then tidy up the state machine.
        removeHooks();
        if (state_ == DockState::Picking) {
            cancelPick(L"Docking disabled");
        }
        if (state_ == DockState::Docked) {
            window_.setMode(ui::WindowMode::Floating, nullptr, RECT{});
            g_fallbackDock = false;
            setState(DockState::Undocked);
            if (onMessage) {
                onMessage(L"Docking disabled - floating");
            }
        } else if (state_ == DockState::Searching) {
            setState(DockState::Undocked);
        }
        return;
    }

    // Re-enabled: dock again if the panel already said hello, or onto the
    // panel the user picked last time.
    if (rendererPid_ != 0 && !userUndocked_) {
        tryDock();
    } else if (!userUndocked_) {
        dockToDefaultTarget();
    }
}

// ---------------------------------------------------------------------------
// Panel bridge inputs
// ---------------------------------------------------------------------------

/**
 * @brief The panel connected: remember who it is and start docking.
 */
void DockController::onPanelHello(uint32_t connectionId, DWORD rendererPid, std::optional<COLORREF> panelBackground)
{
    HH_LOG_INFO(kLog, L"panel hello: connection {} renderer pid {}", connectionId, rendererPid);
    connectionId_ = connectionId;
    rendererPid_ = rendererPid;
    panelBackground_ = panelBackground;
    elevationMismatch_ = false;

    // A hello from a new panel invalidates any earlier location; the bounds
    // hint stays because the same panel usually reports them right after.
    if (state_ == DockState::Docked) {
        removeHooks();
        window_.setMode(ui::WindowMode::Floating, nullptr, RECT{});
        g_fallbackDock = false;
        setState(DockState::Undocked);
    }
    panel_.reset();

    if (rendererPid_ == 0) {
        HH_LOG_WARN(kLog, L"panel reported pid 0; cannot locate its window");
        setState(DockState::Undocked);
        return;
    }
    if (!enabled_ || userUndocked_) {
        HH_LOG_DEBUG(kLog, L"not docking (enabled {} userUndocked {})", enabled_, userUndocked_);
        return;
    }

    // Start (or restart) the search phase and try right away.
    g_searchStartedMs = platform::nowMonotonicMs();
    lastSearchMs_ = 0;
    setState(DockState::Searching);
    tryDock();
}

/**
 * @brief The panel measured itself (CSS px + devicePixelRatio).
 */
void DockController::onPanelBounds(uint32_t connectionId, double x, double y, double w, double h, double dpr, bool visible)
{
    // Ignore stale reports from a previous connection.
    if (connectionId_ != 0 && connectionId != 0 && connectionId != connectionId_) {
        HH_LOG_DEBUG(kLog, L"ignoring panelBounds from stale connection {}", connectionId);
        return;
    }

    // Reject nonsense so a bad report can never produce a giant window.
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) || !std::isfinite(dpr)) {
        HH_LOG_WARN(kLog, L"panelBounds with non-finite values ignored");
        return;
    }
    jsBounds_.x = x;
    jsBounds_.y = y;
    jsBounds_.w = std::max(0.0, w);
    jsBounds_.h = std::max(0.0, h);
    jsBounds_.dpr = (dpr > 0.1 && dpr < 10.0) ? dpr : 1.0;
    jsBounds_.visible = visible;
    jsBounds_.valid = jsBounds_.w > 0.0 && jsBounds_.h > 0.0;

    // The visibility hint feeds the docked geometry; the fallback rect too.
    if (state_ == DockState::Docked) {
        applyGeometry(g_fallbackDock);
    }
}

/**
 * @brief Visibility-only update (WindowVisibilityChanged / document.hidden).
 */
void DockController::onPanelVisibility(bool visible)
{
    if (jsBounds_.visible == visible) {
        return;
    }
    jsBounds_.visible = visible;
    HH_LOG_DEBUG(kLog, L"panel visibility hint: {}", visible);
    if (state_ == DockState::Docked) {
        applyGeometry(false);
    }
}

/**
 * @brief The host theme changed; the app re-skins from panelBackground().
 */
void DockController::onPanelThemeChanged(COLORREF panelBackground)
{
    panelBackground_ = panelBackground;
}

/**
 * @brief The panel went away (pipe closed, extension unloaded, tab closed).
 *
 * Un-owning happens first: once the panel is gone AME may tear down the
 * frame that owns us at any moment.
 */
void DockController::onPanelGone(uint32_t connectionId)
{
    // A stale "gone" for an older connection must not float the new panel.
    if (connectionId_ != 0 && connectionId != 0 && connectionId != connectionId_) {
        HH_LOG_DEBUG(kLog, L"ignoring panelGone from stale connection {}", connectionId);
        return;
    }
    HH_LOG_INFO(kLog, L"panel gone (connection {})", connectionId);

    removeHooks();
    const bool wasDocked = state_ == DockState::Docked;
    const bool wasPicked = pickedDock_;
    if (wasDocked) {
        window_.setMode(ui::WindowMode::Floating, nullptr, RECT{});
    }
    g_fallbackDock = false;
    pickedDock_ = false;
    panel_.reset();
    rendererPid_ = 0;
    connectionId_ = 0;
    jsBounds_.valid = false;
    elevationMismatch_ = false;
    setState(DockState::Undocked);
    if (wasDocked && onMessage) {
        // A picked panel usually comes back (workspace switch, AME restart):
        // tick() re-docks onto the saved target, so keep the message low-key.
        onMessage(wasPicked ? L"Media Encoder panel went away - floating until it returns"
                            : L"Media Encoder panel closed - floating");
    }
}

/**
 * @brief AME is quitting: un-own immediately, keep running.
 */
void DockController::onAmeBeforeQuit()
{
    HH_LOG_INFO(kLog, L"Media Encoder is quitting");
    stopPickTimer();
    removeHooks();
    const bool wasDocked = state_ == DockState::Docked;
    if (wasDocked) {
        window_.setMode(ui::WindowMode::Floating, nullptr, RECT{});
    }
    g_fallbackDock = false;
    pickedDock_ = false;
    panel_.reset();
    rendererPid_ = 0;
    connectionId_ = 0;
    jsBounds_.valid = false;
    setState(DockState::Undocked);
    if (wasDocked && onMessage) {
        onMessage(L"Media Encoder is quitting - floating");
    }
}

/**
 * @brief AME switched workspaces: the panel host (and maybe the frame) was
 *        rebuilt, so re-locate the panel windows from scratch.
 */
void DockController::onWorkspaceChanged()
{
    HH_LOG_INFO(kLog, L"workspace changed");
    if (rendererPid_ == 0 || !enabled_ || userUndocked_) {
        return;
    }

    if (state_ != DockState::Docked) {
        // Not docked: simply try again now.
        g_searchStartedMs = platform::nowMonotonicMs();
        lastSearchMs_ = 0;
        setState(DockState::Searching);
        tryDock();
        return;
    }

    // Docked: the old HWNDs may be gone; float, then search again. tryDock()
    // re-installs the hooks for whichever process ids it finds.
    removeHooks();
    window_.setMode(ui::WindowMode::Floating, nullptr, RECT{});
    g_fallbackDock = false;
    panel_.reset();
    g_searchStartedMs = platform::nowMonotonicMs();
    lastSearchMs_ = 0;
    setState(DockState::Searching);
    tryDock();
}

/**
 * @brief The user pressed Dock.
 */
void DockController::userDock()
{
    userUndocked_ = false;
    if (!enabled_) {
        if (onMessage) {
            onMessage(L"Enable docking in Settings first");
        }
        return;
    }
    if (state_ == DockState::Picking) {
        return;   // already waiting for the click
    }
    if (rendererPid_ == 0) {
        // No CEP panel around: dock onto the panel picked last time, or ask
        // for a new pick. Works on every AME version, no extension needed.
        if (state_ == DockState::Docked && pickedDock_) {
            applyGeometry(true);
            return;
        }
        if (dockToDefaultTarget()) {
            return;
        }
        beginPick();
        return;
    }
    if (state_ == DockState::Docked) {
        applyGeometry(true);
        return;
    }
    g_searchStartedMs = platform::nowMonotonicMs();
    lastSearchMs_ = 0;
    setState(DockState::Searching);
    tryDock();
}

/**
 * @brief The user pressed Undock: float and stay floating until userDock().
 */
void DockController::userUndock()
{
    userUndocked_ = true;
    if (state_ == DockState::Picking) {
        cancelPick(L"Pick cancelled");
    }
    removeHooks();
    if (state_ == DockState::Docked) {
        pickedDock_ = false;
        window_.setMode(ui::WindowMode::Floating, nullptr, RECT{});
        g_fallbackDock = false;
        setState(DockState::Undocked);
        if (onMessage) {
            onMessage(L"Undocked - floating");
        }
    } else if (state_ == DockState::Searching) {
        setState(DockState::Undocked);
    }
}

// ---------------------------------------------------------------------------
// Window inputs
// ---------------------------------------------------------------------------

/**
 * @brief Coalesced WinEvent tick delivered through WM_HH_DOCK_TICK.
 */
void DockController::onDockTick()
{
    // Allow the hook to post again; then grab everything that piled up.
    g_tickPosted.store(false, std::memory_order_relaxed);
    const uint32_t pending = g_pendingEvents.exchange(0, std::memory_order_relaxed);
    if (state_ != DockState::Docked) {
        return;
    }

    // Structural events first: they decide whether geometry still matters.
    if ((pending & kPendingPanelGone) != 0) {
        HH_LOG_INFO(kLog, L"panel or owner window destroyed");
        onPanelGone(connectionId_);
        return;
    }
    if ((pending & kPendingMoveSizeBegin) != 0) {
        g_inMoveSize = true;
    }
    if ((pending & kPendingMoveSizeEnd) != 0) {
        g_inMoveSize = false;
    }
    if ((pending & kPendingReparent) != 0 && panel_.has_value() && panel_->cefWindow != nullptr) {
        // The panel may have moved into another AME frame; re-read the root.
        if (!refreshPanelWindows(*panel_)) {
            onPanelGone(connectionId_);
            return;
        }
    }

    // Geometry: at most one pass per 8 ms; a deferred timer catches the last
    // event of a burst so the window never lags behind AME's final layout.
    const bool wantsGeometry = (pending & (kPendingGeometry | kPendingReparent | kPendingMoveSizeBegin |
                                           kPendingMoveSizeEnd)) != 0 || g_geometryDirty;
    if (!wantsGeometry) {
        return;
    }
    const uint64_t now = platform::nowMonotonicMs();
    if (now - g_lastApplyMs < kApplyIntervalMs) {
        g_geometryDirty = true;
        if (!g_throttleTimerArmed && window_.hwnd() != nullptr) {
            const UINT delay = static_cast<UINT>(kApplyIntervalMs - (now - g_lastApplyMs));
            if (::SetTimer(window_.hwnd(), kThrottleTimerId, std::max(1u, delay), &throttleTimerProc) != 0) {
                g_throttleTimerArmed = true;
            }
        }
        return;
    }
    g_lastApplyMs = now;
    g_geometryDirty = false;
    applyGeometry((pending & (kPendingReparent | kPendingMoveSizeEnd)) != 0);
}

/**
 * @brief 1 Hz housekeeping: re-search while Searching, re-check while Docked.
 */
void DockController::tick()
{
    const uint64_t now = platform::nowMonotonicMs();

    if (state_ == DockState::Picking) {
        return;   // the pick timer owns this phase
    }

    // Floating: glue back on as soon as AME (and a panel) are around again -
    // the remembered panel first, otherwise the empty "HDR Hint" extension
    // frame the moment it opens. Never fights an explicit Undock.
    if (state_ == DockState::Undocked && enabled_ && !userUndocked_ && rendererPid_ == 0) {
        if (now - lastRedockMs_ >= kRedockIntervalMs) {
            lastRedockMs_ = now;
            dockToDefaultTarget();
        }
        return;
    }

    if (state_ == DockState::Searching) {
        if (!enabled_ || userUndocked_ || rendererPid_ == 0) {
            setState(DockState::Undocked);
            return;
        }
        if (now - lastSearchMs_ < kSearchIntervalMs) {
            return;
        }
        // Give up after a minute: last resort is the JS-derived rectangle,
        // otherwise we float and say so.
        if (now - g_searchStartedMs >= kSearchTimeoutMs) {
            HH_LOG_WARN(kLog, L"panel window not found within {} s", kSearchTimeoutMs / 1000);
            lastSearchMs_ = now;
            const std::optional<RECT> fallback = fallbackRect();
            const std::optional<AmeWindow> ameMain = findAmeMainWindow();
            if (fallback.has_value() && ameMain.has_value() && ameMain->hwnd != nullptr) {
                const DWORD browserPid = browserPidForRenderer(rendererPid_);
                if (browserPid != 0 && platform::elevationDiffers(browserPid)) {
                    elevationMismatch_ = true;
                    setState(DockState::Undocked);
                    if (onMessage) {
                        onMessage(L"Run HDR Hint and Media Encoder at the same privilege level to dock");
                    }
                    return;
                }
                PanelWindows synthetic;
                synthetic.rendererPid = rendererPid_;
                synthetic.browserPid = browserPid;
                synthetic.cefWindow = nullptr;
                synthetic.panelHost = nullptr;
                synthetic.ownerRoot = ameMain->hwnd;
                synthetic.rectPx = *fallback;
                synthetic.visible = jsBounds_.visible;
                panel_ = synthetic;
                g_fallbackDock = true;
                installHooks();
                window_.setMode(ui::WindowMode::Docked, panel_->ownerRoot, *fallback);
                lastRect_ = *fallback;
                lastVisible_ = false;
                setState(DockState::Docked);
                if (onMessage) {
                    onMessage(L"Docked inside Media Encoder (approximate position)");
                }
                applyGeometry(true);
                return;
            }
            setState(DockState::Undocked);
            if (onMessage) {
                onMessage(L"Could not find the panel inside Media Encoder - floating");
            }
            return;
        }
        tryDock();
        return;
    }

    if (state_ == DockState::Docked) {
        if (!panel_.has_value()) {
            HH_LOG_WARN(kLog, L"docked without panel windows; floating");
            onPanelGone(connectionId_);
            return;
        }
        // The anchor HWND must still exist; the hook can miss its destroy
        // event when the browser process dies abruptly.
        if (panel_->cefWindow != nullptr && !::IsWindow(panel_->cefWindow)) {
            HH_LOG_INFO(kLog, L"panel window {:#x} no longer exists", reinterpret_cast<uintptr_t>(panel_->cefWindow));
            onPanelGone(connectionId_);
            return;
        }
        if (panel_->ownerRoot != nullptr && !::IsWindow(panel_->ownerRoot)) {
            HH_LOG_INFO(kLog, L"owner root {:#x} no longer exists", reinterpret_cast<uintptr_t>(panel_->ownerRoot));
            onPanelGone(connectionId_);
            return;
        }
        // While docked on the JS fallback keep trying to find the real HWND.
        if (g_fallbackDock && now - lastSearchMs_ >= kSearchIntervalMs) {
            lastSearchMs_ = now;
            const SIZE expected{toLong(jsBounds_.w * jsBounds_.dpr), toLong(jsBounds_.h * jsBounds_.dpr)};
            std::optional<PanelWindows> located = locatePanelWindows(rendererPid_, jsBounds_.valid ? std::optional<SIZE>(expected) : std::nullopt);
            if (located.has_value() && located->ownerRoot != nullptr) {
                HH_LOG_INFO(kLog, L"upgraded fallback dock to the real panel window");
                const HWND previousRoot = panel_->ownerRoot;
                panel_ = std::move(*located);
                g_fallbackDock = false;
                installHooks();
                if (panel_->ownerRoot != previousRoot) {
                    window_.setMode(ui::WindowMode::Docked, panel_->ownerRoot, panel_->rectPx);
                }
                applyGeometry(true);
                return;
            }
        }
        applyGeometry(false);
    }
}

/**
 * @brief The HWND was destroyed by AME and recreated floating: hooks are
 *        stale (their post target changed) and ownership is gone.
 */
void DockController::onWindowRecreated()
{
    HH_LOG_INFO(kLog, L"main window recreated; re-evaluating dock");
    removeHooks();
    g_throttleTimerArmed = false;   // the old HWND took its timers with it
    g_geometryDirty = false;
    g_inMoveSize = false;
    g_fallbackDock = false;
    lastRect_ = RECT{};
    lastVisible_ = false;
    panel_.reset();
    setState(DockState::Undocked);

    // Dock again when the panel is still around and the user wants it.
    const bool wasPicked = pickedDock_;
    pickedDock_ = false;
    if (rendererPid_ != 0 && enabled_ && !userUndocked_ && platform::isProcessAlive(rendererPid_)) {
        g_searchStartedMs = platform::nowMonotonicMs();
        lastSearchMs_ = 0;
        setState(DockState::Searching);
        tryDock();
    } else if (wasPicked && enabled_ && !userUndocked_ && !savedTarget_.empty()) {
        redockSaved();
    }
}

// ---------------------------------------------------------------------------
// Pick-a-panel docking
// ---------------------------------------------------------------------------

/**
 * @brief TIMERPROC of the pick watcher (thread timer, no window needed).
 */
void CALLBACK DockController::pickTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    if (g_current != nullptr) {
        g_current->pollPick();
    }
}

/**
 * @brief Stops the pick watcher timer (idempotent).
 */
void DockController::stopPickTimer()
{
    if (pickTimer_ != 0) {
        ::KillTimer(nullptr, pickTimer_);
        pickTimer_ = 0;
    }
}

/**
 * @brief Enters pick mode: floats first, then watches for the next click.
 */
void DockController::beginPick()
{
    if (!enabled_) {
        if (onMessage) {
            onMessage(L"Enable docking in Settings first");
        }
        return;
    }
    if (state_ == DockState::Picking) {
        return;
    }
    if (!isAmeRunning()) {
        if (onMessage) {
            onMessage(L"Start Adobe Media Encoder first, then press Dock again");
        }
        return;
    }

    // A docked window would sit under the very click we are waiting for.
    removeHooks();
    if (state_ == DockState::Docked) {
        window_.setMode(ui::WindowMode::Floating, nullptr, RECT{});
        g_fallbackDock = false;
        pickedDock_ = false;
        panel_.reset();
    }

    pickStartedMs_ = platform::nowMonotonicMs();
    // Ignore a button that is still held from the click that got us here.
    pickButtonWasDown_ = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    // Clear the "pressed since last call" bits so a stale press does not count.
    (void)::GetAsyncKeyState(VK_LBUTTON);
    (void)::GetAsyncKeyState(VK_ESCAPE);

    stopPickTimer();
    pickTimer_ = ::SetTimer(nullptr, 0, kPickPollMs, &DockController::pickTimerProc);
    if (pickTimer_ == 0) {
        HH_LOG_ERROR(kLog, L"pick timer could not be created (error {})", ::GetLastError());
        if (onMessage) {
            onMessage(L"Could not start the panel picker");
        }
        return;
    }
    setState(DockState::Picking);
    HH_LOG_INFO(kLog, L"pick mode: waiting for a click inside Media Encoder");
    if (onMessage) {
        onMessage(L"Click the Media Encoder panel HDR Hint should cover. Esc cancels.");
    }
}

/**
 * @brief Leaves pick mode without docking.
 */
void DockController::cancelPick(const std::wstring& reason)
{
    stopPickTimer();
    if (state_ != DockState::Picking) {
        return;
    }
    HH_LOG_INFO(kLog, L"pick cancelled: {}", reason);
    setState(DockState::Undocked);
    if (onMessage && !reason.empty()) {
        onMessage(reason);
    }
}

/**
 * @brief Timer body: Esc cancels, a fresh left click picks, silence times out.
 */
void DockController::pollPick()
{
    if (state_ != DockState::Picking) {
        stopPickTimer();
        return;
    }
    if ((::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        cancelPick(L"Pick cancelled");
        return;
    }
    const uint64_t now = platform::nowMonotonicMs();
    if (now - pickStartedMs_ >= kPickTimeoutMs) {
        cancelPick(L"No panel picked - still floating");
        return;
    }

    // Edge-detect the left button so a held button never re-triggers.
    const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool pressed = down && !pickButtonWasDown_;
    pickButtonWasDown_ = down;
    if (!pressed) {
        return;
    }

    POINT pt{};
    if (!::GetCursorPos(&pt)) {
        return;
    }
    const HWND hit = ::WindowFromPoint(pt);
    HWND root = hit != nullptr ? ::GetAncestor(hit, GA_ROOT) : nullptr;
    if (root == nullptr) {
        root = hit;
    }
    // Clicks on HDR Hint itself (the Dock button, the tray menu) are not picks.
    if (root != nullptr && (root == window_.hwnd() || root == window_.popup().hwnd())) {
        return;
    }
    const HWND target = resolvePickTarget(pt);
    if (target == nullptr) {
        cancelPick(L"That click was outside Media Encoder - still floating");
        return;
    }
    stopPickTimer();
    setState(DockState::Undocked);
    if (!dockToWindow(target)) {
        if (onMessage) {
            onMessage(L"Could not dock onto that panel");
        }
    }
}

/**
 * @brief Maps a screen point to the AME tab panel (or frame) under it.
 *
 * The tab panel is the best anchor: it is hidden when another tab of the same
 * group comes to the front, so the docked window hides with it.
 */
HWND DockController::resolvePickTarget(POINT pt) const
{
    const HWND hit = ::WindowFromPoint(pt);
    if (hit == nullptr) {
        return nullptr;
    }
    HWND root = ::GetAncestor(hit, GA_ROOT);
    if (root == nullptr) {
        root = hit;
    }
    if (!belongsToAme(root, nullptr)) {
        return nullptr;
    }

    // Walk up: prefer a tab panel, then a frame, then any Drover window.
    HWND frame = nullptr;
    HWND anyDrover = nullptr;
    HWND current = hit;
    for (int depth = 0; depth < 32 && current != nullptr; ++depth) {
        if (windowClass(current) == droverLordClassName()) {
            const std::wstring title = windowTitle(current);
            if (title == kDroverTabPanelTitle) {
                return current;
            }
            if (title == kDroverFrameTitle && frame == nullptr) {
                frame = current;
            }
            if (anyDrover == nullptr) {
                anyDrover = current;
            }
        }
        const HWND parent = ::GetAncestor(current, GA_PARENT);
        if (parent == nullptr || parent == current || parent == root) {
            break;
        }
        current = parent;
    }
    if (frame != nullptr) {
        return frame;
    }
    if (anyDrover != nullptr) {
        return anyDrover;
    }
    // The click hit AME's frame itself (menu bar, empty area): not a panel.
    return nullptr;
}

/**
 * @brief "l,t,w,h" of @p target relative to @p root's client origin.
 */
std::wstring DockController::signatureFor(HWND target, HWND root) const
{
    RECT rect{};
    if (target == nullptr || !::GetWindowRect(target, &rect)) {
        return {};
    }
    const std::optional<RECT> client = clientRectOnScreen(root);
    const LONG originX = client.has_value() ? client->left : 0;
    const LONG originY = client.has_value() ? client->top : 0;
    return std::format(L"{},{},{},{}", rect.left - originX, rect.top - originY, rect.right - rect.left, rect.bottom - rect.top);
}

/**
 * @brief Owns the window to AME and glues it over @p target.
 */
bool DockController::dockToWindow(HWND target)
{
    if (target == nullptr || !::IsWindow(target)) {
        return false;
    }
    DWORD amePid = 0;
    HWND root = ::GetAncestor(target, GA_ROOT);
    if (root == nullptr) {
        root = target;
    }
    if (!belongsToAme(root, &amePid)) {
        HH_LOG_WARN(kLog, L"dockToWindow: {:#x} is not a Media Encoder window", reinterpret_cast<uintptr_t>(target));
        return false;
    }
    if (window_.hwnd() == nullptr || !::IsWindow(window_.hwnd())) {
        return false;
    }
    // UIPI blocks ownership and hook delivery across integrity levels.
    if (platform::elevationDiffers(amePid)) {
        const bool firstTime = !elevationMismatch_;
        elevationMismatch_ = true;
        if (firstTime && onMessage) {
            onMessage(L"Run HDR Hint and Media Encoder at the same privilege level to dock");
        }
        return false;
    }

    RECT rect{};
    if (!::GetWindowRect(target, &rect) || rectEmpty(rect)) {
        return false;
    }

    // Leave any earlier dock cleanly before taking the new anchor.
    removeHooks();
    if (state_ == DockState::Docked) {
        window_.setMode(ui::WindowMode::Floating, nullptr, RECT{});
    }

    PanelWindows anchor;
    anchor.rendererPid = 0;
    anchor.browserPid = 0;
    anchor.cefWindow = target;                // the tracked window (refreshPanelWindows works on any HWND)
    anchor.panelHost = target;
    anchor.ownerRoot = root;
    anchor.rectPx = rect;
    anchor.visible = ::IsWindowVisible(target) != FALSE;
    panel_ = anchor;
    pickedDock_ = true;
    g_fallbackDock = false;
    jsBounds_.valid = false;
    jsBounds_.visible = true;
    // The extension frame floats wherever the user last left it, so a
    // position signature would later match some unrelated panel; remember
    // it by kind instead.
    savedTarget_ = isExtensionFrame(root) ? std::wstring(kExtensionSignature) : signatureFor(target, root);
    installHooks();

    if (const auto clip = clientRectOnScreen(root); clip.has_value()) {
        RECT clipped{};
        if (::IntersectRect(&clipped, &rect, &*clip)) {
            rect = clipped;
        }
    }
    window_.setMode(ui::WindowMode::Docked, root, rect);
    lastRect_ = rect;
    lastVisible_ = false;
    setState(DockState::Docked);
    HH_LOG_INFO(kLog, L"docked onto {:#x} ('{}') rect {},{} {}x{} signature {}", reinterpret_cast<uintptr_t>(target),
                windowTitle(target), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, savedTarget_);
    if (onMessage) {
        onMessage(L"Docked onto the Media Encoder panel");
    }
    applyGeometry(true);
    return true;
}

/**
 * @brief Scripting entry: dock onto whatever AME panel is under @p pt.
 */
bool DockController::dockAtScreenPoint(POINT pt)
{
    if (!enabled_) {
        return false;
    }
    if (state_ == DockState::Picking) {
        cancelPick(std::wstring());
    }
    userUndocked_ = false;
    const HWND target = resolvePickTarget(pt);
    if (target == nullptr) {
        HH_LOG_WARN(kLog, L"no Media Encoder panel at {},{}", pt.x, pt.y);
        if (onMessage) {
            onMessage(L"No Media Encoder panel at that point");
        }
        return false;
    }
    return dockToWindow(target);
}

/**
 * @brief Finds the saved panel again by its position inside AME and docks.
 */
bool DockController::redockSaved()
{
    RECT saved{};
    if (!parseSignature(savedTarget_, saved)) {
        return false;
    }
    const std::optional<AmeWindow> ame = findAmeMainWindow();
    if (!ame.has_value() || ame->hwnd == nullptr) {
        return false;
    }
    const std::optional<RECT> client = clientRectOnScreen(ame->hwnd);
    if (!client.has_value()) {
        return false;
    }
    // Saved rect is relative to the client origin; bring it to screen space.
    const RECT wanted{saved.left + client->left, saved.top + client->top, saved.right + client->left, saved.bottom + client->top};

    std::vector<RedockCandidate> candidates;
    ::EnumChildWindows(ame->hwnd, &collectDroverWindows, reinterpret_cast<LPARAM>(&candidates));
    // Panels torn off into floating frames live in AME's other top-level windows.
    for (HWND top : ameTopLevelWindows(ame->pid)) {
        if (top != ame->hwnd) {
            ::EnumChildWindows(top, &collectDroverWindows, reinterpret_cast<LPARAM>(&candidates));
        }
    }

    HWND best = nullptr;
    double bestScore = 0.0;
    for (const RedockCandidate& c : candidates) {
        const double score = rectIoU(c.rect, wanted);
        // Prefer tab panels over frames when the overlap ties.
        const double bonus = windowTitle(c.hwnd) == kDroverTabPanelTitle ? 0.01 : 0.0;
        if (score + bonus > bestScore) {
            bestScore = score + bonus;
            best = c.hwnd;
        }
    }
    if (best == nullptr || bestScore < kRedockMinIoU) {
        HH_LOG_DEBUG(kLog, L"saved panel not found ({} candidate(s), best overlap {:.2f})", candidates.size(), bestScore);
        return false;
    }
    HH_LOG_INFO(kLog, L"saved panel found again ({:#x}, overlap {:.2f})", reinterpret_cast<uintptr_t>(best), bestScore);
    return dockToWindow(best);
}

/**
 * @brief Docks onto the best target that needs no user input.
 *
 * The Window > Extensions > HDR Hint frame wins whenever it is open: opening
 * it is the clearest "put HDR Hint here" the user can express. Otherwise the
 * panel picked last time, if it can still be found.
 */
bool DockController::dockToDefaultTarget()
{
    if (!enabled_ || userUndocked_) {
        return false;
    }
    const std::optional<AmeWindow> ame = findAmeMainWindow();
    if (!ame.has_value() || ame->hwnd == nullptr) {
        return false;
    }
    if (const HWND frame = findExtensionFrame(*ame); frame != nullptr) {
        HH_LOG_INFO(kLog, L"extension frame {:#x} is open; docking onto it", reinterpret_cast<uintptr_t>(frame));
        if (dockToWindow(frame)) {
            return true;
        }
    }
    if (!savedTarget_.empty() && savedTarget_ != kExtensionSignature) {
        return redockSaved();
    }
    return false;
}

// ---------------------------------------------------------------------------
// WinEvent hooks
// ---------------------------------------------------------------------------

/**
 * @brief Hook callback (UI thread, out-of-context). Records what happened
 *        and posts one coalesced WM_HH_DOCK_TICK.
 */
void CALLBACK DockController::winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                                           DWORD thread, DWORD time)
{
    // Cursor, caret and child-object events fire constantly; only windows count.
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }
    DockController* self = g_current;
    if (self == nullptr || hwnd == nullptr) {
        return;
    }
    (void)thread;
    (void)time;

    // Classify the event into pending bits for onDockTick().
    uint32_t bits = 0;
    switch (event) {
    case EVENT_OBJECT_DESTROY:
        if (self->panel_.has_value() &&
            (hwnd == self->panel_->cefWindow || hwnd == self->panel_->ownerRoot)) {
            bits |= kPendingPanelGone;
        } else {
            bits |= kPendingGeometry;
        }
        break;
    case EVENT_OBJECT_REPARENT:
        bits |= kPendingReparent;
        break;
    case EVENT_SYSTEM_MOVESIZESTART:
        bits |= kPendingMoveSizeBegin;
        break;
    case EVENT_SYSTEM_MOVESIZEEND:
        bits |= kPendingMoveSizeEnd;
        break;
    default:
        bits |= kPendingGeometry;
        break;
    }
    g_pendingEvents.fetch_or(bits, std::memory_order_relaxed);

    // One message in flight at a time keeps the queue from flooding.
    const HWND target = self->window_.hwnd();
    if (target == nullptr) {
        return;
    }
    bool expected = false;
    if (g_tickPosted.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        if (!::PostMessageW(target, ui::WM_HH_DOCK_TICK, static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(hwnd))) {
            g_tickPosted.store(false, std::memory_order_relaxed);
            HH_LOG_DEBUG(kLog, L"PostMessage(WM_HH_DOCK_TICK) failed (error {})", ::GetLastError());
        }
    }
}

/**
 * @brief Installs the three scoped hooks for the current panel.
 */
void DockController::installHooks()
{
    removeHooks();
    if (!panel_.has_value()) {
        return;
    }

    // AME's pid comes from the frame that owns us; the browser pid from the
    // locator. Both are filtered by the hook itself, so nothing else's
    // events reach our callback.
    DWORD amePid = 0;
    if (panel_->ownerRoot != nullptr) {
        ::GetWindowThreadProcessId(panel_->ownerRoot, &amePid);
    }
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;

    if (amePid != 0) {
        hooks_[0] = ::SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr,
                                      &DockController::winEventProc, amePid, 0, flags);
        hooks_[1] = ::SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                      &DockController::winEventProc, amePid, 0, flags);
    } else {
        HH_LOG_WARN(kLog, L"owner root has no process id; AME hooks not installed");
    }
    if (panel_->browserPid != 0) {
        hooks_[2] = ::SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                      &DockController::winEventProc, panel_->browserPid, 0, flags);
    }

    // Only hooks that were actually requested count as failures (picked
    // docks have no browser process, so hook #2 is simply not needed).
    const bool wanted[3] = {amePid != 0, amePid != 0, panel_->browserPid != 0};
    for (size_t i = 0; i < 3; ++i) {
        if (wanted[i] && hooks_[i] == nullptr) {
            HH_LOG_WARN(kLog, L"SetWinEventHook #{} failed (error {})", i, ::GetLastError());
        }
    }
    HH_LOG_DEBUG(kLog, L"hooks installed for AME pid {} / browser pid {}", amePid, panel_->browserPid);
}

/**
 * @brief Unhooks everything (safe to call repeatedly).
 */
void DockController::removeHooks()
{
    for (HWINEVENTHOOK& hook : hooks_) {
        if (hook != nullptr) {
            if (!::UnhookWinEvent(hook)) {
                HH_LOG_DEBUG(kLog, L"UnhookWinEvent failed (error {})", ::GetLastError());
            }
            hook = nullptr;
        }
    }
    g_pendingEvents.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Docking
// ---------------------------------------------------------------------------

/**
 * @brief One attempt to locate the panel and own the window.
 */
void DockController::tryDock()
{
    if (!enabled_ || userUndocked_) {
        return;
    }
    if (rendererPid_ == 0) {
        setState(DockState::Undocked);
        return;
    }
    if (window_.hwnd() == nullptr || !::IsWindow(window_.hwnd())) {
        HH_LOG_DEBUG(kLog, L"no main window yet; dock attempt postponed");
        return;
    }
    lastSearchMs_ = platform::nowMonotonicMs();

    // The panel's own measurement disambiguates several CEP panels.
    std::optional<SIZE> expected;
    if (jsBounds_.valid) {
        expected = SIZE{toLong(jsBounds_.w * jsBounds_.dpr), toLong(jsBounds_.h * jsBounds_.dpr)};
    }
    std::optional<PanelWindows> located = locatePanelWindows(rendererPid_, expected);
    if (!located.has_value() || located->ownerRoot == nullptr) {
        if (state_ != DockState::Searching) {
            g_searchStartedMs = lastSearchMs_;
            setState(DockState::Searching);
        }
        return;
    }

    // UIPI blocks ownership and hook delivery across integrity levels.
    if (platform::elevationDiffers(located->browserPid)) {
        HH_LOG_WARN(kLog, L"elevation differs from CEPHtmlEngine pid {}; cannot dock", located->browserPid);
        panel_ = std::move(*located);
        const bool firstTime = !elevationMismatch_;
        elevationMismatch_ = true;
        setState(DockState::Undocked);
        if (firstTime && onMessage) {
            onMessage(L"Run HDR Hint and Media Encoder at the same privilege level to dock");
        }
        return;
    }

    // Own the window, size it over the panel and start tracking.
    panel_ = std::move(*located);
    g_fallbackDock = false;
    installHooks();

    RECT rect = panel_->rectPx;
    if (const auto clip = clientRectOnScreen(panel_->ownerRoot); clip.has_value()) {
        RECT clipped{};
        if (::IntersectRect(&clipped, &rect, &*clip)) {
            rect = clipped;
        }
    }
    window_.setMode(ui::WindowMode::Docked, panel_->ownerRoot, rect);
    lastRect_ = rect;
    lastVisible_ = false;
    setState(DockState::Docked);
    if (onMessage) {
        onMessage(L"Docked inside Media Encoder");
    }
    applyGeometry(true);
}

/**
 * @brief Re-reads the panel geometry and pushes it to the window.
 */
void DockController::applyGeometry(bool force)
{
    if (state_ != DockState::Docked || !panel_.has_value()) {
        return;
    }

    // Refresh the anchor: real HWND, or the JS-derived rect in fallback mode.
    const HWND previousRoot = panel_->ownerRoot;
    if (panel_->cefWindow != nullptr) {
        if (!refreshPanelWindows(*panel_)) {
            onPanelGone(connectionId_);
            return;
        }
    } else {
        const std::optional<RECT> fallback = fallbackRect();
        if (fallback.has_value()) {
            panel_->rectPx = *fallback;
        }
        panel_->visible = jsBounds_.visible;
        if (panel_->ownerRoot == nullptr || !::IsWindow(panel_->ownerRoot)) {
            onPanelGone(connectionId_);
            return;
        }
    }

    // The owner changed (panel torn off into a floating frame): re-own.
    if (panel_->ownerRoot != previousRoot && panel_->ownerRoot != nullptr) {
        HH_LOG_INFO(kLog, L"owner root changed {:#x} -> {:#x}", reinterpret_cast<uintptr_t>(previousRoot),
                    reinterpret_cast<uintptr_t>(panel_->ownerRoot));
        installHooks();
        window_.setMode(ui::WindowMode::Docked, panel_->ownerRoot, panel_->rectPx);
        force = true;
    }

    // Clip to the owner's client area so a scrolled panel never spills out.
    RECT rect = panel_->rectPx;
    bool empty = rectEmpty(rect);
    if (!empty) {
        if (const auto clip = clientRectOnScreen(panel_->ownerRoot); clip.has_value()) {
            RECT clipped{};
            if (::IntersectRect(&clipped, &rect, &*clip)) {
                rect = clipped;
            } else {
                empty = true;
            }
        }
    }

    // Visible only when every input agrees. AME's own move/size loop is NOT a
    // reason to hide: LOCATIONCHANGE keeps arriving during the drag, so the
    // window simply follows (a frame behind at worst).
    bool visible = !empty && panel_->visible && jsBounds_.visible;
    if (visible && panel_->ownerRoot != nullptr && ::IsIconic(panel_->ownerRoot)) {
        visible = false;
    }
    if (visible && occluded(rect)) {
        visible = false;
    }

    if (!visible) {
        if (lastVisible_ || force) {
            window_.setDockVisible(false);
        }
        lastVisible_ = false;
        return;
    }

    // Order matters: bounds before show so the first frame lands in place.
    if (!::EqualRect(&rect, &lastRect_) || force) {
        window_.setDockBounds(rect);
        lastRect_ = rect;
    }
    if (!lastVisible_ || force) {
        window_.setDockVisible(true);
    }
    lastVisible_ = true;
}

/**
 * @brief Publishes a state change.
 */
void DockController::setState(DockState s)
{
    if (state_ == s) {
        return;
    }
    HH_LOG_INFO(kLog, L"state {} -> {}", stateName(state_), stateName(s));
    state_ = s;
    if (onStateChanged) {
        onStateChanged(s);
    }
}

/**
 * @brief True when another Media Encoder window (a dialog, a different
 *        frame) sits over the dock rect. Foreign windows do not count.
 */
bool DockController::occluded(const RECT& rect) const
{
    if (rectEmpty(rect) || !panel_.has_value()) {
        return false;
    }

    // Probe the centre and four inset corners.
    const LONG insetX = std::min(kOcclusionInsetPx, std::max(0L, (rect.right - rect.left) / 2 - 1));
    const LONG insetY = std::min(kOcclusionInsetPx, std::max(0L, (rect.bottom - rect.top) / 2 - 1));
    const POINT probes[5] = {
        {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2},
        {rect.left + insetX, rect.top + insetY},
        {rect.right - 1 - insetX, rect.top + insetY},
        {rect.left + insetX, rect.bottom - 1 - insetY},
        {rect.right - 1 - insetX, rect.bottom - 1 - insetY},
    };

    const HWND ourHwnd = window_.hwnd();
    const HWND popupHwnd = window_.popup().hwnd();
    const DWORD ourPid = ::GetCurrentProcessId();
    DWORD amePid = 0;
    if (panel_->ownerRoot != nullptr) {
        ::GetWindowThreadProcessId(panel_->ownerRoot, &amePid);
    }

    for (const POINT& pt : probes) {
        const HWND hit = ::WindowFromPoint(pt);
        if (hit == nullptr) {
            continue;
        }
        HWND root = ::GetAncestor(hit, GA_ROOT);
        if (root == nullptr) {
            root = hit;
        }
        // Ourselves, our popup, or anything inside the frame that owns us.
        if (root == ourHwnd || root == popupHwnd || root == panel_->ownerRoot) {
            continue;
        }
        DWORD hitPid = 0;
        ::GetWindowThreadProcessId(root, &hitPid);
        if (hitPid == ourPid || (hitPid != 0 && hitPid == panel_->browserPid)) {
            continue;
        }
        // A foreign app above Media Encoder is not an occluder: we live in
        // AME's z-band (owned window), so it already sits over us and drops
        // behind us the moment AME comes back to the front.
        if (amePid != 0 && hitPid != amePid) {
            continue;
        }
        // Another AME window (a dialog or a different frame) over the panel.
        HH_LOG_TRACE(kLog, L"occluded at {},{} by {:#x} pid {}", pt.x, pt.y, reinterpret_cast<uintptr_t>(root), hitPid);
        return true;
    }
    return false;
}

/**
 * @brief Physical rect derived from the panel's CSS bounds.
 *
 * AME is System-DPI-aware, so the JS coordinates live in its virtualised
 * logical space; LogicalToPhysicalPointForPerMonitorDPI with AME's frame as
 * the reference undoes that. The result must overlap AME's frame to count.
 */
std::optional<RECT> DockController::fallbackRect() const
{
    if (!jsBounds_.valid) {
        return std::nullopt;
    }
    const std::optional<AmeWindow> ameMain = findAmeMainWindow();
    if (!ameMain.has_value() || ameMain->hwnd == nullptr) {
        return std::nullopt;
    }

    // CSS px -> logical px of AME's DPI context.
    const double dpr = jsBounds_.dpr;
    POINT topLeft{toLong(jsBounds_.x * dpr), toLong(jsBounds_.y * dpr)};
    POINT bottomRight{toLong((jsBounds_.x + jsBounds_.w) * dpr), toLong((jsBounds_.y + jsBounds_.h) * dpr)};

    // Logical -> physical using AME's window as the reference.
    if (!::LogicalToPhysicalPointForPerMonitorDPI(ameMain->hwnd, &topLeft) ||
        !::LogicalToPhysicalPointForPerMonitorDPI(ameMain->hwnd, &bottomRight)) {
        HH_LOG_DEBUG(kLog, L"LogicalToPhysicalPointForPerMonitorDPI failed (error {})", ::GetLastError());
        return std::nullopt;
    }
    RECT rect{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    if (rectEmpty(rect)) {
        return std::nullopt;
    }

    // Cross-check: the panel must be somewhere inside AME's frame.
    RECT overlap{};
    if (!::IntersectRect(&overlap, &rect, &ameMain->rect)) {
        HH_LOG_DEBUG(kLog, L"fallback rect {},{} {}x{} does not overlap the AME frame", rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top);
        return std::nullopt;
    }
    return rect;
}

} // namespace hh::ame
