// ---------------------------------------------------------------------------
// WindowHost.cpp - the main borderless window (floating or docked in AME).
//
// One HWND, two personalities:
//
//   FLOATING  WS_OVERLAPPED|WS_CAPTION|WS_THICKFRAME... with the whole frame
//             handed to the client area through WM_NCCALCSIZE. DWM still
//             draws the shadow, rounded corners and Mica; snap layouts work
//             because WM_NCHITTEST reports HTCAPTION / HTMAXBUTTON / HT*
//             resize bands from the widget tree.
//   DOCKED    WS_POPUP owned by Adobe Media Encoder's window, positioned by
//             the dock controller over the CEP panel. Never activates on a
//             click (WM_MOUSEACTIVATE -> MA_NOACTIVATE) so AME keeps its
//             shortcuts, and never rounds or extends the frame.
//
// The frame loop lives in runLoop(): it blocks on the swap chain's frame
// latency waitable only while a frame is wanted AND the window is visible,
// wakes for Timeline timers, and renders synchronously from WM_SIZE /
// WM_TIMER while DefWindowProc runs its modal move/size loop.
//
// The header is a frozen contract, so the two bits of per-window state that
// did not fit (the "we are releasing capture" flag and the precision-wheel
// latch) live as window properties on the HWND (SetPropW), which is the
// Win32-native way to attach data to a window without a member.
// ---------------------------------------------------------------------------
#include "ui/window/WindowHost.h"

#include "core/Expected.h"
#include "core/Logger.h"
#include "platform/Registry.h"
#include "platform/Utf.h"
#include "platform/WinVersion.h"
#include "ui/window/Messages.h"

#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace hh::ui {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"WindowHost";

/// Timer ids (WM_TIMER wParam).
constexpr UINT_PTR kSizeMoveTimerId = 0x100;   ///< renders animations inside the modal move/size loop
constexpr UINT_PTR kTickTimerId = 0x101;       ///< 1 Hz housekeeping tick

/// Window properties used for state the header has no member for.
constexpr const wchar_t* kPropReleasingCapture = L"HdrHint.ReleasingCapture";
constexpr const wchar_t* kPropPreciseWheelUntil = L"HdrHint.PreciseWheelUntil";

/// How long a non-notch wheel delta keeps the "precision device" latch.
constexpr ULONGLONG kPreciseWheelLatchMs = 500;

/// Corner squares of the resize hit-test, in dips.
constexpr float kResizeCornerDips = 16.0f;

/// Minimum visible overlap for a restored placement to be accepted.
constexpr LONG kMinVisiblePx = 64;

/// DWM attribute ids and values written as numbers so SDK gating cannot bite.
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaBorderColor = 34;
constexpr DWORD kDwmwaSystemBackdropType = 38;
constexpr DWORD kDwmwaMicaEffectLegacy = 1029;     ///< 22000..22620 only
constexpr DWORD kDwmCornerDoNotRound = 1;
constexpr DWORD kDwmCornerRound = 2;
constexpr DWORD kDwmBackdropNone = 1;
constexpr DWORD kDwmBackdropMainWindow = 2;
constexpr DWORD kDwmColorNone = 0xFFFFFFFE;
constexpr DWORD kDwmColorDefault = 0xFFFFFFFF;

/// Registry location of the "Transparency effects" switch.
constexpr const wchar_t* kPersonalizeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr const wchar_t* kEnableTransparencyValue = L"EnableTransparency";

/**
 * @brief Maps the toolkit cursor kind to a system cursor handle.
 */
HCURSOR cursorFor(CursorKind kind) {
    const wchar_t* id = IDC_ARROW;
    switch (kind) {
    case CursorKind::Hand: id = IDC_HAND; break;
    case CursorKind::IBeam: id = IDC_IBEAM; break;
    case CursorKind::SizeNS: id = IDC_SIZENS; break;
    case CursorKind::SizeWE: id = IDC_SIZEWE; break;
    case CursorKind::SizeNWSE: id = IDC_SIZENWSE; break;
    case CursorKind::SizeNESW: id = IDC_SIZENESW; break;
    case CursorKind::SizeAll: id = IDC_SIZEALL; break;
    case CursorKind::No: id = IDC_NO; break;
    case CursorKind::Arrow:
    default: id = IDC_ARROW; break;
    }
    return ::LoadCursorW(nullptr, id);
}

/**
 * @brief Current keyboard modifier state from the message queue.
 */
Modifiers modifiersNow() {
    Modifiers m;
    m.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    m.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    m.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    return m;
}

/**
 * @brief Effective DPI of a monitor (96 on failure).
 */
float dpiOfMonitor(HMONITOR mon) {
    if (mon == nullptr) {
        return 96.0f;
    }
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(::GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) {
        return 96.0f;
    }
    return static_cast<float>(dpiX);
}

/**
 * @brief Work area of a monitor, or the primary monitor's when null.
 */
