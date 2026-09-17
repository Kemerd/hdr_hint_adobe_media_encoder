// ---------------------------------------------------------------------------
// TrayIcon.cpp - notification-area icon with a context menu and balloons.
//
// The icon speaks NOTIFYICON_VERSION_4, so the shell packs the mouse event
// into LOWORD(lParam) and the icon id into HIWORD(lParam); wParam carries the
// anchor point. The context menu is a classic HMENU on purpose: it is drawn
// by the shell outside our window, so it does not need the custom toolkit.
// ---------------------------------------------------------------------------
#include "ui/window/TrayIcon.h"

#include "core/Expected.h"
#include "core/Logger.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>

namespace hh::ui {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"TrayIcon";

/// The single icon id this process registers (one tray icon per app).
constexpr UINT kIconId = 1;

/**
 * @brief Copies a wide string into a fixed NOTIFYICONDATAW buffer, always
 *        NUL-terminated and never overflowing.
 */
template <size_t N>
void copyBounded(wchar_t (&dest)[N], const std::wstring& src) {
    // Leave room for the terminator; the shell truncates silently otherwise.
    const size_t count = std::min(src.size(), N - 1);
    if (count > 0) {
        std::memcpy(dest, src.data(), count * sizeof(wchar_t));
    }
    dest[count] = L'\0';
}

/**
 * @brief Builds the base NOTIFYICONDATAW every Shell_NotifyIcon call starts
 *        from (window, id and the version-4 message contract).
 */
NOTIFYICONDATAW makeBase(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kIconId;
    nid.uVersion = NOTIFYICON_VERSION_4;
    return nid;
}

} // namespace

/**
 * @brief Removes the icon on destruction so no ghost icon lingers in the
 *        tray until the user hovers it.
 */
TrayIcon::~TrayIcon() {
    remove();
}

/**
 * @brief Adds the icon to the notification area and opts into version 4
 *        messaging.
 */
bool TrayIcon::add(HWND hwnd, HICON icon, const std::wstring& tooltip, UINT callbackMessage) {
    // A dead window would make the shell drop every callback on the floor.
    if (hwnd == nullptr || !::IsWindow(hwnd)) {
        HH_LOG_WARN(kLog, L"add: invalid window handle");
        return false;
    }
    if (callbackMessage == 0) {
        HH_LOG_WARN(kLog, L"add: callback message must not be 0");
        return false;
    }

    // Replacing an existing icon: remove the old registration first so the
    // shell does not end up with two entries for the same id.
    if (added_) {
        remove();
    }

    hwnd_ = hwnd;
    icon_ = icon;
    callback_ = callbackMessage;
    tooltip_ = tooltip;

    // NIF_SHOWTIP keeps the standard tooltip even though we use version 4.
    NOTIFYICONDATAW nid = makeBase(hwnd_);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = callback_;
    nid.hIcon = icon_;
    copyBounded(nid.szTip, tooltip_);

    if (!::Shell_NotifyIconW(NIM_ADD, &nid)) {
        HH_LOG_WARN(kLog, L"Shell_NotifyIcon(NIM_ADD) failed: {}", Error::fromLastError(L"NIM_ADD").toString());
        return false;
    }

    // Version 4 must be requested after NIM_ADD; without it the shell falls
    // back to the legacy message packing and WM_CONTEXTMENU never arrives.
    NOTIFYICONDATAW ver = makeBase(hwnd_);
    if (!::Shell_NotifyIconW(NIM_SETVERSION, &ver)) {
        HH_LOG_WARN(kLog, L"Shell_NotifyIcon(NIM_SETVERSION) failed: {}", Error::fromLastError(L"NIM_SETVERSION").toString());
        // The icon still exists; keep it and live with legacy messages.
    }

    added_ = true;
    HH_LOG_DEBUG(kLog, L"tray icon added");
    return true;
}

/**
 * @brief Deletes the icon from the notification area (no-op when absent).
 */
void TrayIcon::remove() {
    if (!added_) {
        return;
    }
    added_ = false;

    // Even if the window is already gone the shell accepts the delete by id.
    NOTIFYICONDATAW nid = makeBase(hwnd_);
    if (!::Shell_NotifyIconW(NIM_DELETE, &nid)) {
        HH_LOG_DEBUG(kLog, L"Shell_NotifyIcon(NIM_DELETE) failed: {}", Error::fromLastError(L"NIM_DELETE").toString());
    }
}

/**
 * @brief Updates the hover tooltip text.
 */
void TrayIcon::setTooltip(const std::wstring& tooltip) {
    tooltip_ = tooltip;
    if (!added_ || hwnd_ == nullptr) {
        return;
    }

    NOTIFYICONDATAW nid = makeBase(hwnd_);
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    copyBounded(nid.szTip, tooltip_);
    if (!::Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        HH_LOG_DEBUG(kLog, L"setTooltip: NIM_MODIFY failed: {}", Error::fromLastError(L"NIM_MODIFY").toString());
    }
}

/**
 * @brief Swaps the icon image (status badges etc.).
 */
void TrayIcon::setIcon(HICON icon) {
    icon_ = icon;
    if (!added_ || hwnd_ == nullptr) {
        return;
    }

    NOTIFYICONDATAW nid = makeBase(hwnd_);
    nid.uFlags = NIF_ICON;
    nid.hIcon = icon_;
    if (!::Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        HH_LOG_DEBUG(kLog, L"setIcon: NIM_MODIFY failed: {}", Error::fromLastError(L"NIM_MODIFY").toString());
    }
}

