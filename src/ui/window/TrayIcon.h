// ---------------------------------------------------------------------------
// TrayIcon.h - notification-area icon with a context menu and balloons.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <functional>
#include <string>
#include <vector>

namespace hh::ui {

class TrayIcon {
public:
    struct MenuItem {
        int id = 0;                 ///< 0 = separator
        std::wstring text;
        bool checked = false;
        bool enabled = true;
    };

    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    /// Adds the icon; @p callbackMessage is posted to @p hwnd on interaction.
    bool add(HWND hwnd, HICON icon, const std::wstring& tooltip, UINT callbackMessage);
    void remove();
    [[nodiscard]] bool added() const noexcept { return added_; }
    void setTooltip(const std::wstring& tooltip);
    void setIcon(HICON icon);
    /// Balloon notification (NIIF_USER with the app icon).
    void showBalloon(const std::wstring& title, const std::wstring& text);

    /// Menu shown on right-click; built fresh each time from @p provider.
    void setMenuProvider(std::function<std::vector<MenuItem>()> provider) { menuProvider_ = std::move(provider); }
    /// Handles the callback message. Returns true when consumed.
    bool handleMessage(WPARAM wParam, LPARAM lParam);
    /// Re-adds the icon after Explorer restarted (TaskbarCreated).
    void onTaskbarCreated();
    [[nodiscard]] static UINT taskbarCreatedMessage();

    std::function<void()> onLeftClick;
    std::function<void()> onDoubleClick;
    std::function<void(int id)> onCommand;

private:
    void showMenu();

    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    UINT callback_ = 0;
    std::wstring tooltip_;
    bool added_ = false;
    std::function<std::vector<MenuItem>()> menuProvider_;
};

} // namespace hh::ui