RECT workAreaOf(HMONITOR mon) {
    if (mon == nullptr) {
        const POINT origin{0, 0};
        mon = ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (mon != nullptr && ::GetMonitorInfoW(mon, &info)) {
        return info.rcWork;
    }
    RECT r{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
    return r;
}

/**
 * @brief The user's "Transparency effects" switch (defaults to on).
 */
bool systemTransparencyEnabled() {
    const auto value = platform::regReadDword(HKEY_CURRENT_USER, kPersonalizeKey, kEnableTransparencyValue);
    if (!value) {
        return true;
    }
    return value.value() != 0;
}

/**
 * @brief True for the HRESULTs that mean "the GPU device is gone".
 */
bool isDeviceLostHr(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == D2DERR_RECREATE_TARGET;
}

/**
 * @brief Exact RECT comparison (Win32 has EqualRect, but this reads better).
 */
bool rectsEqual(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

/**
 * @brief Reads a placement string ("x,y,w,h,dpi,max"); false on junk.
 */
bool parsePlacement(const std::wstring& text, RECT& rect, float& dpi, bool& maximized) {
    const std::vector<std::wstring> parts = platform::split(text, L',', true);
    if (parts.size() < 4) {
        return false;
    }
    long long values[6] = {0, 0, 0, 0, 96, 0};
    for (size_t i = 0; i < parts.size() && i < 6; ++i) {
        const auto v = platform::parseInt(platform::trim(parts[i]));
        if (!v) {
            return false;
        }
        values[i] = *v;
    }
    // Reject absurd numbers before they reach SetWindowPlacement.
    const long long limit = 1LL << 20;
    for (long long v : values) {
        if (v < -limit || v > limit) {
            return false;
        }
    }
    if (values[2] <= 0 || values[3] <= 0) {
        return false;
    }
    rect.left = static_cast<LONG>(values[0]);
    rect.top = static_cast<LONG>(values[1]);
    rect.right = rect.left + static_cast<LONG>(values[2]);
    rect.bottom = rect.top + static_cast<LONG>(values[3]);
    dpi = values[4] > 0 ? static_cast<float>(values[4]) : 96.0f;
    maximized = values[5] != 0;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / creation
// ---------------------------------------------------------------------------

/**
 * @brief Builds the root view and popup object; the HWNDs come in create().
 *
 * root() and popup() must never dereference null, so both objects exist
 * from the constructor onwards regardless of window state.
 */
WindowHost::WindowHost(GraphicsDevice& device, TextCache& text, ThemeManager& themes)
    : device_(device), text_(text), themes_(themes) {
    scale_ = DipScale::fromDpi(96.0f);
    root_ = std::make_unique<RootView>(*this, themes_);
    popup_ = std::make_unique<PopupWindow>(device_, text_, themes_);
}

/**
 * @brief Destroys the window if the app did not already.
 */
WindowHost::~WindowHost() {
    destroy();
}

/**
 * @brief Registers the class, creates the floating window, binds the surface
 *        and presents the first frame before the window becomes visible.
 */
bool WindowHost::create(const WindowSpec& spec) {
    if (hwnd_ != nullptr) {
        HH_LOG_WARN(kLog, L"create called twice; ignoring");
        return true;
    }
    spec_ = spec;
    alwaysOnTop_ = spec.alwaysOnTop;
    mode_ = WindowMode::Floating;
    owner_ = nullptr;

    // Defensive: the constructor made these, but never dereference blindly.
    if (!root_) {
        root_ = std::make_unique<RootView>(*this, themes_);
    }
    if (!popup_) {
        popup_ = std::make_unique<PopupWindow>(device_, text_, themes_);
    }

    // CS_DBLCLKS only: no HREDRAW/VREDRAW (GDI never paints here), no class
    // cursor (WM_SETCURSOR picks it from the hovered widget), no brush.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &WindowHost::wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hIcon = spec_.icon;
    wc.hIconSm = spec_.icon;
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = spec_.className.c_str();
    if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        HH_LOG_ERROR(kLog, L"RegisterClassEx failed: {}", Error::fromLastError(L"RegisterClassExW").toString());
        return false;
    }

    if (!createHwnd(WindowMode::Floating, nullptr)) {
        return false;
    }

    // Reduced motion follows the system animation switch from the start.
    root_->timeline().setReducedMotion(ThemeManager::readReducedMotion());

    // Popup HWND (hidden) owned by the main window.
    if (!popup_->create(hwnd_)) {
        HH_LOG_WARN(kLog, L"popup window creation failed; menus will not open");
    }

    // Surface + first frame while still hidden so the reveal is clean.
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const UINT w = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    const UINT h = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    if (!surface_.bind(hwnd_, device_, w, h)) {
        HH_LOG_ERROR(kLog, L"surface bind failed");
        // Keep the window: the loop retries binding on the next frame.
    }
    firstFramePresented_ = false;
    renderFrame();

    if (!spec_.startHidden) {
        ::ShowWindow(hwnd_, SW_SHOW);
        ::UpdateWindow(hwnd_);
    } else {
        // Hidden windows freeze their clock so springs do not jump on show.
        root_->timeline().pause();
    }
    HH_LOG_INFO(kLog, L"window created ({}x{} px, dpi {})", w, h, static_cast<int>(scale_.dpi));
    return true;
}

/**
 * @brief Creates the HWND for a mode. Floating uses the spec's initial rect
 *        (centred when x/y < 0); docked uses the stored dock bounds.
 */
bool WindowHost::createHwnd(WindowMode mode, HWND owner) {
    DWORD style = 0;
    DWORD exStyle = 0;
    int x = 0, y = 0, w = 1, h = 1;

    if (mode == WindowMode::Floating) {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW | WS_EX_ACCEPTFILES | (alwaysOnTop_ ? WS_EX_TOPMOST : 0);

        // Size in the primary monitor's DPI; centre unless a position is given.
        const POINT origin{0, 0};
        HMONITOR mon = ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        const DipScale target = DipScale::fromDpi(dpiOfMonitor(mon));
        const RECT work = workAreaOf(mon);
        w = std::max(1, target.toPxInt(std::max(spec_.initialDips.w, spec_.minSizeDips.w)));
        h = std::max(1, target.toPxInt(std::max(spec_.initialDips.h, spec_.minSizeDips.h)));
        if (spec_.initialDips.x < 0.0f || spec_.initialDips.y < 0.0f) {
            x = work.left + std::max<LONG>(0, ((work.right - work.left) - w) / 2);
            y = work.top + std::max<LONG>(0, ((work.bottom - work.top) - h) / 2);
        } else {
            x = target.toPxInt(spec_.initialDips.x);
            y = target.toPxInt(spec_.initialDips.y);
        }
        owner = nullptr;
    } else {
        style = WS_POPUP | WS_CLIPSIBLINGS;
        exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW;
        x = dockBounds_.left;
        y = dockBounds_.top;
        w = std::max<LONG>(1, dockBounds_.right - dockBounds_.left);
        h = std::max<LONG>(1, dockBounds_.bottom - dockBounds_.top);
        if (owner != nullptr && !::IsWindow(owner)) {
            HH_LOG_WARN(kLog, L"createHwnd: docked owner is not a window; creating without owner");
            owner = nullptr;
        }
    }

    HWND hwnd = ::CreateWindowExW(exStyle, spec_.className.c_str(), spec_.title.c_str(), style, x, y, w, h, owner,
                                  nullptr, ::GetModuleHandleW(nullptr), this);
    if (hwnd == nullptr) {
        HH_LOG_ERROR(kLog, L"CreateWindowEx failed: {}", Error::fromLastError(L"CreateWindowExW").toString());
        hwnd_ = nullptr;
        return false;
    }
    hwnd_ = hwnd;
    mode_ = mode;
    owner_ = (mode == WindowMode::Docked) ? owner : nullptr;

    const UINT dpi = ::GetDpiForWindow(hwnd_);
    scale_ = DipScale::fromDpi(dpi > 0 ? static_cast<float>(dpi) : 96.0f);
    currentCursor_ = ::LoadCursorW(nullptr, IDC_ARROW);

    applyDwmAttributes();
    refreshBackdrop();

    // Session changes (RDP attach/detach) switch backdrops off and on.
    if (!::WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION)) {
        HH_LOG_DEBUG(kLog, L"WTSRegisterSessionNotification failed: {}", Error::fromLastError(L"WTS").toString());
    }
    return true;
}

/**
 * @brief Destroys the HWND (our own decision) without posting WM_QUIT.
 */
void WindowHost::destroy() {
    if (hwnd_ == nullptr) {
        if (popup_) {
            popup_->destroy();
        }
        canvas_.resetDeviceResources();
        surface_.unbind();
        return;
    }
    destroyingSelf_ = true;
    savePlacementIfFloating();

    if (sizeMoveTimer_ != 0) {
        ::KillTimer(hwnd_, kSizeMoveTimerId);
        sizeMoveTimer_ = 0;
    }
    if (tickTimer_ != 0) {
        ::KillTimer(hwnd_, kTickTimerId);
        tickTimer_ = 0;
    }
    ::WTSUnRegisterSessionNotification(hwnd_);

    // Owned windows go first, then the surface, then the HWND itself.
    if (popup_) {
        popup_->destroy();
    }
    canvas_.resetDeviceResources();
    surface_.unbind();
    ::RemovePropW(hwnd_, kPropReleasingCapture);
    ::RemovePropW(hwnd_, kPropPreciseWheelUntil);

    HWND h = hwnd_;
    ::DestroyWindow(h);
    hwnd_ = nullptr;   // WM_NCDESTROY also clears it; be explicit.
    destroyingSelf_ = false;
}

/**
 * @brief Destroys and re-creates the HWND in @p mode (owner change fallback
 *        and recovery after the owner destroyed us).
 */
bool WindowHost::recreate(WindowMode mode) {
    HH_LOG_INFO(kLog, L"recreating window ({})", mode == WindowMode::Docked ? L"docked" : L"floating");
    const HWND dockOwner = (mode == WindowMode::Docked) ? owner_ : nullptr;

    // Remember the floating rect if we are leaving a live floating window.
    if (hwnd_ != nullptr && mode_ == WindowMode::Floating) {
        floatingPlacement_ = placementString();
    }

    // Tear down whatever is left (the HWND may already be gone).
    destroy();

    if (!createHwnd(mode, dockOwner)) {
        return false;
    }

    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const UINT w = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    const UINT h = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    if (!surface_.bind(hwnd_, device_, w, h)) {
        HH_LOG_ERROR(kLog, L"recreate: surface bind failed");
    }
    if (popup_ && !popup_->create(hwnd_)) {
        HH_LOG_WARN(kLog, L"recreate: popup creation failed");
    }
    if (root_) {
        root_->invalidateLayout();
        if (root_->timeline().paused()) {
            root_->timeline().resume();
        }
    }

    if (mode == WindowMode::Floating) {
        // Restore the last floating rect (or centre) and show without
        // stealing focus: this path runs while the user is elsewhere.
        applyPlacement(floatingPlacement_);
        ::SetWindowPos(hwnd_, alwaysOnTop_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        // The window just changed size twice (creation, then placement) while
        // it was still hidden, so the swap chain and the widget tree must be
        // brought to the final client size before the first frame is drawn.
        // Skipping this is what leaves a recreated window showing a stale,
        // wrongly-scaled frame until something else forces a repaint.
        RECT placed{};
        if (::GetClientRect(hwnd_, &placed)) {
            const UINT pw = static_cast<UINT>(std::max<LONG>(1, placed.right - placed.left));
            const UINT ph = static_cast<UINT>(std::max<LONG>(1, placed.bottom - placed.top));
            if (!surface_.resize(pw, ph)) {
                HH_LOG_WARN(kLog, L"recreate: surface resize to {}x{} failed", pw, ph);
            }
        }
        if (root_) {
            root_->invalidateLayout();
        }
        firstFramePresented_ = false;
        renderFrame();
        ::ShowWindow(hwnd_, SW_SHOWNA);
    } else {
        firstFramePresented_ = false;
        renderFrame();
        if (dockVisible_) {
            ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        }
    }
    // The tick timer belonged to the old HWND; re-arm it on the new one.
    if (tickTimer_ == 0) {
        tickTimer_ = ::SetTimer(hwnd_, kTickTimerId, 1000, nullptr);
    }
    pacer_.requestFrame();
    return true;
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

/**
 * @brief Shows the window (restoring from minimized), optionally activating.
 */
void WindowHost::show(bool activate) {
    if (hwnd_ == nullptr) {
        return;
    }
    if (root_ && root_->timeline().paused()) {
        root_->timeline().resume();
    }

    if (mode_ == WindowMode::Docked) {
        dockVisible_ = true;
        if (!::IsWindowVisible(hwnd_)) {
            firstFramePresented_ = false;
            renderFrame();
        }
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        pacer_.requestFrame();
        return;
    }

    // A hidden window that was saved maximized comes back maximized.
    bool wantMax = false;
    if (!::IsWindowVisible(hwnd_)) {
        RECT ignored{};
        float ignoredDpi = 96.0f;
        if (!floatingPlacement_.empty() && parsePlacement(floatingPlacement_, ignored, ignoredDpi, wantMax)) {
            wantMax = wantMax || ::IsZoomed(hwnd_);
        } else {
            wantMax = ::IsZoomed(hwnd_) != FALSE;
        }
        firstFramePresented_ = false;
        renderFrame();
    }

    if (::IsIconic(hwnd_)) {
        ::ShowWindow(hwnd_, SW_RESTORE);
    } else if (wantMax) {
        ::ShowWindow(hwnd_, activate ? SW_SHOWMAXIMIZED : SW_SHOWNA);
        if (!::IsZoomed(hwnd_)) {
            ::ShowWindow(hwnd_, SW_MAXIMIZE);
        }
    } else {
        ::ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNA);
    }
    if (activate) {
        ::SetForegroundWindow(hwnd_);
    }
    pacer_.requestFrame();
}

/**
 * @brief Hides the window and freezes its animation clock.
 */
void WindowHost::hide() {
    if (hwnd_ == nullptr) {
        return;
    }
    if (popup_ && popup_->visible()) {
        popup_->dismiss();
    }
    if (mode_ == WindowMode::Docked) {
        dockVisible_ = false;
    } else {
        savePlacementIfFloating();
    }
    ::ShowWindow(hwnd_, SW_HIDE);
    if (root_ && !root_->timeline().paused()) {
        root_->timeline().pause();
    }
}

/**
 * @brief Minimizes (floating only; a docked panel follows its owner).
 */
void WindowHost::minimize() {
    if (hwnd_ == nullptr || mode_ != WindowMode::Floating) {
        return;
    }
    ::ShowWindow(hwnd_, SW_MINIMIZE);
}

/**
 * @brief Maximize <-> restore (floating only).
 */
void WindowHost::toggleMaximize() {
    if (hwnd_ == nullptr || mode_ != WindowMode::Floating) {
        return;
    }
    ::ShowWindow(hwnd_, ::IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
}

/**
 * @brief True when the window is shown (minimized still counts as shown).
 */
bool WindowHost::visible() const {
    return hwnd_ != nullptr && ::IsWindowVisible(hwnd_) != FALSE;
}

/**
 * @brief True while minimized.
 */
bool WindowHost::minimized() const {
    return hwnd_ != nullptr && ::IsIconic(hwnd_) != FALSE;
}

/**
 * @brief Raises and activates the window (tray click, second instance).
 */
void WindowHost::bringToFront() {
    if (hwnd_ == nullptr) {
        return;
    }
    if (!::IsWindowVisible(hwnd_)) {
        show(true);
    }
    if (::IsIconic(hwnd_)) {
        ::ShowWindow(hwnd_, SW_RESTORE);
    }
    if (mode_ == WindowMode::Floating) {
        ::SetWindowPos(hwnd_, alwaysOnTop_ ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    ::SetForegroundWindow(hwnd_);
    pacer_.requestFrame();
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

/**
 * @brief Applies the window / extended styles of the current mode.
 */
void WindowHost::applyStylesForMode() {
    if (hwnd_ == nullptr) {
        return;
    }
    DWORD style = 0;
    DWORD exStyle = 0;
    if (mode_ == WindowMode::Floating) {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW | WS_EX_ACCEPTFILES | (alwaysOnTop_ ? WS_EX_TOPMOST : 0);
    } else {
        style = WS_POPUP | WS_CLIPSIBLINGS;
        exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW;
    }
    // Keep the visibility bit as it is; the callers manage showing.
    const LONG_PTR current = ::GetWindowLongPtrW(hwnd_, GWL_STYLE);
    if (current & WS_VISIBLE) {
        style |= WS_VISIBLE;
    }
    ::SetLastError(0);
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(style));
    ::SetLastError(0);
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, static_cast<LONG_PTR>(exStyle));
}

/**
 * @brief Switches between floating and docked (owned by AME) modes.
 */
void WindowHost::setMode(WindowMode mode, HWND owner, const RECT& dockBoundsPx) {
    if (hwnd_ == nullptr) {
        HH_LOG_WARN(kLog, L"setMode: no window");
        return;
    }

    if (mode == WindowMode::Docked) {
        // Guard: a dead or self owner would either fail or deadlock z-order.
        if (owner == nullptr || owner == hwnd_ || !::IsWindow(owner)) {
            HH_LOG_WARN(kLog, L"setMode(Docked): owner window not found; staying floating");
            return;
        }
        if (mode_ == WindowMode::Docked && owner_ == owner) {
            setDockBounds(dockBoundsPx);
            return;
        }
        HH_LOG_INFO(kLog, L"docking to owner {:#x}", reinterpret_cast<uintptr_t>(owner));

        // Hide first so the style flip, owner change and DWM changes are one
        // clean transition instead of three visible steps.
        if (popup_ && popup_->visible()) {
            popup_->dismiss();
        }
        // A minimized window cannot be moved or resized: SetWindowPos updates
        // the restored placement instead and the panel would sit at the old
        // size until something restored it by hand. Leave the icon first.
        if (::IsIconic(hwnd_)) {
            HH_LOG_DEBUG(kLog, L"docking a minimized window; restoring it first");
            ::ShowWindow(hwnd_, SW_RESTORE);
        }
        ::ShowWindow(hwnd_, SW_HIDE);
        if (mode_ == WindowMode::Floating) {
            floatingPlacement_ = placementString();
        }
        mode_ = WindowMode::Docked;
        owner_ = owner;
        dockBounds_ = dockBoundsPx;
        applyStylesForMode();

        // Owner (not parent): the window stays top-level, so DComp, DPI and
        // Mica keep working; a cross-process owner is allowed.
        ::SetLastError(0);
        const LONG_PTR prev = ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
        if (prev == 0 && ::GetLastError() != 0) {
            HH_LOG_WARN(kLog, L"SetWindowLongPtr(GWLP_HWNDPARENT) failed: {}; recreating",
                        Error::fromLastError(L"GWLP_HWNDPARENT").toString());
            recreate(WindowMode::Docked);
            return;
        }

        applyDwmAttributes();
        refreshBackdrop();
        const int w = std::max<LONG>(1, dockBounds_.right - dockBounds_.left);
        const int h = std::max<LONG>(1, dockBounds_.bottom - dockBounds_.top);
        // HWND_NOTOPMOST: clearing WS_EX_TOPMOST through SetWindowLongPtr is
        // not enough; only SetWindowPos really drops the topmost band, and
        // a docked panel must sit in AME's band, not above every window.
        ::SetWindowPos(hwnd_, HWND_NOTOPMOST, dockBounds_.left, dockBounds_.top, w, h,
                       SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        surface_.resize(static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left)),
                        static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top)));
        if (root_) {
            root_->invalidateLayout();
            if (root_->timeline().paused() && dockVisible_) {
                root_->timeline().resume();
            }
        }
        firstFramePresented_ = false;
        renderFrame();
        if (dockVisible_) {
            ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        }
        // Windows can refuse a resize (minimized, a modal size-move loop); say
        // so loudly rather than leaving a wrongly sized panel on screen.
        if (RECT dbg{}; ::GetWindowRect(hwnd_, &dbg)) {
            const int gotW = dbg.right - dbg.left;
            const int gotH = dbg.bottom - dbg.top;
            if (gotW != w || gotH != h) {
                HH_LOG_WARN(kLog, L"docked window is {}x{} but {}x{} was requested", gotW, gotH, w, h);
            }
        }
        return;
    }

    // ---- Docked -> Floating --------------------------------------------------
    if (mode_ == WindowMode::Floating) {
        return;
    }
    HH_LOG_INFO(kLog, L"undocking to floating");
    if (popup_ && popup_->visible()) {
        popup_->dismiss();
    }
    ::ShowWindow(hwnd_, SW_HIDE);
    mode_ = WindowMode::Floating;
    owner_ = nullptr;
    applyStylesForMode();

    ::SetLastError(0);
    const LONG_PTR prev = ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, 0);
    if (prev == 0 && ::GetLastError() != 0) {
        HH_LOG_WARN(kLog, L"clearing GWLP_HWNDPARENT failed: {}; recreating",
                    Error::fromLastError(L"GWLP_HWNDPARENT").toString());
        recreate(WindowMode::Floating);
        return;
    }

    applyDwmAttributes();
    refreshBackdrop();
    // Placement keeps the window hidden; the frame change + show come last.
    applyPlacement(floatingPlacement_);
    ::SetWindowPos(hwnd_, alwaysOnTop_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    surface_.resize(static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left)),
                    static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top)));
    if (root_) {
        root_->invalidateLayout();
        if (root_->timeline().paused()) {
            root_->timeline().resume();
        }
    }
    firstFramePresented_ = false;
    renderFrame();
    ::SetWindowPos(hwnd_, alwaysOnTop_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    ::SetForegroundWindow(hwnd_);
    pacer_.requestFrame();
}

