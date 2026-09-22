// ---------------------------------------------------------------------------
// AppShell.cpp - top bar, tab switching, window chrome and global shortcuts.
// ---------------------------------------------------------------------------
#include "ui/screens/AppShell.h"

#include "core/Logger.h"
#include "ui/anim/Spring.h"
#include "ui/controls/Button.h"
#include "ui/controls/Divider.h"
#include "ui/controls/Label.h"
#include "ui/controls/SegmentedControl.h"
#include "ui/controls/StatusDot.h"
#include "ui/screens/GuideScreen.h"
#include "ui/screens/QueueFooter.h"
#include "ui/screens/QueueScreen.h"
#include "ui/screens/SettingsScreen.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace hh::ui {

namespace {

/// Below this window width the tabs shrink to their narrow width.
constexpr float kNarrowWidth = 380.0f;
/// Below this width the title is dropped.
constexpr float kTitleWidth = 460.0f;
/// Below this width the status label is dropped (the dot stays).
constexpr float kStatusLabelWidth = 620.0f;
/// Left inset of the top bar (and right inset when there are no window buttons).
constexpr float kTopBarPadding = 12.0f;
/// Gap between top bar items.
constexpr float kTopBarSpacing = 8.0f;
/// Tab widths for the wide / narrow layouts.
constexpr float kSegmentWide = 70.0f;
constexpr float kSegmentNarrow = 60.0f;
/// Windows caption button size.
constexpr float kWindowButtonW = 46.0f;
constexpr float kWindowButtonH = 32.0f;
/// The app mark glyph size.
constexpr float kAppMarkSize = 20.0f;
/// Minimum gap kept between the centred tabs and the groups either side.
constexpr float kTabGap = 8.0f;

/**
 * @brief The 20 dip accent-coloured app mark at the left of the top bar.
 *
 * Purely decorative: not interactive, so clicks on it drag the window.
 */
class AppMarkIcon final : public Widget {
public:
    /// Always 20x20 within the constraints.
    Size measure(const Constraints& c) override { return c.constrain({kAppMarkSize, kAppMarkSize}); }

