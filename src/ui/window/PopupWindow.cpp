// ---------------------------------------------------------------------------
// PopupWindow.cpp - a separate, non-activating HWND for menus.
//
// Why a second window at all: the docked panel can be sized down to a few
// hundred pixels by the AME user, and a menu drawn inside it would be
// clipped. A WS_EX_NOACTIVATE top-most popup owned by the main window can
// hang over the panel edge, gets DWM's small rounded corners, and never
// takes keyboard focus away from the main window, which forwards keys here.
//
// Input model while open:
//   * the popup holds mouse capture, so a click anywhere outside its content
//     arrives as WM_xBUTTONDOWN with out-of-client coordinates -> dismiss;
//   * capture can be lost when our process is not in the foreground (docked
//     mode), so the owner also reports clicks (ownerClickedAt) and a low
//     frequency timer watches for button presses elsewhere;
//   * keys come from the owner through forwardKeyDown / forwardChar.
//
// The menu content is hosted in a PopupSurface widget that paints the drop
// shadow and the opaque rounded card. The surface stays the root content for
// the popup's lifetime; only the menu child is swapped. A dismissed menu is
// detached immediately but destroyed one message later, because dismissal is
// usually triggered from inside the menu's own mouse handler.
// ---------------------------------------------------------------------------
#include "ui/window/PopupWindow.h"

#include "core/Expected.h"
#include "core/Logger.h"
#include "platform/WinVersion.h"
#include "ui/core/PopupSurface.h"
#include "ui/window/Messages.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace hh::ui {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"PopupWindow";

/// Window class of the popup.
constexpr const wchar_t* kClassName = L"HdrHint.Popup";

/// Private messages (WM_USER range is safe: nobody else posts to this HWND).
constexpr UINT kMsgAcquireCapture = WM_USER + 1;   ///< take mouse capture once the opener's click finished
constexpr UINT kMsgPurgeRetired = WM_USER + 2;     ///< destroy menus that were dismissed from their own handlers

/// Timer that watches for clicks elsewhere when capture is unavailable.
constexpr UINT_PTR kOutsideClickTimer = 1;
constexpr UINT kOutsideClickIntervalMs = 50;

/// Gap between the anchor and the popup card.
constexpr float kAnchorGapDips = kPopupAnchorGap;

/// DWM attribute ids written as numbers so SDK gating cannot bite.
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaBorderColor = 34;
constexpr DWORD kDwmwaSystemBackdropType = 38;
constexpr DWORD kDwmCornerRoundSmall = 3;
constexpr DWORD kDwmBackdropNone = 1;
constexpr DWORD kDwmColorNone = 0xFFFFFFFE;

/// Whether the popup window class is registered for this process.
bool g_classRegistered = false;

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
 * @brief Current keyboard modifier state (GetKeyState is per message queue).
 */
Modifiers modifiersNow() {
    Modifiers m;
    m.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    m.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    m.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    return m;
}

/**
 * @brief Effective DPI of the monitor that contains @p rect (96 on failure).
 */