/**
 * @brief Moves / sizes the docked window; the surface only resizes when the
 *        size really changed (moves are the common case during drags).
 */
void WindowHost::setDockBounds(const RECT& px) {
    if (rectsEqual(px, dockBounds_) && hwnd_ != nullptr) {
        return;
    }
    const bool sizeChanged = (px.right - px.left) != (dockBounds_.right - dockBounds_.left) ||
                             (px.bottom - px.top) != (dockBounds_.bottom - dockBounds_.top);
    dockBounds_ = px;
    if (hwnd_ == nullptr || mode_ != WindowMode::Docked) {
        return;
    }
    const int w = std::max<LONG>(1, px.right - px.left);
    const int h = std::max<LONG>(1, px.bottom - px.top);

    // Minimized: restore before moving, for the reason above.
    if (::IsIconic(hwnd_)) {
        HH_LOG_DEBUG(kLog, L"setDockBounds on a minimized window; restoring it first");
        ::ShowWindow(hwnd_, SW_RESTORE);
    }

    // Trust the window, not the bookkeeping: if the last resize was refused
    // (minimized, a pending size-move loop) the recorded bounds would match
    // while the window is still the wrong size, and SWP_NOSIZE would keep it
    // that way forever.
    bool mustSize = sizeChanged;
    if (!mustSize) {
        if (RECT actual{}; ::GetWindowRect(hwnd_, &actual)) {
            mustSize = (actual.right - actual.left) != w || (actual.bottom - actual.top) != h;
        }
    }
    UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;
    if (!mustSize) {
        flags |= SWP_NOSIZE;
    }
    ::SetWindowPos(hwnd_, nullptr, px.left, px.top, w, h, flags);
    if (mustSize) {
        // WM_SIZE already resized and rendered; this is the belt for the case
        // where the message was swallowed (hidden window).
        RECT rc{};
        if (::GetClientRect(hwnd_, &rc)) {
            surface_.resize(static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left)),
                            static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top)));
        }
        if (root_) {
            root_->invalidateLayout();
        }
        pacer_.requestFrame();
    }
}

/**
 * @brief Shows / hides the docked panel (panel tab in the background etc.).
 */