/**
 * @brief Shows a balloon / toast notification with the app icon.
 */
void TrayIcon::showBalloon(const std::wstring& title, const std::wstring& text) {
    if (!added_ || hwnd_ == nullptr) {
        HH_LOG_DEBUG(kLog, L"showBalloon skipped: icon not added");
        return;
    }

    // NIIF_USER + hBalloonIcon shows our own icon in the notification
    // instead of the generic info glyph (honoured from version 4 onwards).
    NOTIFYICONDATAW nid = makeBase(hwnd_);
    nid.uFlags = NIF_INFO | NIF_SHOWTIP;
    nid.dwInfoFlags = NIIF_USER | NIIF_RESPECT_QUIET_TIME;
    nid.hBalloonIcon = icon_;
    if (icon_ == nullptr) {
        // No icon to show: fall back to the standard info glyph.
        nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    }
    copyBounded(nid.szInfoTitle, title);
    copyBounded(nid.szInfo, text);

    if (!::Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        HH_LOG_DEBUG(kLog, L"showBalloon: NIM_MODIFY failed: {}", Error::fromLastError(L"NIM_MODIFY").toString());
    }
}

/**
 * @brief Dispatches a version-4 callback message.
 *
 * Left click and keyboard selection map to onLeftClick, a double-click to
 * onDoubleClick, and the context-menu gestures (right click, Shift+F10,
 * the menu key) open the provider's menu.
 */
bool TrayIcon::handleMessage(WPARAM wParam, LPARAM lParam) {
    // wParam holds the anchor point in version 4; it is only used by the menu
    // path, which reads the live cursor position instead.
    (void)wParam;

    // Only our icon id is expected; anything else is not ours to consume.
    const UINT iconId = HIWORD(lParam);
    if (iconId != kIconId && iconId != 0) {
        return false;
    }

    const UINT event = LOWORD(lParam);
    switch (event) {
    case WM_LBUTTONUP:
    case NIN_KEYSELECT:
        // Plain activation: the app usually shows / raises its window.
        if (onLeftClick) {
            onLeftClick();
        }
        return true;

    case WM_LBUTTONDBLCLK:
        if (onDoubleClick) {
            onDoubleClick();
        }
        return true;

    case WM_CONTEXTMENU:
    case WM_RBUTTONUP:
        // Version 4 sends WM_CONTEXTMENU; the legacy fallback sends the
        // raw button message. Both mean "show the menu".
        showMenu();
        return true;

    case NIN_BALLOONUSERCLICK:
        // Clicking the balloon behaves like a left click on the icon.
        if (onLeftClick) {
            onLeftClick();
        }
        return true;

    default:
        // NIN_SELECT (follows WM_LBUTTONUP), hover moves, balloon timeouts:
        // consumed but no action, so the caller does not treat them as
        // unhandled messages.
        return true;
    }
}

/**
 * @brief Re-registers the icon after Explorer restarted.
 */
void TrayIcon::onTaskbarCreated() {
    // Only icons that were previously added come back; the shell forgot them.
    if (!added_ || hwnd_ == nullptr) {
        return;
    }
    HH_LOG_INFO(kLog, L"taskbar re-created, re-adding tray icon");

    // add() would call remove() first, which is pointless (and logs a
    // failure) because the shell already dropped the icon.
    added_ = false;
    add(hwnd_, icon_, tooltip_, callback_);
}

/**
 * @brief The broadcast message Explorer sends when the taskbar is created.
 */
UINT TrayIcon::taskbarCreatedMessage() {
    // RegisterWindowMessage returns the same id for the same string per
    // session; cache it so repeated calls are free.
    static const UINT message = ::RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

/**
 * @brief Builds the context menu from the provider and tracks it.
 *
 * The SetForegroundWindow / WM_NULL dance is the documented requirement for
 * tray menus: without it the menu does not close when the user clicks away.
 */
void TrayIcon::showMenu() {
    if (hwnd_ == nullptr || !::IsWindow(hwnd_)) {
        return;
    }

    // No provider means no menu; a right click is then simply ignored.
    std::vector<MenuItem> items;
    if (menuProvider_) {
        items = menuProvider_();
    }
    if (items.empty()) {
        return;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        HH_LOG_WARN(kLog, L"CreatePopupMenu failed: {}", Error::fromLastError(L"CreatePopupMenu").toString());
        return;
    }

    // Append every item; id 0 is a separator by contract.
    for (const MenuItem& item : items) {
        if (item.id == 0) {
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_STRING;
        if (item.checked) {
            flags |= MF_CHECKED;
        }
        if (!item.enabled) {
            flags |= MF_GRAYED;
        }
        ::AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.id), item.text.c_str());
    }

    // The menu must belong to the foreground window, otherwise it stays open
    // after the user clicks elsewhere.
    ::SetForegroundWindow(hwnd_);

    POINT pt{};
    if (!::GetCursorPos(&pt)) {
        pt = POINT{0, 0};
    }

    // TPM_RETURNCMD hands the chosen id back instead of posting WM_COMMAND.
    const UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_LEFTALIGN;
    const BOOL chosen = ::TrackPopupMenuEx(menu, flags, pt.x, pt.y, hwnd_, nullptr);

    // WM_NULL nudges the window so the menu really goes away (MS KB 135788).
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
    ::DestroyMenu(menu);

    const int id = static_cast<int>(chosen);
    if (id > 0 && onCommand) {
        onCommand(id);
    }
}

} // namespace hh::ui