float monitorDpiFor(const RECT& rect) {
    HMONITOR mon = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
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
 * @brief Work area of the monitor nearest to @p rect (the whole virtual
 *        screen when monitor information is unavailable).
 */
RECT workAreaFor(const RECT& rect) {
    HMONITOR mon = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (mon != nullptr && ::GetMonitorInfoW(mon, &info)) {
        return info.rcWork;
    }
    // Fallback: the virtual screen bounds.
    RECT vs{};
    vs.left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    vs.top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    vs.right = vs.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return vs;
}

/**
 * @brief True for the HRESULTs that mean "the GPU device is gone".
 */
bool isDeviceLostHr(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == D2DERR_RECREATE_TARGET;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / window lifetime
// ---------------------------------------------------------------------------

/**
 * @brief Builds the popup's root view; the HWND comes later in create().
 */
PopupWindow::PopupWindow(GraphicsDevice& device, TextCache& text, ThemeManager& themes)
    : device_(device), text_(text), themes_(themes) {
    root_ = std::make_unique<RootView>(*this, themes_);
    root_->setContent(std::make_unique<PopupSurface>(shadowMargin_));
    scale_ = DipScale::fromDpi(96.0f);
}

/**
 * @brief Tears the window down; the root view goes with the members.
 */
PopupWindow::~PopupWindow() {
    destroy();
}

/**
 * @brief Registers the class (once) and creates the hidden popup HWND.
 */
bool PopupWindow::create(HWND owner) {
    if (hwnd_ != nullptr) {
        // Already created: only the owner may have changed (window recreation).
        if (owner_ != owner) {
            destroy();
        } else {
            return true;
        }
    }
    owner_ = owner;

    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    if (!g_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &PopupWindow::wndProc;
        wc.hInstance = inst;
        wc.hCursor = nullptr;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            HH_LOG_ERROR(kLog, L"RegisterClassEx failed: {}", Error::fromLastError(L"RegisterClassExW").toString());
            return false;
        }
        g_classRegistered = true;
    }

    // NOREDIRECTIONBITMAP: the swap chain is composed by DWM directly, so the
    // transparent shadow margin really is transparent. NOACTIVATE keeps the
    // keyboard focus in the owner; TOOLWINDOW keeps it off the taskbar.
    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    const DWORD style = WS_POPUP | WS_CLIPSIBLINGS;
    HWND hwnd = ::CreateWindowExW(exStyle, kClassName, L"", style, 0, 0, 64, 64, owner, nullptr, inst, this);
    if (hwnd == nullptr) {
        HH_LOG_ERROR(kLog, L"CreateWindowEx failed: {}", Error::fromLastError(L"CreateWindowExW").toString());
        hwnd_ = nullptr;
        return false;
    }
    hwnd_ = hwnd;

    // Small rounded corners, no system backdrop, no DWM border (we outline
    // the card ourselves so the hairline follows the theme).
    const DWORD corner = kDwmCornerRoundSmall;
    ::DwmSetWindowAttribute(hwnd_, kDwmwaWindowCornerPreference, &corner, sizeof(corner));
    const DWORD border = kDwmColorNone;
    ::DwmSetWindowAttribute(hwnd_, kDwmwaBorderColor, &border, sizeof(border));
    if (platform::supportsSystemBackdrop()) {
        const DWORD backdrop = kDwmBackdropNone;
        ::DwmSetWindowAttribute(hwnd_, kDwmwaSystemBackdropType, &backdrop, sizeof(backdrop));
    }
    const BOOL dark = themes_.current().isDark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd_, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));

    // Initial scale from the window's DPI; show() refines it per monitor.
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    scale_ = DipScale::fromDpi(dpi > 0 ? static_cast<float>(dpi) : 96.0f);

    // Bind a small surface now so the first show() only has to resize.
    if (!surface_.bind(hwnd_, device_, 64, 64)) {
        HH_LOG_WARN(kLog, L"initial surface bind failed; will retry on show");
    }
    HH_LOG_DEBUG(kLog, L"popup window created");
    return true;
}

/**
 * @brief Dismisses, unbinds the surface and destroys the HWND.
 */
void PopupWindow::destroy() {
    if (visible_) {
        dismiss();
    }
    // Retired menus can go now: no handler of theirs is on the stack once
    // the caller reached destroy().
    if (root_) {
        if (auto* surface = static_cast<PopupSurface*>(root_->content())) {
            surface->retireMenu();
            surface->purgeRetired();
        }
    }
    canvas_.resetDeviceResources();
    surface_.unbind();
    if (hwnd_ != nullptr) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        ::DestroyWindow(h);
    }
}

// ---------------------------------------------------------------------------
// Show / dismiss
// ---------------------------------------------------------------------------