void WindowHost::setDockVisible(bool visible) {
    dockVisible_ = visible;
    if (hwnd_ == nullptr || mode_ != WindowMode::Docked) {
        return;
    }
    if (visible) {
        if (root_ && root_->timeline().paused()) {
            root_->timeline().resume();
        }
        if (!::IsWindowVisible(hwnd_)) {
            firstFramePresented_ = false;
            renderFrame();
        }
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        pacer_.requestFrame();
    } else {
        if (popup_ && popup_->visible()) {
            popup_->dismiss();
        }
        ::ShowWindow(hwnd_, SW_HIDE);
        if (root_ && !root_->timeline().paused()) {
            root_->timeline().pause();
        }
    }
}

/**
 * @brief Toggles the topmost flag (floating only; docked follows its owner).
 */
void WindowHost::setAlwaysOnTop(bool on) {
    alwaysOnTop_ = on;
    if (hwnd_ == nullptr || mode_ != WindowMode::Floating) {
        return;
    }
    ::SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/**
 * @brief Updates the minimum tracking size (read on the next WM_GETMINMAXINFO).
 */
void WindowHost::setMinSize(Size dips) {
    spec_.minSizeDips = {std::max(0.0f, dips.w), std::max(0.0f, dips.h)};
}

// ---------------------------------------------------------------------------
// DWM
// ---------------------------------------------------------------------------

/**
 * @brief Dark mode, corner rounding, NC rendering, frame extension and the
 *        border colour for the current mode.
 */
void WindowHost::applyDwmAttributes() {
    if (hwnd_ == nullptr) {
        return;
    }
    const bool floating = (mode_ == WindowMode::Floating);

    // Dark title/frame colours follow the theme (also picks Mica's tint).
    const BOOL dark = themes_.current().isDark ? TRUE : FALSE;
    HRESULT hr = ::DwmSetWindowAttribute(hwnd_, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));
    if (FAILED(hr)) {
        HH_LOG_DEBUG(kLog, L"DWMWA_USE_IMMERSIVE_DARK_MODE failed ({:#010x})", static_cast<uint32_t>(hr));
    }

    // Rounded corners only when floating; docked must sit flush in the panel.
    const DWORD corner = floating ? kDwmCornerRound : kDwmCornerDoNotRound;
    hr = ::DwmSetWindowAttribute(hwnd_, kDwmwaWindowCornerPreference, &corner, sizeof(corner));
    if (FAILED(hr)) {
        HH_LOG_DEBUG(kLog, L"DWMWA_WINDOW_CORNER_PREFERENCE failed ({:#010x})", static_cast<uint32_t>(hr));
    }

    // Keep DWM's non-client rendering (shadow) even though the frame is ours.
    const DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
    hr = ::DwmSetWindowAttribute(hwnd_, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    if (FAILED(hr)) {
        HH_LOG_DEBUG(kLog, L"DWMWA_NCRENDERING_POLICY failed ({:#010x})", static_cast<uint32_t>(hr));
    }

    // A 1 px bottom extension keeps the Win10 shadow alive; docked gets none
    // so no DWM-coloured line shows under the panel.
    MARGINS margins{0, 0, 0, floating ? 1 : 0};
    hr = ::DwmExtendFrameIntoClientArea(hwnd_, &margins);
    if (FAILED(hr)) {
        HH_LOG_DEBUG(kLog, L"DwmExtendFrameIntoClientArea failed ({:#010x})", static_cast<uint32_t>(hr));
    }

    // Win11 hairline border: keep in floating, remove in docked.
    const DWORD border = floating ? kDwmColorDefault : kDwmColorNone;
    hr = ::DwmSetWindowAttribute(hwnd_, kDwmwaBorderColor, &border, sizeof(border));
    if (FAILED(hr)) {
        HH_LOG_DEBUG(kLog, L"DWMWA_BORDER_COLOR failed ({:#010x})", static_cast<uint32_t>(hr));
    }
}

/**
 * @brief Decides whether Mica is actually rendered and tells the theme.
 *
 * Mica needs build 22621+ (22000..22620 has the undocumented 1029 switch),
 * a floating window, transparency effects on, and no remote session.
 */
void WindowHost::refreshBackdrop() {
    if (hwnd_ == nullptr) {
        return;
    }
    // The dark-mode attribute is cheap and must follow theme changes, which
    // reach this window through the same settings notifications.
    const BOOL dark = themes_.current().isDark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd_, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));

    const DWORD build = platform::windowsBuildNumber();
    const bool floating = (mode_ == WindowMode::Floating);
    BOOL composition = TRUE;
    if (FAILED(::DwmIsCompositionEnabled(&composition))) {
        composition = TRUE;
    }
    const bool remote = platform::isRemoteSession() || ::GetSystemMetrics(SM_REMOTESESSION) != 0;
    const bool allowed = floating && composition != FALSE && systemTransparencyEnabled() && !remote;

    Backdrop result = Backdrop::None;
    if (build >= 22621) {
        const DWORD type = allowed ? kDwmBackdropMainWindow : kDwmBackdropNone;
        const HRESULT hr = ::DwmSetWindowAttribute(hwnd_, kDwmwaSystemBackdropType, &type, sizeof(type));
        if (SUCCEEDED(hr) && allowed) {
            result = Backdrop::Mica;
        } else if (FAILED(hr)) {
            HH_LOG_DEBUG(kLog, L"DWMWA_SYSTEMBACKDROP_TYPE failed ({:#010x})", static_cast<uint32_t>(hr));
        }
    } else if (build >= 22000) {
        const BOOL on = allowed ? TRUE : FALSE;
        const HRESULT hr = ::DwmSetWindowAttribute(hwnd_, kDwmwaMicaEffectLegacy, &on, sizeof(on));
        if (SUCCEEDED(hr) && allowed) {
            result = Backdrop::MicaLegacy;
        } else if (FAILED(hr)) {
            HH_LOG_DEBUG(kLog, L"legacy Mica attribute failed ({:#010x})", static_cast<uint32_t>(hr));
        }
    }

    if (result != backdrop_) {
        HH_LOG_INFO(kLog, L"backdrop: {}", result == Backdrop::Mica ? L"Mica" : result == Backdrop::MicaLegacy ? L"Mica (legacy)" : L"none");
    }
    backdrop_ = result;
    themes_.setBackdropAvailable(backdrop_ != Backdrop::None);
    pacer_.requestFrame();
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

/**
 * @brief "x,y,w,h,dpi,max" of the restored floating rect (px).
 */
std::wstring WindowHost::placementString() const {
    if (hwnd_ == nullptr) {
        return floatingPlacement_;
    }
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!::GetWindowPlacement(hwnd_, &wp)) {
        return floatingPlacement_;
    }
    const RECT& r = wp.rcNormalPosition;
    const bool maximized = (wp.showCmd == SW_SHOWMAXIMIZED) || ::IsZoomed(hwnd_) != FALSE;
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    return std::format(L"{},{},{},{},{},{}", r.left, r.top, std::max<LONG>(1, r.right - r.left),
                       std::max<LONG>(1, r.bottom - r.top), dpi > 0 ? dpi : 96u, maximized ? 1 : 0);
}

/**
 * @brief Restores a placement string, validating it against the monitors.
 *
 * Off-screen or junk placements centre the window on the primary monitor;
 * a saved DPI that differs from the target monitor scales the size.
 * A hidden window stays hidden (the normal rect is still recorded).
 */
void WindowHost::applyPlacement(const std::wstring& placement) {
    if (hwnd_ == nullptr) {
        floatingPlacement_ = placement;
        return;
    }
    if (mode_ != WindowMode::Docked) {
        floatingPlacement_ = placement;
    }

    RECT rect{};
    float savedDpi = 96.0f;
    bool maximized = false;
    bool valid = !placement.empty() && parsePlacement(placement, rect, savedDpi, maximized);

    HMONITOR mon = valid ? ::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) : nullptr;
    if (valid && mon == nullptr) {
        valid = false;
    }

    if (valid) {
        // Scale the size when the saved DPI differs from the target monitor.
        const float targetDpi = dpiOfMonitor(mon);
        if (std::abs(targetDpi - savedDpi) > 0.5f && savedDpi > 0.0f) {
            const float factor = targetDpi / savedDpi;
            const LONG w = static_cast<LONG>(std::lround(static_cast<float>(rect.right - rect.left) * factor));
            const LONG h = static_cast<LONG>(std::lround(static_cast<float>(rect.bottom - rect.top) * factor));
            rect.right = rect.left + std::max<LONG>(1, w);
            rect.bottom = rect.top + std::max<LONG>(1, h);
        }
        // Enforce the minimum size at the target DPI.
        const DipScale target = DipScale::fromDpi(targetDpi);
        rect.right = std::max(rect.right, rect.left + target.toPxInt(spec_.minSizeDips.w));
        rect.bottom = std::max(rect.bottom, rect.top + target.toPxInt(spec_.minSizeDips.h));

        // At least 64x64 px must be inside the work area or the user cannot
        // grab the window.
        const RECT work = workAreaOf(mon);
        RECT overlap{};
        if (!::IntersectRect(&overlap, &rect, &work) || (overlap.right - overlap.left) < kMinVisiblePx ||
            (overlap.bottom - overlap.top) < kMinVisiblePx) {
            valid = false;
        }
    }

    if (!valid) {
        // Centre on the primary monitor at the spec size.
        const POINT origin{0, 0};
        mon = ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        const DipScale target = DipScale::fromDpi(dpiOfMonitor(mon));
        const RECT work = workAreaOf(mon);
        const LONG w = std::max<LONG>(1, target.toPxInt(std::max(spec_.initialDips.w, spec_.minSizeDips.w)));
        const LONG h = std::max<LONG>(1, target.toPxInt(std::max(spec_.initialDips.h, spec_.minSizeDips.h)));
        rect.left = work.left + std::max<LONG>(0, ((work.right - work.left) - w) / 2);
        rect.top = work.top + std::max<LONG>(0, ((work.bottom - work.top) - h) / 2);
        rect.right = rect.left + w;
        rect.bottom = rect.top + h;
        maximized = false;
        if (!placement.empty()) {
            HH_LOG_INFO(kLog, L"saved placement unusable; centring on the primary monitor");
        }
    }

    // Docked windows only remember the rect; the dock controller owns them.
    if (mode_ == WindowMode::Docked) {
        return;
    }

    const bool wasVisible = ::IsWindowVisible(hwnd_) != FALSE;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    wp.rcNormalPosition = rect;
    if (wasVisible) {
        wp.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    } else {
        // Hidden: SW_HIDE keeps it hidden. A maximized placement needs the
        // WS_MAXIMIZE bit, which only SW_SHOWMAXIMIZED sets; hide again at
        // once (no frame is composed in between).
        wp.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_HIDE;
    }
    if (!::SetWindowPlacement(hwnd_, &wp)) {
        HH_LOG_WARN(kLog, L"SetWindowPlacement failed: {}", Error::fromLastError(L"SetWindowPlacement").toString());
        return;
    }
    if (!wasVisible && maximized) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    if (mode_ == WindowMode::Floating) {
        floatingPlacement_ = placementString();
    }
}