    /// Draws the vector mark in the accent colour.
    void paintSelf(Canvas& c) override {
        c.drawIcon(IconId::AppMark, bounds(), c.theme().accent, 1.5f);
    }
};

/// Segment index for a tab.
int tabIndex(AppTab tab) noexcept {
    return static_cast<int>(tab);
}

/// Tab for a segment index (out-of-range -> Queue).
AppTab tabFromIndex(int index) noexcept {
    switch (index) {
    case 1: return AppTab::Settings;
    case 2: return AppTab::Guide;
    default: return AppTab::Queue;
    }
}

/// Status dot state from the link view.
LinkStatus linkStatus(const LinkView& lv) noexcept {
    if (lv.panelLinked) { return LinkStatus::Linked; }
    if (lv.logFound) { return LinkStatus::Watching; }
    return LinkStatus::Offline;
}

/// Human label next to the status dot.
const wchar_t* linkLabel(const LinkView& lv) noexcept {
    if (lv.panelLinked) { return L"AME linked"; }
    if (lv.logFound) { return L"Watching log"; }
    return L"Offline";
}

/// True for a real (non-spacer) visible child that occupies width.
bool occupiesSpace(const std::unique_ptr<Widget>& child) {
    if (!child || !child->visible() || child->frame().w <= 0.0f) { return false; }
    // Flexible spacers soak up the leftover; they are not content.
    return dynamic_cast<const Spacer*>(child.get()) == nullptr;
}

/// Right edge of the widest content child before @p stop (in the parent's space).
float visibleRightBefore(const Widget* parent, const Widget* stop) {
    float right = 0.0f;
    if (!parent) { return right; }
    for (const std::unique_ptr<Widget>& child : parent->children()) {
        if (child.get() == stop) { break; }
        if (occupiesSpace(child)) { right = std::max(right, child->frame().right()); }
    }
    return right;
}

/// Left edge of the leftmost content child after @p start (in the parent's space).
float visibleLeftAfter(const Widget* parent, const Widget* start, float fallback) {
    float left = fallback;
    if (!parent) { return left; }
    bool passed = false;
    for (const std::unique_ptr<Widget>& child : parent->children()) {
        if (child.get() == start) { passed = true; continue; }
        if (!passed || !occupiesSpace(child)) { continue; }
        left = std::min(left, child->frame().left());
    }
    return left;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/**
 * @brief Builds the top bar, the hairline and the screen host with all three
 *        screens (only the current one is visible).
 */
AppShell::AppShell(IQueueViewModel& queue, ISettingsViewModel& settings, ILinkViewModel& link, IGuideViewModel& guide)
    : queueVm_(queue), settingsVm_(settings), linkVm_(link), guideVm_(guide) {
    stack().axis = Axis::Vertical;
    stack().crossAlign = CrossAlign::Stretch;

    buildTopBar();
    add(std::make_unique<Divider>());

    // The screen host fills the rest of the window and clips the fading screen.
    screenHost_ = add(std::make_unique<Widget>());
    if (screenHost_) {
        screenHost_->layoutParams().flexGrow = 1.0f;
        screenHost_->stack().axis = Axis::Vertical;
        screenHost_->stack().crossAlign = CrossAlign::Stretch;
        screenHost_->stack().clipsChildren = true;

        // All three screens are built once. Hidden screens are skipped by
        // layout, paint and hit-testing, so only the current one costs anything.
        queue_ = screenHost_->add(std::make_unique<QueueScreen>(queueVm_));
        if (queue_) { queue_->layoutParams().flexGrow = 1.0f; }
        settings_ = screenHost_->add(std::make_unique<SettingsScreen>(settingsVm_));
        if (settings_) {
            settings_->layoutParams().flexGrow = 1.0f;
            settings_->setVisible(false);
        }
        guide_ = screenHost_->add(std::make_unique<GuideScreen>(guideVm_));
        if (guide_) {
            guide_->layoutParams().flexGrow = 1.0f;
            guide_->setVisible(false);
        }
    } else {
        HH_LOG_ERROR(L"AppShell", L"failed to create the screen host");
    }

    current_ = queue_;
    tab_ = AppTab::Queue;

    // The dock toggle in the queue footer flips the link view model.
    if (queue_ && queue_->footer()) {
        queue_->footer()->setDockToggle([this](bool) { linkVm_.toggleDock(); }, !docked_ && linkVm_.link().dockingSupported,
                                        linkVm_.link().docked);
    }
}

/**
 * @brief Unhooks the root and model callbacks that point at this object.
 */
AppShell::~AppShell() {
    linkVm_.onChanged = nullptr;
    if (RootView* r = root()) {
        if (r->chromeProvider() == this) { r->setChromeProvider(nullptr); }
        r->onShortcut = nullptr;
    }
}

/**
 * @brief Creates the 44 dip top bar: mark, title, tabs, status, window buttons.
 */
void AppShell::buildTopBar() {
    topBar_ = add(std::make_unique<Widget>());
    if (!topBar_) {
        HH_LOG_ERROR(L"AppShell", L"failed to create the top bar");
        return;
    }
    topBar_->layoutParams().height = kTopBarHeight;
    topBar_->stack().axis = Axis::Horizontal;
    topBar_->stack().spacing = kTopBarSpacing;
    topBar_->stack().crossAlign = CrossAlign::Center;
    // No right inset while the caption buttons sit flush with the window edge.
    topBar_->stack().padding = Insets{0.0f, docked_ ? kTopBarPadding : 0.0f, 0.0f, kTopBarPadding};

    // Left group: app mark + title.
    topBar_->add(std::make_unique<AppMarkIcon>());
    title_ = topBar_->add(std::make_unique<Label>(L"HDR Hint", typography::headline()));

    // Centre group: the tab switcher between two flexible spacers; onLayout()
    // nudges it to the true centre of the bar when there is room.
    topBar_->add(std::make_unique<Spacer>());
    tabs_ = topBar_->add(std::make_unique<SegmentedControl>(
        std::vector<std::wstring>{L"Queue", L"Settings", L"Guide"}, tabIndex(AppTab::Queue)));
    if (tabs_) {
        tabs_->setSegmentWidth(kSegmentWide);
        tabs_->onChanged = [this](int index) { switchTo(tabFromIndex(index), true); };
    }
    topBar_->add(std::make_unique<Spacer>());

    // Right group: link status + caption buttons.
    status_ = topBar_->add(std::make_unique<StatusDot>());

    windowButtons_ = topBar_->add(std::make_unique<Widget>());
    if (windowButtons_) {
        windowButtons_->stack().axis = Axis::Horizontal;
        windowButtons_->stack().spacing = 0.0f;
        windowButtons_->stack().crossAlign = CrossAlign::Start;
        // Caption buttons hug the top edge like every Windows 11 app.
        windowButtons_->layoutParams().crossAlign = CrossAlign::Start;
        windowButtons_->layoutParams().margin.left = kTopBarSpacing;

        minimizeBtn_ = windowButtons_->add(std::make_unique<IconButton>(IconId::Minimize, [this] {
            if (onMinimize) { onMinimize(); }
        }));
        if (minimizeBtn_) {
            minimizeBtn_->setSize(kWindowButtonW, kWindowButtonH);
            minimizeBtn_->setStrokeWidth(1.0f);
            minimizeBtn_->setTooltipText(L"Minimize");
        }
        maximizeBtn_ = windowButtons_->add(std::make_unique<IconButton>(IconId::Maximize, [this] {
            if (onMaximize) { onMaximize(); }
        }));
        if (maximizeBtn_) {
            maximizeBtn_->setSize(kWindowButtonW, kWindowButtonH);
            maximizeBtn_->setStrokeWidth(1.0f);
            maximizeBtn_->setTooltipText(L"Maximize");
        }
        closeBtn_ = windowButtons_->add(std::make_unique<IconButton>(IconId::Close, [this] {
            if (onClose) { onClose(); }
        }));
        if (closeBtn_) {
            closeBtn_->setSize(kWindowButtonW, kWindowButtonH);
            closeBtn_->setStrokeWidth(1.0f);
            closeBtn_->setDestructiveHover(true);
            closeBtn_->setTooltipText(L"Close");
        }
        windowButtons_->setVisible(!docked_);
    }
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

/**
 * @brief Public tab switch: moves the pill and swaps the screen.
 */
void AppShell::setTab(AppTab tab, bool animated) {
    if (tabs_ && tabs_->selected() != tabIndex(tab)) { tabs_->setSelected(tabIndex(tab), animated); }
    switchTo(tab, animated);
}

/**
 * @brief Hides the current screen and fades the requested one in.
 */
void AppShell::switchTo(AppTab tab, bool animated) {
    Widget* next = nullptr;
    switch (tab) {
    case AppTab::Queue: next = queue_; break;
    case AppTab::Settings: next = settings_; break;
    case AppTab::Guide: next = guide_; break;
    }
    if (!next) {
        HH_LOG_WARN(L"AppShell", L"switchTo({}) : screen not available", static_cast<int>(tab));
        return;
    }
    if (next == current_) {
        tab_ = tab;
        if (!next->visible()) { next->setVisible(true); }
        return;
    }

    // Park the outgoing screen fully opaque so it is ready for its next turn.
    if (current_) {
        current_->setVisible(false);
        current_->opacity().set(1.0f);
    }

    // Keyboard focus must not linger inside a hidden screen.
    if (RootView* r = root()) { r->focus().focus(nullptr); }

    current_ = next;
    tab_ = tab;
    next->setVisible(true);
    if (animated && timeline() && !timeline()->reducedMotion()) {
        next->opacity().set(0.0f);
        next->opacity().animateTo(1.0f, springs::snappy);
    } else {
        next->opacity().set(1.0f);
    }
    HH_LOG_DEBUG(L"AppShell", L"tab -> {}", static_cast<int>(tab));
    invalidateLayout();
}

// ---------------------------------------------------------------------------
// Window state
// ---------------------------------------------------------------------------

/**
 * @brief Docked: no caption buttons, symmetric insets, no dock toggle.
 */
void AppShell::setDockedLayout(bool docked) {
    docked_ = docked;
    if (windowButtons_) { windowButtons_->setVisible(!docked && !nativeControls_); }
    if (topBar_) { topBar_->stack().padding.right = (docked || nativeControls_) ? kTopBarPadding : 0.0f; }
    // The footer's dock switch only makes sense while floating.
    if (queue_ && queue_->footer()) {
        queue_->footer()->setDockToggle([this](bool) { linkVm_.toggleDock(); }, !docked && linkVm_.link().dockingSupported,
                                        linkVm_.link().docked);
    }
    invalidateLayout();
}

/**
 * @brief Native caption buttons: hide ours and make room for the system's.
 */
void AppShell::setNativeWindowControls(bool native, float leadingInset) {
    nativeControls_ = native;
    leadingInset_ = native ? std::max(0.0f, leadingInset) : 0.0f;
    if (windowButtons_) { windowButtons_->setVisible(!docked_ && !native); }
    if (topBar_) {
        topBar_->stack().padding.left = kTopBarPadding + leadingInset_;
        topBar_->stack().padding.right = (docked_ || native) ? kTopBarPadding : 0.0f;
    }
    invalidateLayout();
}

/**
 * @brief Swaps the maximize glyph for restore.
 */
void AppShell::setMaximized(bool maximized) {
    maximized_ = maximized;
    if (!maximizeBtn_) { return; }
    maximizeBtn_->setIcon(maximized ? IconId::Restore : IconId::Maximize);
    maximizeBtn_->setTooltipText(maximized ? L"Restore" : L"Maximize");
}

/**
 * @brief Dims the title while the window is inactive (macOS style).
 */
void AppShell::setWindowActive(bool active) {
    active_ = active;
    if (title_) { title_->setTone(active ? LabelTone::Primary : LabelTone::Secondary); }
    invalidate();
}

/**
 * @brief Shows a toast through the root overlay.
 */
void AppShell::showToast(ToastSpec spec) {
    RootView* r = root();
    if (!r) {
        HH_LOG_WARN(L"AppShell", L"toast dropped (detached): {}", spec.text);
        return;
    }
    r->overlay().toasts().show(std::move(spec));
}

/**
 * @brief Re-reads the link view model into the status dot and dock toggle.
 */
void AppShell::refreshLink() {
    const LinkView lv = linkVm_.link();
    // The status label is dropped on compact layouts (the dot always stays).
    const bool narrow = statusLabelHidden_;
    if (status_) {
        const std::wstring label = linkLabel(lv);
        status_->setStatus(linkStatus(lv));
        status_->setLabel(narrow ? std::wstring() : label);
        status_->setTooltipText(lv.tooltip.empty() ? label : lv.tooltip);
    }
    if (queue_ && queue_->footer()) {
        queue_->footer()->setDockToggle([this](bool) { linkVm_.toggleDock(); }, !docked_ && lv.dockingSupported, lv.docked);
    }
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------

/**
 * @brief Non-client hit-test for the custom title bar.
 */
ChromeHit AppShell::chromeHitTest(Point rootPt) const {
    // A docked panel has no caption to drag and no buttons.
    if (docked_ || !topBar_ || !topBar_->visible()) { return ChromeHit::None; }
    const Point local = topBar_->fromRoot(rootPt);
    if (!topBar_->bounds().contains(local)) { return ChromeHit::None; }

    // The maximize button is reported so Snap Layouts can hover it.
    const bool buttonsShown = windowButtons_ && windowButtons_->visible();
    if (buttonsShown && maximizeBtn_ && maximizeBtn_->visible()) {
        if (maximizeBtn_->bounds().contains(maximizeBtn_->fromRoot(rootPt))) { return ChromeHit::Maximize; }
    }
    // Interactive parts stay client area so the widgets receive the click.
    if (buttonsShown && windowButtons_->bounds().contains(windowButtons_->fromRoot(rootPt))) { return ChromeHit::None; }
    if (tabs_ && tabs_->visible() && tabs_->bounds().contains(tabs_->fromRoot(rootPt))) { return ChromeHit::None; }
    if (status_ && status_->visible() && status_->bounds().contains(status_->fromRoot(rootPt))) { return ChromeHit::None; }
    return ChromeHit::Caption;
}

/**
 * @brief Root-space rect of the maximize button (empty when hidden).
 */
Rect AppShell::chromeMaximizeRect() const {
    if (docked_ || !windowButtons_ || !windowButtons_->visible() || !maximizeBtn_ || !maximizeBtn_->visible()) {
        return Rect{};
    }
    return maximizeBtn_->frameInRoot();
}

/**
 * @brief Hover feedback driven by WM_NCMOUSEMOVE over HTMAXBUTTON.
 */
void AppShell::setChromeMaximizeHover(bool hover) {
    if (!maximizeBtn_) { return; }
    if (hover) { maximizeBtn_->onMouseEnter(); } else { maximizeBtn_->onMouseLeave(); }
}

// ---------------------------------------------------------------------------
// Layout / paint
// ---------------------------------------------------------------------------

/**
 * @brief Applies the narrow layout before measuring the stack.
 */
Size AppShell::measure(const Constraints& c) {
    // Three breakpoints, applied from the outside in: the status label goes
    // first, then the title, then the tabs shrink. Each change is applied only
    // when it differs from the current state so measuring stays cheap.
    const bool bounded = c.hasBoundedWidth();
    const bool narrowTabs = bounded && c.maxW < kNarrowWidth;
    const bool hideTitle = bounded && c.maxW < kTitleWidth;
    const bool hideLabel = bounded && c.maxW < kStatusLabelWidth;

    if (title_ && title_->visible() == hideTitle) {
        title_->setVisible(!hideTitle);
    }
    if (tabs_) {
        const float want = narrowTabs ? kSegmentNarrow : kSegmentWide;
        if (tabsNarrow_ != narrowTabs) {
            tabsNarrow_ = narrowTabs;
            tabs_->setSegmentWidth(want);
        }
    }
    if (status_ && statusLabelHidden_ != hideLabel) {
        statusLabelHidden_ = hideLabel;
        status_->setLabel(hideLabel ? std::wstring() : std::wstring(linkLabel(linkVm_.link())));
    }
    return Widget::measure(c);
}

/**
 * @brief Centres the tab switcher in the bar when the side groups allow it.
 */
void AppShell::onLayout() {
    if (!topBar_ || !tabs_ || !tabs_->visible()) { return; }
    const Rect bar = topBar_->bounds();
    const Rect tabs = tabs_->frame();
    if (bar.w <= 0.0f || tabs.w <= 0.0f) { return; }

    // Room between the left group (mark/title) and the right group (status/buttons).
    const float minX = visibleRightBefore(topBar_, tabs_) + kTabGap;
    const float maxX = visibleLeftAfter(topBar_, tabs_, bar.w - topBar_->stack().padding.right) - kTabGap - tabs.w;
    if (maxX < minX) { return; }

    const float centred = std::clamp((bar.w - tabs.w) * 0.5f, minX, maxX);
    if (std::fabs(centred - tabs.x) > 0.5f) {
        tabs_->layout(Rect{centred, tabs.y, tabs.w, tabs.h});
    }
}

/**
 * @brief The window clears the background; nothing to paint here.
 */
void AppShell::paintSelf(Canvas&) {}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

/**
 * @brief Direct key handling (the shell is never focused, but stays correct).
 */
bool AppShell::onKeyDown(const KeyEvent& e) {
    return handleShortcut(e);
}

/**
 * @brief Global shortcuts, invoked after the focused widget declined the key.
 */
bool AppShell::handleShortcut(const KeyEvent& e) {
    const bool ctrl = e.mods.ctrl;
    const bool shift = e.mods.shift;
    const bool alt = e.mods.alt;

    // Ctrl+1/2/3 and Ctrl+, switch tabs; Ctrl+R runs a job.
    if (ctrl && !shift && !alt) {
        switch (e.vk) {
        case '1': setTab(AppTab::Queue); return true;
        case '2': setTab(AppTab::Settings); return true;
        case '3': setTab(AppTab::Guide); return true;
        case VK_OEM_COMMA: setTab(AppTab::Settings); return true;
        case 'R':
            if (queue_ && tab_ == AppTab::Queue) { return queue_->onKeyDown(e); }
            return false;
        default: break;
        }
    }

    // Ctrl+Shift+A toggles auto-process, Ctrl+Shift+C copies the guide summary.
    if (ctrl && shift && !alt) {
        if (e.vk == 'A') {
            const bool on = !queueVm_.autoProcess();
            queueVm_.setAutoProcess(on);
            if (queue_) { queue_->refresh(); }
            showToast(ToastSpec{on ? L"Auto-process on" : L"Auto-process off", ToastTone::Info, {}, {}, 2.5});
            return true;
        }
        if (e.vk == 'C') {
            guideVm_.copySummary();
            showToast(ToastSpec{L"Summary copied", ToastTone::Success, {}, {}, 3.0});
            return true;
        }
    }

    if (!ctrl && !shift && !alt) {
        // F1 opens the guide.
        if (e.vk == VK_F1) {
            setTab(AppTab::Guide);
            return true;
        }
        // Escape: dismiss the newest toast, else clear the queue selection.
        if (e.vk == VK_ESCAPE) {
            if (RootView* r = root()) {
                if (r->overlay().toasts().dismissTop()) { return true; }
            }
            if (queue_ && tab_ == AppTab::Queue) { return queue_->onKeyDown(e); }
            return false;
        }
        // Delete removes the selected job.
        if (e.vk == VK_DELETE) {
            if (queue_ && tab_ == AppTab::Queue) { return queue_->onKeyDown(e); }
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Registers as chrome provider and shortcut handler, binds the link model.
 */
void AppShell::onAttached() {
    RootView* r = root();
    if (r) {
        r->setChromeProvider(this);
        r->onShortcut = [this](const KeyEvent& e) { return handleShortcut(e); };
    } else {
        HH_LOG_WARN(L"AppShell", L"onAttached without a root");
    }
    linkVm_.onChanged = [this] { refreshLink(); };
    refreshLink();
    // The active-window tint may have been set before we had a root.
    setWindowActive(r ? r->windowActive() : active_);
}

/**
 * @brief Children re-read their own tokens; the shell only repaints.
 */
void AppShell::onThemeChanged() {
    invalidate();
}

} // namespace hh::ui