/**
 * @brief Places the popup next to the anchor, attaches the content, and shows
 *        the window without activating it.
 */
void PopupWindow::show(std::unique_ptr<Widget> content, const RECT& anchorPx, PopupPlacement placement, Size sizeDips,
                       std::function<void()> onDismiss) {
    if (hwnd_ == nullptr || !root_) {
        HH_LOG_WARN(kLog, L"show: no window; popup dropped");
        if (onDismiss) {
            onDismiss();
        }
        return;
    }
    if (!content) {
        HH_LOG_WARN(kLog, L"show: null content; popup dropped");
        if (onDismiss) {
            onDismiss();
        }
        return;
    }

    // A popup replacing another one closes the first properly (its callback
    // runs) before the new content is attached.
    if (visible_) {
        dismiss();
    }

    auto* surface = static_cast<PopupSurface*>(root_->content());
    if (surface == nullptr) {
        HH_LOG_ERROR(kLog, L"show: popup surface missing");
        if (onDismiss) {
            onDismiss();
        }
        return;
    }

    // ---- geometry (all in screen pixels of the anchor's monitor) ----------
    const RECT work = workAreaFor(anchorPx);
    scale_ = DipScale::fromDpi(monitorDpiFor(anchorPx));
    const float marginDips = surface->margin();

    // Content that was not pre-measured measures itself against the work area.
    const float workWDips = scale_.toDip(static_cast<float>(work.right - work.left));
    const float workHDips = scale_.toDip(static_cast<float>(work.bottom - work.top));
    const float maxContentW = std::max(1.0f, workWDips - 2.0f * marginDips);
    const float maxContentH = std::max(1.0f, workHDips - 2.0f * marginDips);
    if (sizeDips.isEmpty()) {
        sizeDips = content->measure(Constraints::loose({maxContentW, maxContentH}));
    }
    sizeDips.w = std::clamp(sizeDips.w, 1.0f, maxContentW);
    sizeDips.h = std::clamp(sizeDips.h, 1.0f, maxContentH);

    const int contentW = std::max(1, scale_.toPxInt(sizeDips.w));
    const int contentH = std::max(1, scale_.toPxInt(sizeDips.h));
    const int marginPx = std::max(0, scale_.toPxInt(marginDips));
    const int gapPx = scale_.toPxInt(kAnchorGapDips);

    // Card position relative to the anchor.
    const bool fitsBelow = anchorPx.bottom + gapPx + contentH <= work.bottom;
    const bool fitsAbove = anchorPx.top - gapPx - contentH >= work.top;
    int cardX = anchorPx.left;
    int cardY = 0;
    switch (placement) {
    case PopupPlacement::AtPoint:
        cardY = anchorPx.top;
        break;
    case PopupPlacement::Above:
        // Above, unless that runs off the top and below would fit.
        cardY = (fitsAbove || !fitsBelow) ? anchorPx.top - gapPx - contentH : anchorPx.bottom + gapPx;
        break;
    case PopupPlacement::Below:
    case PopupPlacement::Auto:
    default:
        // Below, flipping above only when below leaves the work area.
        cardY = (fitsBelow || !fitsAbove) ? anchorPx.bottom + gapPx : anchorPx.top - gapPx - contentH;
        break;
    }

    // Keep the card inside the work area; the shadow may hang over the edge.
    cardX = std::clamp(cardX, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - contentW));
    cardY = std::clamp(cardY, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - contentH));

    const int winX = cardX - marginPx;
    const int winY = cardY - marginPx;
    const int winW = contentW + 2 * marginPx;
    const int winH = contentH + 2 * marginPx;

    // ---- content ------------------------------------------------------------
    Widget* menu = surface->setMenu(std::move(content));
    root_->focus().setKeyboardMode(false);
    if (menu != nullptr) {
        root_->focus().focus(menu);
    }
    onDismiss_ = std::move(onDismiss);
    visible_ = true;

    // Size the swap chain before the first paint so nothing is stretched.
    const UINT pxW = static_cast<UINT>(winW);
    const UINT pxH = static_cast<UINT>(winH);
    if (!surface_.bound()) {
        if (!surface_.bind(hwnd_, device_, pxW, pxH)) {
            HH_LOG_WARN(kLog, L"show: surface bind failed");
        }
    } else if (!surface_.resize(pxW, pxH)) {
        HH_LOG_WARN(kLog, L"show: surface resize failed");
    }

    // Move / size while still hidden, paint, then reveal without activation.
    ::SetWindowPos(hwnd_, HWND_TOPMOST, winX, winY, winW, winH, SWP_NOACTIVATE);
    root_->invalidateLayout();
    renderFrame();
    ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Capture is taken one message later: the opener's mouse-up handler is
    // usually still running and its ReleaseCapture would undo an immediate
    // SetCapture. The timer covers the cases where capture is not granted
    // (our process is not in the foreground while docked).
    ::PostMessageW(hwnd_, kMsgAcquireCapture, 0, 0);
    ::SetTimer(hwnd_, kOutsideClickTimer, kOutsideClickIntervalMs, nullptr);
    pacer_.requestFrame();
    HH_LOG_DEBUG(kLog, L"popup shown at {},{} size {}x{}", winX, winY, winW, winH);
}