/**
 * @brief Records the floating rect after size/move (used when undocking).
 */
void WindowHost::savePlacementIfFloating() {
    if (hwnd_ == nullptr || mode_ != WindowMode::Floating) {
        return;
    }
    if (::IsIconic(hwnd_)) {
        return;
    }
    floatingPlacement_ = placementString();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

/**
 * @brief Draws and presents one frame.
 *
 * Minimized windows never render. Hidden windows render only their first
 * frame after a (re)appearance (firstFramePresented_ == false) so the reveal
 * shows fresh content instead of a stretched stale buffer.
 */
void WindowHost::renderFrame() {
    pacer_.consume();
    if (hwnd_ == nullptr || !root_) {
        return;
    }
    if (::IsIconic(hwnd_)) {
        return;
    }
    if (!::IsWindowVisible(hwnd_) && firstFramePresented_) {
        return;
    }

    // Device recovery first; if it fails, try again on the next frame.
    if (device_.isLost() || !device_.valid()) {
        handleDeviceLost();
        if (!device_.valid()) {
            return;
        }
    }
    if (!surface_.bound()) {
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        if (!surface_.bind(hwnd_, device_, static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left)),
                           static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top)))) {
            return;
        }
    }

    root_->layoutIfNeeded(clientSizeDips());

    // Translucent themes clear to transparent so Mica shows through.
    const Theme& theme = themes_.current();
    const Color clear = theme.translucent ? Color::transparent() : theme.windowBackground;
    ID2D1DeviceContext1* ctx = surface_.beginFrame(scale_.dpi, clear);
    if (ctx == nullptr) {
        // Usually a lost device: mark it so the next frame recovers.
        if (!device_.isLost()) {
            HH_LOG_DEBUG(kLog, L"beginFrame returned no context");
        }
        pacer_.requestFrame();
        return;
    }
    canvas_.begin(ctx, scale_, theme, text_);
    root_->paintAll(canvas_);
    canvas_.end();

    const HRESULT hr = surface_.endFrame();
    if (isDeviceLostHr(hr)) {
        HH_LOG_WARN(kLog, L"present reported device loss ({:#010x})", static_cast<uint32_t>(hr));
        device_.markLost();
        pacer_.requestFrame();
        return;
    }
    if (FAILED(hr)) {
        HH_LOG_DEBUG(kLog, L"present failed ({:#010x})", static_cast<uint32_t>(hr));
    }
    firstFramePresented_ = true;
}

/**
 * @brief Re-creates the device and every device-bound object.
 */
void WindowHost::handleDeviceLost() {
    HH_LOG_WARN(kLog, L"graphics device lost; recreating");
    canvas_.resetDeviceResources();
    surface_.unbind();
    if (popup_) {
        popup_->destroy();
    }
    if (!device_.recreate()) {
        HH_LOG_ERROR(kLog, L"device recreation failed; will retry");
        return;
    }
    if (hwnd_ == nullptr) {
        return;
    }
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    if (!surface_.bind(hwnd_, device_, static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left)),
                       static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top)))) {
        HH_LOG_ERROR(kLog, L"surface rebind after device loss failed");
    }
    if (popup_ && !popup_->create(hwnd_)) {
        HH_LOG_WARN(kLog, L"popup recreation after device loss failed");
    }
    if (root_) {
        root_->invalidateLayout();
    }
    pacer_.requestFrame();
}

/**
 * @brief Renders the current frame and reads it back as BGRA.
 */
bool WindowHost::captureFrame(std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    if (hwnd_ == nullptr) {
        return false;
    }
    // Force a fresh frame even when the window is hidden.
    firstFramePresented_ = false;
    renderFrame();
    if (!surface_.bound()) {
        return false;
    }
    return surface_.readback(bgra, w, h);
}

/**
 * @brief Posts WM_QUIT to this thread's queue.
 */
void WindowHost::quit(int code) {
    ::PostQuitMessage(code);
}

/**
 * @brief The message / frame loop.
 */
int WindowHost::runLoop(const std::function<void()>& tickFn) {
    if (tickFn) {
        onTick = tickFn;
    }
    if (hwnd_ != nullptr && tickTimer_ == 0) {
        tickTimer_ = ::SetTimer(hwnd_, kTickTimerId, 1000, nullptr);
    }

    MSG msg{};
    for (;;) {
        // What the wait needs to know: do we want a frame, are we visible,
        // and when is the next timeline timer due?
        const bool windowVisible = hwnd_ != nullptr && ::IsWindowVisible(hwnd_) && !::IsIconic(hwnd_);
        const bool needFrame = pacer_.needsFrame() || (root_ && root_->timeline().hasActive());
        const bool popupVisible = popup_ && popup_->visible();
        const bool popupNeedsFrame = popupVisible && (popup_->pacer().needsFrame() || popup_->root().timeline().hasActive());

        // Block on the present waitable only while a frame is wanted AND the
        // window is visible: a hidden window's swap chain is never consumed
        // by DWM, so waiting on it would either spin or stall forever.
        HANDLE handles[1] = {nullptr};
        DWORD count = 0;
        int mainIndex = -1;
        if (needFrame && windowVisible) {
            HANDLE h = surface_.frameLatencyWaitable();
            if (h != nullptr) {
                mainIndex = static_cast<int>(count);
                handles[count++] = h;
            }
        }
        // The popup's surface waitable is not exposed; it renders whenever it
        // needs a frame (its content is tiny) and the timeout below wakes us.

        // Timeout = time to the next timer of either timeline, else INFINITE.
        DWORD timeout = INFINITE;
        const auto deadlineFor = [](Timeline& tl) -> DWORD {
            const std::optional<double> next = tl.nextDeadline();
            if (!next) {
                return INFINITE;
            }
            const double ms = (*next - tl.now()) * 1000.0;
            if (ms <= 0.0) {
                return 0;
            }
            return static_cast<DWORD>(std::min(ms, 3600000.0)) + 1;
        };
        if (root_) {
            timeout = std::min(timeout, deadlineFor(root_->timeline()));
        }
        if (popupVisible) {
            timeout = std::min(timeout, deadlineFor(popup_->root().timeline()));
        }
        // A wanted frame without a waitable (surface unbound) or a popup
        // frame must not sleep forever.
        if ((needFrame && windowVisible && mainIndex < 0) || popupNeedsFrame) {
            timeout = std::min<DWORD>(timeout, 16);
        }

        const DWORD wait = ::MsgWaitForMultipleObjectsEx(count, count > 0 ? handles : nullptr, timeout, QS_ALLINPUT,
                                                         MWMO_INPUTAVAILABLE);

        // Drain everything that arrived.
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                if (hwnd_ != nullptr && tickTimer_ != 0) {
                    ::KillTimer(hwnd_, kTickTimerId);
                    tickTimer_ = 0;
                }
                return static_cast<int>(msg.wParam);
            }
            if (msg.hwnd == nullptr && msg.message == WM_HH_RECREATE) {
                // The owner (AME) destroyed our window: come back floating.
                if (hwnd_ == nullptr) {
                    if (recreate(WindowMode::Floating) && onDestroyedByOwner) {
                        onDestroyedByOwner();
                    }
                }
                continue;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        // Animations / timers of both roots.
        if (root_ && root_->tickAnimations()) {
            pacer_.requestFrame();
        }
        if (popup_ && popup_->visible() && popup_->root().tickAnimations()) {
            popup_->requestFrame();
        }

        // Main frame: when the present slot is free, or when a timer wanted
        // one and we were not blocked on the waitable.
        const bool mainSignalled = mainIndex >= 0 && wait == WAIT_OBJECT_0 + static_cast<DWORD>(mainIndex);
        const bool timedOut = (wait == WAIT_TIMEOUT);
        const bool visibleNow = hwnd_ != nullptr && ::IsWindowVisible(hwnd_) && !::IsIconic(hwnd_);
        const bool wantNow = pacer_.needsFrame() || (root_ && root_->timeline().hasActive());
        if (visibleNow && wantNow && (mainSignalled || timedOut || mainIndex < 0)) {
            renderFrame();
        }

        // Popup frame.
        if (popup_ && popup_->visible() && (popup_->pacer().needsFrame() || popup_->root().timeline().hasActive())) {
            popup_->renderFrame();
        }
    }
}

