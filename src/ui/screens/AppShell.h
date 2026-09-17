// ---------------------------------------------------------------------------
// AppShell.h - top bar + tab switcher + screens + footer; the window content.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/controls/Toast.h"
#include "ui/core/RootView.h"
#include "ui/core/Widget.h"
#include "ui/screens/ViewModels.h"

#include <functional>
#include <memory>
#include <string>

namespace hh::ui {

class SegmentedControl;
class StatusDot;
class IconButton;
class QueueScreen;
class SettingsScreen;
class GuideScreen;
class Label;

enum class AppTab { Queue = 0, Settings = 1, Guide = 2 };

class AppShell : public Widget, public IChromeProvider {
public:
    AppShell(IQueueViewModel& queue, ISettingsViewModel& settings, ILinkViewModel& link, IGuideViewModel& guide);
    ~AppShell() override;

    void setTab(AppTab tab, bool animated = true);
    [[nodiscard]] AppTab tab() const noexcept { return tab_; }
    /// Docked layout: no window buttons, opaque background, tighter paddings.
    void setDockedLayout(bool docked);
    [[nodiscard]] bool dockedLayout() const noexcept { return docked_; }
    /// Maximized state affects the maximize/restore glyph.
    void setMaximized(bool maximized);
    /// Window-active dims the title like macOS.
    void setWindowActive(bool active);
    void showToast(ToastSpec spec);
    /// Re-reads the link view model.
    void refreshLink();

    /// Window button callbacks (floating mode).
    std::function<void()> onMinimize;
    std::function<void()> onMaximize;
    std::function<void()> onClose;

    // IChromeProvider
    [[nodiscard]] ChromeHit chromeHitTest(Point rootPt) const override;
    [[nodiscard]] Rect chromeMaximizeRect() const override;
    void setChromeMaximizeHover(bool hover) override;

    Size measure(const Constraints& c) override;
    void onLayout() override;
    void paintSelf(Canvas& c) override;
    bool onKeyDown(const KeyEvent& e) override;
    void onAttached() override;
    void onThemeChanged() override;

    static constexpr float kTopBarHeight = 44.0f;

private:
    void buildTopBar();
    void switchTo(AppTab tab, bool animated);
    bool handleShortcut(const KeyEvent& e);

    IQueueViewModel& queueVm_;
    ISettingsViewModel& settingsVm_;
    ILinkViewModel& linkVm_;
    IGuideViewModel& guideVm_;

    Widget* topBar_ = nullptr;
    Label* title_ = nullptr;
    SegmentedControl* tabs_ = nullptr;
    StatusDot* status_ = nullptr;
    Widget* windowButtons_ = nullptr;
    IconButton* minimizeBtn_ = nullptr;
    IconButton* maximizeBtn_ = nullptr;
    IconButton* closeBtn_ = nullptr;
    Widget* screenHost_ = nullptr;
    QueueScreen* queue_ = nullptr;
    SettingsScreen* settings_ = nullptr;
    GuideScreen* guide_ = nullptr;
    Widget* current_ = nullptr;
    AppTab tab_ = AppTab::Queue;
    bool docked_ = false;
    bool maximized_ = false;
    bool active_ = true;
    bool tabsNarrow_ = false;
    bool statusLabelHidden_ = false;
};

} // namespace hh::ui