/**
 * @brief Hides the popup, releases capture, detaches the content and fires
 *        the dismiss callback exactly once.
 */
void PopupWindow::dismiss() {
    if (!visible_) {
        return;
    }
    // Flag first: the capture release below re-enters through
    // WM_CAPTURECHANGED and must see "already closed".
    visible_ = false;

    if (hwnd_ != nullptr) {
        ::KillTimer(hwnd_, kOutsideClickTimer);
        if (::GetCapture() == hwnd_) {
            ::ReleaseCapture();
        }
        ::ShowWindow(hwnd_, SW_HIDE);
    }

    // Clear hover / press / focus before the menu leaves the tree, then
    // detach it. Destruction is deferred to kMsgPurgeRetired because this
    // call usually originates inside the menu's own handler.
    if (root_) {
        root_->cancelInteraction();
        root_->focus().focus(nullptr);
        if (auto* surface = static_cast<PopupSurface*>(root_->content())) {
            surface->retireMenu();
        }
    }
    if (hwnd_ != nullptr) {
        ::PostMessageW(hwnd_, kMsgPurgeRetired, 0, 0);
    }

    // Move the callback out first so a re-entrant show() cannot clobber it.
    std::function<void()> cb = std::move(onDismiss_);
    onDismiss_ = {};
    if (cb) {
        cb();
    }
    HH_LOG_DEBUG(kLog, L"popup dismissed");
}

/**
 * @brief The owner saw a click: dismiss when it landed outside the card.
 */
void PopupWindow::ownerClickedAt(POINT screenPx) {
    if (!visible_ || hwnd_ == nullptr) {
        return;
    }
    RECT wr{};
    if (!::GetWindowRect(hwnd_, &wr)) {
        dismiss();
        return;
    }
    // Only the card counts as "inside"; the shadow margin is outside.
    const int marginPx = scale_.toPxInt(shadowMargin_);
    RECT card{wr.left + marginPx, wr.top + marginPx, wr.right - marginPx, wr.bottom - marginPx};
    if (!::PtInRect(&card, screenPx)) {
        dismiss();
    }
}

// ---------------------------------------------------------------------------
// Keyboard forwarding
// ---------------------------------------------------------------------------

/**
 * @brief Key from the owner: the menu gets it first, Escape closes.
 */
bool PopupWindow::forwardKeyDown(UINT vk, Modifiers mods, bool repeat) {
    if (!visible_ || !root_) {
        return false;
    }
    if (root_->dispatchKeyDown(vk, mods, repeat)) {
        return true;
    }
    if (vk == VK_ESCAPE) {
        dismiss();
        return true;
    }
    return false;
}