// ---------------------------------------------------------------------------
// IWindowServices
// ---------------------------------------------------------------------------

/**
 * @brief Remembers and applies the cursor for the client area.
 */
void WindowHost::setCursor(CursorKind cursor) {
    HCURSOR h = cursorFor(cursor);
    if (h == nullptr) {
        h = ::LoadCursorW(nullptr, IDC_ARROW);
    }
    currentCursor_ = h;
    if (hwnd_ == nullptr) {
        return;
    }
    // Only touch the live cursor while it is ours to set.
    POINT pt{};
    if (::GetCapture() == hwnd_ || (::GetCursorPos(&pt) && ::WindowFromPoint(pt) == hwnd_)) {
        ::SetCursor(currentCursor_);
    }
}

/**
 * @brief SetCapture / ReleaseCapture, flagging our own release so the
 *        resulting WM_CAPTURECHANGED is not mistaken for a cancel.
 */
void WindowHost::captureMouse(bool capture) {
    if (hwnd_ == nullptr) {
        return;
    }
    if (capture) {
        ::SetCapture(hwnd_);
        return;
    }
    if (::GetCapture() == hwnd_) {
        ::SetPropW(hwnd_, kPropReleasingCapture, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
        ::ReleaseCapture();
        ::RemovePropW(hwnd_, kPropReleasingCapture);
    }
}

/**
 * @brief Client size in dips.
 */
Size WindowHost::clientSizeDips() const {
    if (hwnd_ == nullptr) {
        return {0.0f, 0.0f};
    }
    RECT rc{};
    if (!::GetClientRect(hwnd_, &rc)) {
        return {0.0f, 0.0f};
    }
    return {scale_.toDip(static_cast<float>(rc.right - rc.left)), scale_.toDip(static_cast<float>(rc.bottom - rc.top))};
}

/**
 * @brief Root dips -> screen pixels.
 */
POINT WindowHost::rootToScreenPx(Point rootPt) const {
    POINT p{scale_.toPxInt(rootPt.x), scale_.toPxInt(rootPt.y)};
    if (hwnd_ != nullptr) {
        ::ClientToScreen(hwnd_, &p);
    }
    return p;
}

/**
 * @brief Screen pixels -> root dips.
 */
Point WindowHost::screenPxToRoot(POINT pt) const {
    if (hwnd_ != nullptr) {
        ::ScreenToClient(hwnd_, &pt);
    }
    return {scale_.toDip(static_cast<float>(pt.x)), scale_.toDip(static_cast<float>(pt.y))};
}

/**
 * @brief True when this window is the foreground window.
 */
bool WindowHost::isActiveWindow() const {
    return hwnd_ != nullptr && (::GetForegroundWindow() == hwnd_ || ::GetActiveWindow() == hwnd_);
}

/**
 * @brief Opens the popup window anchored to a root-space rect.
 */
void WindowHost::showPopupWindow(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                                 Size contentSize, std::function<void()> onDismiss) {
    if (!popup_ || hwnd_ == nullptr || popup_->hwnd() == nullptr) {
        // No popup window: tell the caller it closed so its state resets.
        HH_LOG_WARN(kLog, L"showPopupWindow: popup window unavailable");
        if (onDismiss) {
            onDismiss();
        }
        return;
    }
    const POINT tl = rootToScreenPx(anchorRoot.origin());
    const POINT br = rootToScreenPx({anchorRoot.right(), anchorRoot.bottom()});
    RECT anchorPx{tl.x, tl.y, std::max(tl.x, br.x), std::max(tl.y, br.y)};
    popup_->show(std::move(content), anchorPx, placement, contentSize, std::move(onDismiss));
}

/**
 * @brief Closes the popup window if open.
 */
void WindowHost::dismissPopupWindow() {
    if (popup_ && popup_->visible()) {
        popup_->dismiss();
    }
}

/**
 * @brief Whether the popup window is showing.
 */
bool WindowHost::popupWindowVisible() const {
    return popup_ && popup_->visible();
}

/**
 * @brief Moves the IME composition/candidate windows and the (hidden)
 *        system caret to the text caret so IMEs, Magnifier and Narrator
 *        follow the insertion point.
 */
void WindowHost::setImeCaret(const Rect& caretRoot) {
    if (hwnd_ == nullptr) {
        return;
    }
    const int x = scale_.toPxInt(caretRoot.x);
    const int y = scale_.toPxInt(caretRoot.y);
    const int h = std::max(1, scale_.toPxInt(std::max(1.0f, caretRoot.h)));

    // System caret (never shown; painted by the widget) for accessibility.
    if (::GetFocus() == hwnd_) {
        ::CreateCaret(hwnd_, nullptr, 1, h);
        ::SetCaretPos(x, y);
    }

    HIMC imc = ::ImmGetContext(hwnd_);
    if (imc == nullptr) {
        return;
    }
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = POINT{x, y};
    if (!::ImmSetCompositionWindow(imc, &cf)) {
        HH_LOG_DEBUG(kLog, L"ImmSetCompositionWindow failed");
    }
    CANDIDATEFORM cand{};
    cand.dwIndex = 0;
    cand.dwStyle = CFS_CANDIDATEPOS;
    cand.ptCurrentPos = POINT{x, y + h};
    if (!::ImmSetCandidateWindow(imc, &cand)) {
        HH_LOG_DEBUG(kLog, L"ImmSetCandidateWindow failed");
    }
    ::ImmReleaseContext(hwnd_, imc);
}

/**
 * @brief Associates / disassociates the IME context (no composition while
 *        shortcuts are the only thing keys can mean).
 */
void WindowHost::setImeEnabled(bool enabled) {
    if (hwnd_ == nullptr) {
        return;
    }
    if (enabled) {
        if (!::ImmAssociateContextEx(hwnd_, nullptr, IACE_DEFAULT)) {
            HH_LOG_DEBUG(kLog, L"ImmAssociateContextEx(IACE_DEFAULT) failed");
        }
    } else {
        ::ImmAssociateContext(hwnd_, nullptr);
        ::DestroyCaret();
    }
}

/**
 * @brief Puts Unicode text on the clipboard.
 */
bool WindowHost::clipboardSetText(const std::wstring& text) {
    if (!::OpenClipboard(hwnd_)) {
        HH_LOG_DEBUG(kLog, L"OpenClipboard failed: {}", Error::fromLastError(L"OpenClipboard").toString());
        return false;
    }
    bool ok = false;
    if (::EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem != nullptr) {
            void* dst = ::GlobalLock(mem);
            if (dst != nullptr) {
                std::memcpy(dst, text.c_str(), bytes);
                ::GlobalUnlock(mem);
                // The clipboard owns the block once SetClipboardData succeeds.
                if (::SetClipboardData(CF_UNICODETEXT, mem) != nullptr) {
                    ok = true;
                } else {
                    ::GlobalFree(mem);
                }
            } else {
                ::GlobalFree(mem);
            }
        }
    }
    ::CloseClipboard();
    return ok;
}

/**
 * @brief Reads Unicode text from the clipboard (empty when none).
 */
std::wstring WindowHost::clipboardGetText() {
    std::wstring result;
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return result;
    }
    if (!::OpenClipboard(hwnd_)) {
        return result;
    }
    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (data != nullptr) {
        const auto* src = static_cast<const wchar_t*>(::GlobalLock(data));
        if (src != nullptr) {
            // Bound the scan by the block size; never trust the terminator.
            const size_t maxChars = ::GlobalSize(data) / sizeof(wchar_t);
            size_t len = 0;
            while (len < maxChars && src[len] != L'\0') {
                ++len;
            }
            result.assign(src, len);
            ::GlobalUnlock(data);
        }
    }
    ::CloseClipboard();
    return result;
}

// ---------------------------------------------------------------------------
// Chrome helpers
// ---------------------------------------------------------------------------

/**
 * @brief Hover state of the maximize button (Snap Layouts flyout target).
 */
void WindowHost::setMaximizeHover(bool on) {
    if (maximizeHover_ == on) {
        return;
    }
    maximizeHover_ = on;
    if (root_ && root_->chromeProvider() != nullptr) {
        root_->chromeProvider()->setChromeMaximizeHover(on);
    }
    pacer_.requestFrame();
}

/**
 * @brief WM_NCCALCSIZE: the client area is the whole window; when maximized
 *        the invisible frame that Windows pushes off-screen is trimmed.
 */
LRESULT WindowHost::onNcCalcSize(WPARAM wp, LPARAM lp) {
    if (wp == FALSE || lp == 0) {
        return 0;
    }
    if (mode_ == WindowMode::Docked) {
        return 0;
    }
    if (!::IsZoomed(hwnd_)) {
        return 0;
    }
    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    const int padded = ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const int frameX = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi) + padded;
    const int frameY = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi) + padded;
    RECT& r = params->rgrc[0];
    r.left += frameX;
    r.right -= frameX;
    r.top += frameY;
    r.bottom -= frameY;
    return 0;
}

/**
 * @brief WM_NCHITTEST: resize bands (floating, not maximized), then the
 *        chrome provider's caption / maximize answers.
 */
