// ---------------------------------------------------------------------------
// MacStatusItem.h - the menu bar extra and user notifications.
//
// The macOS counterpart of the Win32 TrayIcon: a template-image status item
// whose menu is rebuilt from a provider every time it opens, and banner
// notifications (Notification Center) in place of tray balloons.
// ---------------------------------------------------------------------------
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hh::ui {

class MacStatusItem {
public:
    struct MenuItem {
        int id = 0;                 ///< 0 = separator
        std::wstring text;
        bool checked = false;
        bool enabled = true;
    };

    MacStatusItem();
    ~MacStatusItem();
    MacStatusItem(const MacStatusItem&) = delete;
    MacStatusItem& operator=(const MacStatusItem&) = delete;

    /// Puts the item in the menu bar.
    bool add(const std::wstring& tooltip);
    void remove();
    [[nodiscard]] bool added() const noexcept;

    /// Menu shown on click; built fresh each time from @p provider.
    void setMenuProvider(std::function<std::vector<MenuItem>()> provider);

    /**
     * @brief Posts a banner through Notification Center.
     *
     * Asks for permission the first time; silently does nothing when the
     * user declined or the binary runs outside an app bundle.
     */
    void notify(const std::wstring& title, const std::wstring& text);

    std::function<void(int id)> onCommand;
    /// The user clicked one of our banners.
    std::function<void()> onNotificationClicked;

private:
    struct Objc;
    std::unique_ptr<Objc> objc_;
    std::shared_ptr<std::function<std::vector<MenuItem>()>> provider_;
};

} // namespace hh::ui