/**
 * @brief Character from the owner (type-ahead in menus).
 */
bool PopupWindow::forwardChar(char32_t ch) {
    if (!visible_ || !root_) {
        return false;
    }
    return root_->dispatchChar(ch);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

/**
 * @brief Draws one frame with a transparent clear so the shadow margin and
 *        the rounded corners show whatever is behind the popup.
 */
void PopupWindow::renderFrame() {
    pacer_.consume();
    if (hwnd_ == nullptr || !root_ || !surface_.bound()) {
        return;
    }
    // The main window owns device recovery; until it re-creates us, skip.
    if (device_.isLost() || !device_.valid()) {
        return;
    }

    root_->layoutIfNeeded(clientSizeDips());
    ID2D1DeviceContext1* ctx = surface_.beginFrame(scale_.dpi, Color::transparent());
    if (ctx == nullptr) {
        return;
    }
    canvas_.begin(ctx, scale_, themes_.current(), text_);
    root_->paintAll(canvas_);
    canvas_.end();

    const HRESULT hr = surface_.endFrame();
    if (isDeviceLostHr(hr)) {
        HH_LOG_WARN(kLog, L"present reported device loss ({:#010x})", static_cast<uint32_t>(hr));
        device_.markLost();
    } else if (FAILED(hr)) {
        HH_LOG_DEBUG(kLog, L"present failed ({:#010x})", static_cast<uint32_t>(hr));
    }
}

/**
 * @brief Re-sizes the surface to the current client rect.
 */
void PopupWindow::resizeSurface() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT rc{};
    if (!::GetClientRect(hwnd_, &rc)) {
        return;
    }
    const UINT w = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    const UINT h = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    if (!surface_.bound()) {
        surface_.bind(hwnd_, device_, w, h);
    } else {
        surface_.resize(w, h);
    }
    if (root_) {
        root_->invalidateLayout();
    }
}

// ---------------------------------------------------------------------------
// IWindowServices
// ---------------------------------------------------------------------------

/**
 * @brief Applies the cursor immediately (we are under the pointer).
 */
void PopupWindow::setCursor(CursorKind cursor) {
    if (hwnd_ == nullptr) {
        return;
    }
    HCURSOR h = cursorFor(cursor);
    if (h != nullptr) {
        ::SetCursor(h);
    }
}

/**
 * @brief Capture is held for the whole time the popup is open; a widget
 *        releasing it must not take the click-outside detection with it.
 */
void PopupWindow::captureMouse(bool capture) {
    if (hwnd_ == nullptr) {
        return;
    }
    if (capture) {
        if (::GetCapture() != hwnd_) {
            ::SetCapture(hwnd_);
        }
        return;
    }
    // While visible we keep it (see the file header); otherwise let go.
    if (!visible_ && ::GetCapture() == hwnd_) {
        ::ReleaseCapture();
    }
}

/**
 * @brief Client size in dips.
 */
Size PopupWindow::clientSizeDips() const {
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
 * @brief Work area of the monitor nearest to the window, in this window's dips.
 */
Size PopupWindow::workAreaSizeDips() const {
    if (hwnd_ == nullptr) {
        return {0.0f, 0.0f};
    }
    const HMONITOR mon = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !::GetMonitorInfoW(mon, &mi)) {
        return {0.0f, 0.0f};
    }
    return {scale_.toDip(static_cast<float>(mi.rcWork.right - mi.rcWork.left)),
            scale_.toDip(static_cast<float>(mi.rcWork.bottom - mi.rcWork.top))};
}


/**
 * @brief Root dips -> screen pixels.
 */
POINT PopupWindow::rootToScreenPx(Point rootPt) const {
    POINT p{scale_.toPxInt(rootPt.x), scale_.toPxInt(rootPt.y)};
    if (hwnd_ != nullptr) {
        ::ClientToScreen(hwnd_, &p);
    }
    return p;
}