LRESULT WindowHost::onNcHitTest(LPARAM lp) {
    if (mode_ == WindowMode::Docked || hwnd_ == nullptr) {
        return HTCLIENT;
    }
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ::ScreenToClient(hwnd_, &pt);
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    if (!::IsZoomed(hwnd_)) {
        // System frame width in px for the edges, 16 dip squares for corners.
        const UINT dpi = ::GetDpiForWindow(hwnd_);
        const int frame = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int corner = std::max(frame, scale_.toPxInt(kResizeCornerDips));
        const bool onLeft = pt.x < frame;
        const bool onRight = pt.x >= w - frame;
        const bool onTop = pt.y < frame;
        const bool onBottom = pt.y >= h - frame;
        if (onTop || onBottom || onLeft || onRight) {
            const bool nearLeft = pt.x < corner;
            const bool nearRight = pt.x >= w - corner;
            const bool nearTop = pt.y < corner;
            const bool nearBottom = pt.y >= h - corner;
            if ((onTop && nearLeft) || (onLeft && nearTop)) return HTTOPLEFT;
            if ((onTop && nearRight) || (onRight && nearTop)) return HTTOPRIGHT;
            if ((onBottom && nearLeft) || (onLeft && nearBottom)) return HTBOTTOMLEFT;
            if ((onBottom && nearRight) || (onRight && nearBottom)) return HTBOTTOMRIGHT;
            if (onTop) return HTTOP;
            if (onBottom) return HTBOTTOM;
            if (onLeft) return HTLEFT;
            return HTRIGHT;
        }
    }

    if (!root_) {
        return HTCLIENT;
    }
    const Point dips{scale_.toDip(static_cast<float>(pt.x)), scale_.toDip(static_cast<float>(pt.y))};
    switch (root_->chromeHitTest(dips)) {
    case ChromeHit::Caption: return HTCAPTION;
    case ChromeHit::Maximize: return HTMAXBUTTON;
    case ChromeHit::Minimize:
    case ChromeHit::Close:
    case ChromeHit::None:
    default: return HTCLIENT;
    }
}

/**
 * @brief WM_SIZE (not minimized): resize the swap chain, re-layout and paint
 *        synchronously so DefWindowProc's modal loop shows fresh frames.
 */