/**
 * @brief Screen pixels -> root dips.
 */
Point PopupWindow::screenPxToRoot(POINT pt) const {
    if (hwnd_ != nullptr) {
        ::ScreenToClient(hwnd_, &pt);
    }
    return {scale_.toDip(static_cast<float>(pt.x)), scale_.toDip(static_cast<float>(pt.y))};
}

/**
 * @brief Nested popups are not supported: the request replaces this one.
 */
void PopupWindow::showPopupWindow(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                                  Size contentSize, std::function<void()> onDismiss) {
    // Convert the anchor from our root space to screen pixels and re-show.
    const POINT tl = rootToScreenPx(anchorRoot.origin());
    const POINT br = rootToScreenPx({anchorRoot.right(), anchorRoot.bottom()});
    RECT anchorPx{tl.x, tl.y, br.x, br.y};
    show(std::move(content), anchorPx, placement, contentSize, std::move(onDismiss));
}

/**
 * @brief Puts text on the clipboard (owner-less clipboard is fine here).
 */
bool PopupWindow::clipboardSetText(const std::wstring& text) {
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
                // On success the clipboard owns the block; free it otherwise.
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
std::wstring PopupWindow::clipboardGetText() {
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
            // Never trust the terminator: bound the copy by the block size.
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
// Message handling
// ---------------------------------------------------------------------------

/**
 * @brief Static thunk: binds the instance on WM_NCCREATE, then forwards.
 */
LRESULT CALLBACK PopupWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        auto* self = cs != nullptr ? static_cast<PopupWindow*>(cs->lpCreateParams) : nullptr;
        if (self != nullptr) {
            self->hwnd_ = hwnd;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    auto* self = reinterpret_cast<PopupWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
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
LRESULT PopupWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    // Helpers shared by the mouse handlers: client px -> root dips, and the
    // "is this inside the card" test that decides between input and dismiss.
    const auto toRoot = [this](LPARAM l) -> Point {
        return {scale_.toDip(static_cast<float>(GET_X_LPARAM(l))), scale_.toDip(static_cast<float>(GET_Y_LPARAM(l)))};
    };
    const auto insideCard = [this](LPARAM l) -> bool {
        RECT rc{};
        if (hwnd_ == nullptr || !::GetClientRect(hwnd_, &rc)) {
            return false;
        }
        const int m = scale_.toPxInt(shadowMargin_);
        const int x = GET_X_LPARAM(l);
        const int y = GET_Y_LPARAM(l);
        return x >= m && y >= m && x < rc.right - m && y < rc.bottom - m;
    };

    switch (msg) {
    case WM_MOUSEACTIVATE:
        // Never take activation: the owner keeps keyboard focus.
        return MA_NOACTIVATE;

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
        if (wp != SIZE_MINIMIZED) {
            resizeSurface();
        }
        return 0;

    case WM_DPICHANGED: {
        // Popups are re-positioned on every show(); just track the scale so
        // hit-testing stays correct if the monitor DPI changes while open.
        const UINT dpi = HIWORD(wp);
        scale_ = DipScale::fromDpi(dpi > 0 ? static_cast<float>(dpi) : 96.0f);
        if (root_) {
            root_->dpiChanged();
            root_->invalidateLayout();
        }
        pacer_.requestFrame();
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && root_) {
            setCursor(root_->currentCursor());
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        if (!root_) {
            return 0;
        }
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        ::TrackMouseEvent(&tme);
        root_->dispatchMouseMove(toRoot(lp), modifiersNow());
        return 0;
    }

    case WM_MOUSELEAVE:
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
        // With capture held, clicks anywhere on screen land here; anything
        // outside the card closes the popup and is swallowed.
        if (!insideCard(lp)) {
            dismiss();
            return 0;
        }
        if (!root_) {
            return 0;
        }
        MouseButton button = MouseButton::Left;
        if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) {
            button = MouseButton::Right;
        } else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK) {
            button = MouseButton::Middle;
        }
        const bool dbl = (msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK);
        root_->dispatchMouseDown(toRoot(lp), button, modifiersNow(), dbl ? 2 : 1);
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        if (!root_) {
            return 0;
        }
        MouseButton button = MouseButton::Left;
        if (msg == WM_RBUTTONUP) {
            button = MouseButton::Right;
        } else if (msg == WM_MBUTTONUP) {
            button = MouseButton::Middle;
        }
        root_->dispatchMouseUp(toRoot(lp), button, modifiersNow());
        return 0;
    }

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        if (!root_) {
            return 0;
        }
        // Wheel messages carry screen coordinates.
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ::ScreenToClient(hwnd_, &pt);
        const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp));
        const bool precise = (static_cast<int>(delta) % WHEEL_DELTA) != 0;
        UINT lines = 3;
        if (!::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0)) {
            lines = 3;
        }
        const int linesPerNotch = (lines == WHEEL_PAGESCROLL) ? -1 : static_cast<int>(std::max<UINT>(1, lines));
        const Point root{scale_.toDip(static_cast<float>(pt.x)), scale_.toDip(static_cast<float>(pt.y))};
        if (msg == WM_MOUSEWHEEL) {
            root_->dispatchWheel(root, delta, 0.0f, precise, linesPerNotch, modifiersNow());
        } else {
            root_->dispatchWheel(root, 0.0f, delta, precise, linesPerNotch, modifiersNow());
        }
        return 0;
    }

    case WM_CAPTURECHANGED: {
        // Losing capture to anyone but ourselves while open means the user
        // went elsewhere (Alt+Tab, another app's capture): close.
        const HWND gaining = reinterpret_cast<HWND>(lp);
        if (visible_ && gaining != hwnd_) {
            dismiss();
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // Focus never lands here by design, but be complete.
        if (forwardKeyDown(static_cast<UINT>(wp), modifiersNow(), (lp & (1 << 30)) != 0)) {
            return 0;
        }
        break;

    case WM_CHAR:
        if (wp >= 0x20 && wp != 0x7F && forwardChar(static_cast<char32_t>(wp))) {
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == kOutsideClickTimer) {
            // Fallback for the no-capture case: a button held down while the
            // cursor is neither over us nor over the owner (which reports its
            // own clicks) closes the popup.
            if (!visible_) {
                ::KillTimer(hwnd_, kOutsideClickTimer);
                return 0;
            }
            const bool anyButton = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                                   (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                                   (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
            if (anyButton && ::GetCapture() != hwnd_) {
                POINT cursor{};
                RECT self{}, owner{};
                if (::GetCursorPos(&cursor) && ::GetWindowRect(hwnd_, &self)) {
                    const bool overSelf = ::PtInRect(&self, cursor) != FALSE;
                    const bool overOwner = owner_ != nullptr && ::IsWindow(owner_) && ::GetWindowRect(owner_, &owner) &&
                                           ::PtInRect(&owner, cursor) != FALSE;
                    if (!overSelf && !overOwner) {
                        dismiss();
                    }
                }
            }
            return 0;
        }
        break;

    case kMsgAcquireCapture:
        if (visible_ && ::GetCapture() != hwnd_) {
            ::SetCapture(hwnd_);
        }
        return 0;

    case kMsgPurgeRetired:
        if (root_) {
            if (auto* surface = static_cast<PopupSurface*>(root_->content())) {
                surface->purgeRetired();
            }
        }
        return 0;

    case WM_HH_POPUP_DISMISS:
        dismiss();
        return 0;

    case WM_DESTROY:
        if (visible_) {
            dismiss();
        }
        return 0;

    case WM_NCDESTROY:
        break;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace hh::ui