void WindowHost::onSize(UINT pxW, UINT pxH) {
    const UINT w = std::max<UINT>(1, pxW);
    const UINT h = std::max<UINT>(1, pxH);
    if (surface_.bound()) {
        surface_.resize(w, h);
    } else if (hwnd_ != nullptr && device_.valid()) {
        surface_.bind(hwnd_, device_, w, h);
    }
    if (root_) {
        root_->invalidateLayout();
    }
    renderFrame();
    savePlacementIfFloating();
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

/**
 * @brief Static thunk: binds the instance on WM_NCCREATE, then forwards.
 */
LRESULT CALLBACK WindowHost::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        auto* self = cs != nullptr ? static_cast<WindowHost*>(cs->lpCreateParams) : nullptr;
        if (self != nullptr) {
            self->hwnd_ = hwnd;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    auto* self = reinterpret_cast<WindowHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr || self->hwnd_ != hwnd) {
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    const LRESULT result = self->handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return result;
}

/**
 * @brief Instance message handler.
 */
LRESULT WindowHost::handle(UINT msg, WPARAM wp, LPARAM lp) {
    // Client px (signed, valid during capture) -> root dips.
    const auto toRoot = [this](LPARAM l) -> Point {
        return {scale_.toDip(static_cast<float>(GET_X_LPARAM(l))), scale_.toDip(static_cast<float>(GET_Y_LPARAM(l)))};
    };

    // Private app messages first: the popup's close request and the app's own.
    if (msg >= WM_APP && msg <= WM_APP + 0xFF) {
        if (msg == WM_HH_POPUP_DISMISS) {
            dismissPopupWindow();
            return 0;
        }
        if (onAppMessage && onAppMessage(msg, wp, lp)) {
            return 0;
        }
        return ::DefWindowProcW(hwnd_, msg, wp, lp);
    }

    switch (msg) {
    // ---- frame / geometry ------------------------------------------------
    case WM_NCCALCSIZE:
        return onNcCalcSize(wp, lp);

    case WM_NCHITTEST:
        return onNcHitTest(lp);

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        if (::BeginPaint(hwnd_, &ps) != nullptr) {
            ::EndPaint(hwnd_, &ps);
        }
        renderFrame();
        return 0;
    }

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            return 0;
        }
        onSize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_MOVE:
        savePlacementIfFloating();
        return 0;

    case WM_ENTERSIZEMOVE:
        inSizeMove_ = true;
        sizeMoveTimer_ = ::SetTimer(hwnd_, kSizeMoveTimerId, USER_TIMER_MINIMUM, nullptr);
        return 0;

    case WM_EXITSIZEMOVE:
        inSizeMove_ = false;
        if (sizeMoveTimer_ != 0) {
            ::KillTimer(hwnd_, kSizeMoveTimerId);
            sizeMoveTimer_ = 0;
        }
        savePlacementIfFloating();
        pacer_.requestFrame();
        return 0;

    case WM_GETMINMAXINFO: {
        // Only the minimum track size; ptMaxSize stays untouched so the
        // maximized inset in WM_NCCALCSIZE is not applied twice.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        if (mmi != nullptr) {
            const UINT dpi = hwnd_ != nullptr ? ::GetDpiForWindow(hwnd_) : 96;
            const DipScale s = DipScale::fromDpi(dpi > 0 ? static_cast<float>(dpi) : 96.0f);
            mmi->ptMinTrackSize.x = std::max(1, s.toPxInt(spec_.minSizeDips.w));
            mmi->ptMinTrackSize.y = std::max(1, s.toPxInt(spec_.minSizeDips.h));
        }
        return 0;
    }

    case WM_DPICHANGED: {
        const UINT dpi = HIWORD(wp);
        scale_ = DipScale::fromDpi(dpi > 0 ? static_cast<float>(dpi) : 96.0f);
        if (mode_ == WindowMode::Floating) {
            const auto* suggested = reinterpret_cast<const RECT*>(lp);
            if (suggested != nullptr) {
                ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                               suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        // Docked: the dock controller supplies the next bounds.
        if (root_) {
            root_->dpiChanged();
            root_->invalidateLayout();
        }
        if (onDpiChanged) {
            onDpiChanged();
        }
        pacer_.requestFrame();
        return 0;
    }

    case WM_SHOWWINDOW:
        // Owner-driven show/hide (AME minimized) also freezes the clock.
        if (root_) {
            if (wp != FALSE) {
                if (root_->timeline().paused()) {
                    root_->timeline().resume();
                }
                pacer_.requestFrame();
            } else if (!root_->timeline().paused()) {
                root_->timeline().pause();
            }
        }
        break;

    case WM_DISPLAYCHANGE:
        pacer_.requestFrame();
        break;

    // ---- non-client mouse (maximize button / caption) --------------------
    case WM_NCMOUSEMOVE:
        if (wp == HTMAXBUTTON) {
            setMaximizeHover(true);
            if (!trackingNcMouse_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
                tme.hwndTrack = hwnd_;
                if (::TrackMouseEvent(&tme)) {
                    trackingNcMouse_ = true;
                }
            }
            return 0;
        }
        setMaximizeHover(false);
        break;

    case WM_NCMOUSELEAVE: {
        trackingNcMouse_ = false;
        // The Snap Layouts flyout steals the pointer while it is still over
        // the button: only un-hover when the cursor really left the rect.
        if (maximizeHover_ && root_ && root_->chromeProvider() != nullptr) {
            const Rect r = root_->chromeProvider()->chromeMaximizeRect();
            POINT cursor{};
            if (!r.isEmpty() && ::GetCursorPos(&cursor)) {
                const POINT tl = rootToScreenPx(r.origin());
                const POINT br = rootToScreenPx({r.right(), r.bottom()});
                RECT screen{tl.x, tl.y, br.x, br.y};
                if (::PtInRect(&screen, cursor)) {
                    break;
                }
            }
        }
        setMaximizeHover(false);
        break;
    }

    case WM_NCLBUTTONDOWN:
        if (wp == HTMAXBUTTON) {
            return 0;   // swallowed; the release toggles
        }
        break;

    case WM_NCLBUTTONUP:
        if (wp == HTMAXBUTTON) {
            toggleMaximize();
            return 0;
        }
        break;

    case WM_NCLBUTTONDBLCLK:
        if (wp == HTMAXBUTTON) {
            return 0;
        }
        break;

    case WM_NCRBUTTONUP:
        // HTCAPTION: DefWindowProc shows the system menu.
        break;

    // ---- client mouse -----------------------------------------------------
    case WM_MOUSEMOVE: {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            if (::TrackMouseEvent(&tme)) {
                trackingMouse_ = true;
            }
        }
        setMaximizeHover(false);
        if (root_) {
            root_->dispatchMouseMove(toRoot(lp), modifiersNow());
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (root_) {
            root_->dispatchMouseLeave();
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK: {
        // Floating windows take keyboard focus on click; docked ones only
        // when WM_MOUSEACTIVATE allowed activation.
        if (mode_ == WindowMode::Floating && ::GetFocus() != hwnd_) {
            ::SetFocus(hwnd_);
        }
        MouseButton button = MouseButton::Left;
        if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) {
            button = MouseButton::Right;
        } else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK) {
            button = MouseButton::Middle;
        }
        const bool dbl = (msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK);
        if (root_) {
            root_->dispatchMouseDown(toRoot(lp), button, modifiersNow(), dbl ? 2 : 1);
        }
        // Robustness: a popup that survived the root's dismissal closes when
        // the click was outside it.
        if (popup_ && popup_->visible()) {
            POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ::ClientToScreen(hwnd_, &screen);
            popup_->ownerClickedAt(screen);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        MouseButton button = MouseButton::Left;
        if (msg == WM_RBUTTONUP) {
            button = MouseButton::Right;
        } else if (msg == WM_MBUTTONUP) {
            button = MouseButton::Middle;
        }
        if (root_) {
            root_->dispatchMouseUp(toRoot(lp), button, modifiersNow());
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // Wheel messages carry screen coordinates and go to the focus window,
        // so a visible popup under the cursor gets them forwarded.
        POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp));

        // Precision latch: one non-notch delta marks the next 500 ms as
        // touchpad input even if a later delta happens to equal a notch.
        const ULONGLONG nowMs = ::GetTickCount64();
        ULONGLONG until = reinterpret_cast<ULONGLONG>(::GetPropW(hwnd_, kPropPreciseWheelUntil));
        if (static_cast<int>(delta) % WHEEL_DELTA != 0) {
            until = nowMs + kPreciseWheelLatchMs;
            ::SetPropW(hwnd_, kPropPreciseWheelUntil, reinterpret_cast<HANDLE>(until));
        }
        const bool precise = nowMs < until;

        UINT lines = 3;
        if (msg == WM_MOUSEWHEEL) {
            if (!::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0)) {
                lines = 3;
            }
        } else if (!::SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &lines, 0)) {
            lines = 3;
        }
        const int linesPerNotch = (lines == WHEEL_PAGESCROLL) ? -1 : static_cast<int>(std::max<UINT>(1, lines));
        const float dy = (msg == WM_MOUSEWHEEL) ? delta : 0.0f;
        const float dx = (msg == WM_MOUSEHWHEEL) ? delta : 0.0f;

        if (popup_ && popup_->visible() && popup_->hwnd() != nullptr) {
            RECT pr{};
            if (::GetWindowRect(popup_->hwnd(), &pr) && ::PtInRect(&pr, screen)) {
                popup_->root().dispatchWheel(popup_->screenPxToRoot(screen), dy, dx, precise, linesPerNotch, modifiersNow());
                return 0;
            }
        }
        if (root_) {
            root_->dispatchWheel(screenPxToRoot(screen), dy, dx, precise, linesPerNotch, modifiersNow());
        }
        return 0;
    }

    case WM_MOUSEACTIVATE: {
        if (mode_ == WindowMode::Floating) {
            return MA_ACTIVATE;
        }
        // Docked: stay passive unless the click needs the keyboard.
        if (popup_ && popup_->visible()) {
            return MA_ACTIVATE;
        }
        POINT cursor{};
        if (root_ && ::GetCursorPos(&cursor)) {
            Widget* w = root_->hitTest(screenPxToRoot(cursor));
            if (w != nullptr && w->focusable()) {
                return MA_ACTIVATE;
            }
        }
        return MA_NOACTIVATE;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            ::SetCursor(currentCursor_ != nullptr ? currentCursor_ : ::LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;

    // ---- activation / capture ---------------------------------------------
    case WM_ACTIVATE: {
        const bool active = LOWORD(wp) != WA_INACTIVE;
        if (root_) {
            root_->setWindowActive(active);
            if (!active) {
                // Keep the popup when it is the popup taking over (it never
                // activates, but stay correct if that ever changes).
                const HWND other = reinterpret_cast<HWND>(lp);
                const bool popupTookOver = popup_ && popup_->visible() && other != nullptr && other == popup_->hwnd();
                if (!popupTookOver) {
                    root_->cancelInteraction();
                }
                setMaximizeHover(false);
            }
        }
        if (active && mode_ == WindowMode::Floating) {
            // Win10 sometimes drops the extension on activation changes.
            MARGINS margins{0, 0, 0, 1};
            ::DwmExtendFrameIntoClientArea(hwnd_, &margins);
        }
        if (onActivate) {
            onActivate(active);
        }
        pacer_.requestFrame();
        break;
    }

    case WM_SETFOCUS:
        if (root_) {
            root_->setWindowActive(true);
        }
        pacer_.requestFrame();
        return 0;

    case WM_KILLFOCUS:
        if (root_) {
            root_->setWindowActive(false);
        }
        ::DestroyCaret();
        pacer_.requestFrame();
        return 0;

    case WM_CANCELMODE:
        if (root_) {
            root_->cancelInteraction();
        }
        break;

    case WM_CAPTURECHANGED: {
        const HWND gaining = reinterpret_cast<HWND>(lp);
        const bool selfRelease = ::GetPropW(hwnd_, kPropReleasingCapture) != nullptr;
        const bool toPopup = popup_ && gaining != nullptr && gaining == popup_->hwnd();
        if (gaining != hwnd_ && !selfRelease && !toPopup && root_) {
            root_->cancelInteraction();
        }
        return 0;
    }

    // ---- keyboard ------------------------------------------------------------
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = static_cast<UINT>(wp);
        const Modifiers mods = modifiersNow();
        const bool repeat = (lp & (1 << 30)) != 0;
        if (vk == VK_F4 && mods.alt) {
            break;   // Alt+F4 -> DefWindowProc -> WM_CLOSE
        }
        if (popup_ && popup_->visible()) {
            if (popup_->forwardKeyDown(vk, mods, repeat)) {
                return 0;
            }
            break;
        }
        if (root_ && root_->dispatchKeyDown(vk, mods, repeat)) {
            return 0;
        }
        break;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (root_ && root_->dispatchKeyUp(static_cast<UINT>(wp), modifiersNow())) {
            return 0;
        }
        break;

    case WM_CHAR: {
        const wchar_t ch = static_cast<wchar_t>(wp);
        char32_t cp = 0;
        if (IS_HIGH_SURROGATE(ch)) {
            highSurrogate_ = ch;
            return 0;
        }
        if (IS_LOW_SURROGATE(ch)) {
            if (highSurrogate_ == 0) {
                return 0;   // unpaired: drop
            }
            cp = 0x10000 + ((static_cast<char32_t>(highSurrogate_) - 0xD800) << 10) + (static_cast<char32_t>(ch) - 0xDC00);
            highSurrogate_ = 0;
        } else {
            highSurrogate_ = 0;
            cp = static_cast<char32_t>(ch);
        }
        // Control characters are handled as keys (Backspace, Enter, Tab).
        if (cp < 0x20 || cp == 0x7F) {
            return 0;
        }
        if (popup_ && popup_->visible()) {
            popup_->forwardChar(cp);
            return 0;
        }
        if (root_) {
            root_->dispatchChar(cp);
        }
        return 0;
    }

    case WM_UNICHAR: {
        if (wp == UNICODE_NOCHAR) {
            return TRUE;   // yes, we take WM_UNICHAR
        }
        const auto cp = static_cast<char32_t>(wp);
        if (cp < 0x20 || cp == 0x7F) {
            return 0;
        }
        if (popup_ && popup_->visible()) {
            popup_->forwardChar(cp);
        } else if (root_) {
            root_->dispatchChar(cp);
        }
        return 0;
    }

    // ---- system notifications -------------------------------------------
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        if (root_) {
            root_->timeline().setReducedMotion(ThemeManager::readReducedMotion());
        }
        if (onSystemSettingsChanged) {
            onSystemSettingsChanged();
        }
        refreshBackdrop();
        pacer_.requestFrame();
        break;

    case WM_DWMCOLORIZATIONCOLORCHANGED:
        if (onSystemSettingsChanged) {
            onSystemSettingsChanged();
        }
        refreshBackdrop();
        pacer_.requestFrame();
        break;

    case WM_DWMCOMPOSITIONCHANGED:
    case WM_WTSSESSION_CHANGE:
    case WM_POWERBROADCAST:
        refreshBackdrop();
        renderFrame();
        break;

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        std::vector<std::wstring> files;
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
            if (len == 0) {
                continue;
            }
            std::wstring path(static_cast<size_t>(len) + 1, L'\0');
            const UINT copied = ::DragQueryFileW(drop, i, path.data(), len + 1);
            path.resize(std::min<size_t>(copied, len));
            if (!path.empty()) {
                files.push_back(std::move(path));
            }
        }
        ::DragFinish(drop);
        if (onFilesDropped && !files.empty()) {
            onFilesDropped(files);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kSizeMoveTimerId) {
            // Inside DefWindowProc's modal loop: keep animations moving.
            bool want = pacer_.needsFrame();
            if (root_ && root_->tickAnimations()) {
                want = true;
            }
            if (want) {
                renderFrame();
            }
            if (popup_ && popup_->visible()) {
                // Sample first, then decide: the sample itself may request a frame.
                const bool popupTicked = popup_->root().tickAnimations();
                if (popupTicked || popup_->pacer().needsFrame()) {
                    popup_->renderFrame();
                }
            }
            return 0;
        }
        if (wp == kTickTimerId) {
            if (onTick) {
                onTick();
            }
            return 0;
        }
        break;

    // ---- lifetime ------------------------------------------------------------
    case WM_CLOSE:
        if (onCloseRequested) {
            onCloseRequested();
        } else {
            quit(0);
        }
        return 0;

    case WM_DESTROY:
        if (!destroyingSelf_) {
            // The owner (AME) took us down: drop everything bound to the HWND
            // and ask the loop to build a floating window again.
            HH_LOG_WARN(kLog, L"window destroyed by its owner; scheduling recreation");
            if (popup_) {
                popup_->destroy();
            }
            canvas_.resetDeviceResources();
            surface_.unbind();
            sizeMoveTimer_ = 0;
            tickTimer_ = 0;
            ::WTSUnRegisterSessionNotification(hwnd_);
            if (!::PostThreadMessageW(::GetCurrentThreadId(), WM_HH_RECREATE, 0, 0)) {
                HH_LOG_ERROR(kLog, L"PostThreadMessage(WM_HH_RECREATE) failed: {}",
                             Error::fromLastError(L"PostThreadMessageW").toString());
            }
        }
        // Self-destruction: quit() already posted WM_QUIT when wanted.
        return 0;

    case WM_NCDESTROY:
        ::RemovePropW(hwnd_, kPropReleasingCapture);
        ::RemovePropW(hwnd_, kPropPreciseWheelUntil);
        break;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace hh::ui
