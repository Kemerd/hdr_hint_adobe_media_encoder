// ---------------------------------------------------------------------------
// RootView.cpp - the top of a widget tree: layout, painting, animation
// ticking and input routing for one HWND.
//
// The root has exactly two children stacked on top of each other: the
// content (child 0) and the OverlayHost (child 1). Both fill the window, so
// this is the only z-stack the toolkit needs.
// ---------------------------------------------------------------------------
#include "ui/core/RootView.h"

#include "core/Logger.h"
#include "ui/controls/Toast.h"

#include <algorithm>
#include <vector>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"RootView";

/// Hover delay before a tooltip appears (seconds).
constexpr double kTooltipDelay = 0.6;
/// The pointer glyph is roughly this tall; tooltips anchor below it.
constexpr float kCursorHeight = 16.0f;

/// True when @p ancestor is @p w itself or one of its ancestors.
bool isSelfOrAncestor(const Widget* w, const Widget* ancestor) {
    for (const Widget* cur = w; cur; cur = cur->parent()) {
        if (cur == ancestor) { return true; }
    }
    return false;
}

/// True when every widget from @p w up to the root is visible.
bool visibleChain(const Widget* w) {
    for (const Widget* cur = w; cur; cur = cur->parent()) {
        if (!cur->visible()) { return false; }
    }
    return w != nullptr;
}

/**
 * @brief Deepest visible widget whose bounds contain the point, ignoring
 *        interactive(): wheel events walk up from here until someone scrolls.
 */
Widget* deepestAt(Widget* w, Point local) {
    if (!w || !w->visible() || !w->enabled() || !w->bounds().contains(local)) { return nullptr; }
    const auto& kids = w->children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        Widget* child = it->get();
        if (!child) { continue; }
        if (Widget* hit = deepestAt(child, local - child->frame().origin())) { return hit; }
    }
    return w;
}

/// Recursively notifies a subtree of a theme change.
void notifyTheme(Widget* w) {
    if (!w) { return; }
    w->onThemeChanged();
    for (const auto& child : w->children()) { notifyTheme(child.get()); }
}

/// Recursively notifies a subtree of a DPI change.
void notifyDpi(Widget* w) {
    if (!w) { return; }
    w->onDpiChanged();
    for (const auto& child : w->children()) { notifyDpi(child.get()); }
}

} // namespace

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

/**
 * @brief Builds the root with an empty content placeholder and the overlay.
 */
RootView::RootView(IWindowServices& window, ThemeManager& themes)
    : window_(window), themes_(themes), focus_(*this) {
    // The root is its own root: children attach to it through addChild.
    attachTo(this);
    timeline_.setReducedMotion(ThemeManager::readReducedMotion());

    content_ = add(std::make_unique<Widget>());
    overlay_ = add(std::make_unique<OverlayHost>());
    if (!content_ || !overlay_) {
        HH_LOG_ERROR(kLog, L"failed to create the content placeholder or overlay");
    }
}

/**
 * @brief Tears the tree down while the timeline and focus manager still exist.
 */
RootView::~RootView() {
    cancelTooltipTimer();
    if (overlay_) { overlay_->dismissPopup(); }

    // Nothing may point into the tree while it is destroyed.
    hovered_ = nullptr;
    captured_ = nullptr;
    pressedWidget_ = nullptr;
    tooltipWidget_ = nullptr;
    focus_.focus(nullptr);

    // Children detach and destroy here, with timeline_/focus_ alive for their Animatables.
    clearChildren();
    content_ = nullptr;
    overlay_ = nullptr;
    tickers_.clear();
}

// ---------------------------------------------------------------------------
// composition
// ---------------------------------------------------------------------------

/**
 * @brief Replaces child 0 (the content) and keeps the overlay on top.
 */
void RootView::setContent(std::unique_ptr<Widget> content) {
    if (!content) {
        HH_LOG_WARN(kLog, L"setContent called with null content; keeping the current content");
        return;
    }
    // Remove the old content first so hover/focus pointers into it are cleared.
    if (content_) {
        std::unique_ptr<Widget> old = removeChild(content_);
        content_ = nullptr;
        old.reset();
    }
    content_ = insertChild(0, std::move(content));
    invalidateLayout();
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

/**
 * @brief Measures and lays out both children to the full window size when dirty.
 */
void RootView::layoutIfNeeded(Size size) {
    size.w = std::max(0.0f, size.w);
    size.h = std::max(0.0f, size.h);
    const bool sizeChanged = size.w != lastSize_.w || size.h != lastSize_.h;
    if (!needsLayout_ && !sizeChanged) { return; }

    lastSize_ = size;
    frame_ = {0.0f, 0.0f, size.w, size.h};
    const Rect full = bounds();

    // Every child of the root is a full-size layer (content below, overlay above).
    for (const auto& child : children_) {
        if (!child || !child->visible()) { continue; }
        child->measure(Constraints::tight(size));
        child->layout(full);
    }
    needsLayout_ = false;

    // Widgets may have moved under a resting pointer; refresh hover without moving it.
    if (mouseInside_ && !captured_) {
        updateHover(lastMouseRoot_, Modifiers{});
        window_.setCursor(currentCursor());
    }
}

/**
 * @brief Paints the whole tree.
 */
void RootView::paintAll(Canvas& c) {
    paint(c);
}

/**
 * @brief Samples springs and timers, then ticks the per-frame widgets.
 * @return true when something animated and a repaint is needed
 */
bool RootView::tickAnimations() {
    bool animated = timeline_.tick();

    // Tick a snapshot: onFrame may register/unregister tickers.
    if (!tickers_.empty()) {
        const double now = timeline_.now();
        const std::vector<Widget*> snapshot = tickers_;
        for (Widget* w : snapshot) {
            if (!w) { continue; }
            if (std::find(tickers_.begin(), tickers_.end(), w) == tickers_.end()) { continue; }
            if (!visibleChain(w)) { continue; }
            w->onFrame(now);
            animated = true;
        }
    }
    return animated;
}

/**
 * @brief Asks the window for another frame.
 */
void RootView::requestFrame() {
    window_.requestFrame();
}

/**
 * @brief Adds or removes a widget from the per-frame tick list.
 */
void RootView::registerTicker(Widget* w, bool on) {
    if (!w) { return; }
    auto it = std::find(tickers_.begin(), tickers_.end(), w);
    if (on) {
        if (it == tickers_.end()) { tickers_.push_back(w); }
        requestFrame();
    } else if (it != tickers_.end()) {
        tickers_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// hover / hit-testing
// ---------------------------------------------------------------------------

/**
 * @brief The interactive widget under a root point (null when none).
 */
Widget* RootView::hitAtRoot(Point rootPt) {
    return hitTest(rootPt);
}

/**
 * @brief Recomputes the hovered widget and delivers enter/leave along both chains.
 */
void RootView::updateHover(Point rootPt, Modifiers) {
    lastMouseRoot_ = rootPt;
    mouseInside_ = true;

    Widget* target = captured_ ? captured_ : hitAtRoot(rootPt);
    if (target == hovered_) { return; }

    Widget* previous = hovered_;
    hovered_ = target;
    deliverEnterLeave(previous, target);

    // The tooltip belongs to the widget that was hovered; a new one restarts the clock.
    cancelTooltipTimer();
    if (overlay_ && overlay_->tooltipVisible()) { overlay_->hideTooltip(); }
    if (target && !captured_ && overlay_ && !overlay_->hasPopup() && !target->tooltip().empty()) {
        startTooltipTimer(target, rootPt);
    }
}

/**
 * @brief Leaves the widgets unique to @p from's chain, enters those unique to @p to's.
 */
void RootView::deliverEnterLeave(Widget* from, Widget* to) {
    // Leave from the deepest widget upwards, skipping shared ancestors and the root.
    for (Widget* w = from; w && w != this; w = w->parent()) {
        if (isSelfOrAncestor(to, w)) { continue; }
        w->setHovered(false);
        w->onMouseLeave();
    }

    // Enter from the outermost new widget down to the target.
    std::vector<Widget*> entering;
    for (Widget* w = to; w && w != this; w = w->parent()) {
        if (isSelfOrAncestor(from, w)) { continue; }
        entering.push_back(w);
    }
    for (auto it = entering.rbegin(); it != entering.rend(); ++it) {
        (*it)->setHovered(true);
        (*it)->onMouseEnter();
    }
}

// ---------------------------------------------------------------------------
// mouse
// ---------------------------------------------------------------------------

/**
 * @brief Routes a pointer move: hover diff, then onMouseMove to the capture or hover target.
 */
void RootView::dispatchMouseMove(Point rootPt, Modifiers mods) {
    updateHover(rootPt, mods);

    Widget* target = captured_ ? captured_ : hovered_;
    if (target) {
        MouseEvent ev;
        ev.pos = target->fromRoot(rootPt);
        ev.rootPos = rootPt;
        ev.mods = mods;

        // While pressed, keep the "pointer is still over me" flag current for click-on-release.
        if (pressedWidget_ == target) {
            const bool inside = target->hitTestSelf(ev.pos);
            if (inside != target->pressInside()) {
                target->setPressInside(inside);
                target->invalidate();
            }
        }
        target->onMouseMove(ev);
    }
    window_.setCursor(currentCursor());
}

/**
 * @brief Routes a button press: dismisses popups, moves focus, presses the hit widget.
 *
 * A click while a popup is open only closes the popup. clickCount 2 tries
 * onDoubleClick first and only falls back to onMouseDown when it is refused.
 */
void RootView::dispatchMouseDown(Point rootPt, MouseButton button, Modifiers mods, int clickCount) {
    cancelTooltipTimer();
    if (overlay_ && overlay_->tooltipVisible()) { overlay_->hideTooltip(); }

    // Click-outside closes the popup and is swallowed.
    if (overlay_ && overlay_->hasPopup()) {
        overlay_->dismissPopup();
        return;
    }

    updateHover(rootPt, mods);
    Widget* w = hitAtRoot(rootPt);

    // Focus goes to the hit widget or its nearest focusable ancestor; empty space clears it.
    Widget* target = w;
    while (target && !target->focusable()) { target = target->parent(); }
    focus_.focus(target);
    focus_.setKeyboardMode(false);
    if (Widget* f = focus_.focused()) { f->invalidate(); }   // the ring goes away

    if (!w) { return; }
    MouseEvent ev;
    ev.pos = w->fromRoot(rootPt);
    ev.rootPos = rootPt;
    ev.button = button;
    ev.mods = mods;
    ev.clickCount = std::max(1, clickCount);

    pressedWidget_ = w;
    w->setPressed(true);
    w->setPressInside(true);

    bool handled = false;
    if (ev.clickCount >= 2) { handled = w->onDoubleClick(ev); }
    if (!handled && pressedWidget_ == w) { handled = w->onMouseDown(ev); }

    // The handler may have removed the widget; only capture when it is still pressed.
    if (handled && pressedWidget_ == w) {
        captured_ = w;
        window_.captureMouse(true);
    }
    window_.setCursor(currentCursor());
}

/**
 * @brief Routes a button release to the captured (or pressed) widget, then refreshes hover.
 *
 * Capture is released before the event is delivered so a WM_CAPTURECHANGED
 * arriving synchronously cannot cancel the click that is about to happen.
 */
void RootView::dispatchMouseUp(Point rootPt, MouseButton button, Modifiers mods) {
    Widget* target = captured_ ? captured_ : pressedWidget_;
    const bool hadCapture = captured_ != nullptr;
    captured_ = nullptr;
    if (hadCapture) { window_.captureMouse(false); }

    if (target) {
        MouseEvent ev;
        ev.pos = target->fromRoot(rootPt);
        ev.rootPos = rootPt;
        ev.button = button;
        ev.mods = mods;
        target->setPressInside(target->hitTestSelf(ev.pos));

        // Deliver the release while the press state is still set: every
        // clickable widget decides "was pressed and released inside" from
        // pressed(), so clearing it first would turn each click into a no-op.
        // pressedWidget_ stays armed during the call; a widget destroyed by
        // its own handler reaches widgetRemoved(), which nulls it, so a dead
        // pointer is never touched afterwards.
        pressedWidget_ = target;
        target->onMouseUp(ev);
        if (pressedWidget_ == target) {
            target->setPressed(false);
            target->setPressInside(false);
        }
    }
    pressedWidget_ = nullptr;

    // Re-run the hover diff: the release may have changed what is under the pointer.
    // A release outside the window (the pointer left during the drag) clears hover instead.
    if (mouseInside_) {
        updateHover(rootPt, mods);
    } else if (Widget* previous = hovered_) {
        hovered_ = nullptr;
        deliverEnterLeave(previous, nullptr);
    }
    window_.setCursor(currentCursor());
}

/**
 * @brief The pointer left the client area: clear hover unless a drag is in progress.
 */
void RootView::dispatchMouseLeave() {
    mouseInside_ = false;
    cancelTooltipTimer();
    if (overlay_ && overlay_->tooltipVisible()) { overlay_->hideTooltip(); }
    if (captured_) { return; }

    Widget* previous = hovered_;
    hovered_ = nullptr;
    deliverEnterLeave(previous, nullptr);
    window_.setCursor(CursorKind::Arrow);
}

/**
 * @brief Delivers a wheel event to the deepest widget under the point that accepts it.
 */
void RootView::dispatchWheel(Point rootPt, float delta, float deltaX, bool precise, int linesPerNotch, Modifiers mods) {
    Widget* w = deepestAt(this, rootPt);
    for (; w && w != this; w = w->parent()) {
        WheelEvent ev;
        ev.pos = w->fromRoot(rootPt);
        ev.rootPos = rootPt;
        ev.delta = delta;
        ev.deltaX = deltaX;
        ev.precise = precise;
        ev.linesPerNotch = linesPerNotch;
        ev.mods = mods;
        if (w->onWheel(ev)) { break; }
    }
}

// ---------------------------------------------------------------------------
// keyboard
// ---------------------------------------------------------------------------

/**
 * @brief Key routing: Escape (popup, toasts) -> focused widget -> Tab traversal -> shortcuts.
 * @return true when something consumed the key
 */
bool RootView::dispatchKeyDown(UINT vk, Modifiers mods, bool repeat) {
    cancelTooltipTimer();
    if (overlay_ && overlay_->tooltipVisible()) { overlay_->hideTooltip(); }

    // Escape peels the transient layers off first.
    if (vk == VK_ESCAPE && overlay_) {
        if (overlay_->hasPopup()) { overlay_->dismissPopup(); return true; }
        if (overlay_->toasts().hasToasts() && overlay_->toasts().dismissTop()) { return true; }
    }

    KeyEvent ev;
    ev.vk = vk;
    ev.mods = mods;
    ev.repeat = repeat;

    // The focused widget gets first refusal.
    Widget* f = focus_.focused();
    if (f && f->onKeyDown(ev)) { return true; }

    // Escape with a focused widget that did not want it: blur (like clicking empty space).
    if (vk == VK_ESCAPE && focus_.focused()) {
        focus_.focus(nullptr);
        return true;
    }

    // Tab / Shift+Tab traverse and switch the focus ring on.
    if (vk == VK_TAB && !mods.ctrl && !mods.alt) {
        focus_.setKeyboardMode(true);
        if (mods.shift) { focus_.movePrev(); } else { focus_.moveNext(); }
        if (Widget* nf = focus_.focused()) { nf->invalidate(); }
        return true;
    }

    // Global shortcuts registered by the app shell.
    if (onShortcut && onShortcut(ev)) { return true; }
    return false;
}

/**
 * @brief Key-up goes to the focused widget only.
 */
bool RootView::dispatchKeyUp(UINT vk, Modifiers mods) {
    Widget* f = focus_.focused();
    if (!f) { return false; }
    KeyEvent ev;
    ev.vk = vk;
    ev.mods = mods;
    return f->onKeyUp(ev);
}

/**
 * @brief Character input goes to the focused widget only.
 */
bool RootView::dispatchChar(char32_t ch) {
    Widget* f = focus_.focused();
    return f && f->onChar(ch);
}

// ---------------------------------------------------------------------------
// interaction state
// ---------------------------------------------------------------------------

/**
 * @brief Clears press/capture/hover, closes popups and hides the tooltip
 *        (WM_CANCELMODE, WM_CAPTURECHANGED, deactivation).
 */
void RootView::cancelInteraction() {
    cancelTooltipTimer();
    if (overlay_ && overlay_->tooltipVisible()) { overlay_->hideTooltip(); }

    // Release the press without delivering a click.
    if (Widget* p = pressedWidget_) {
        pressedWidget_ = nullptr;
        p->setPressInside(false);
        p->setPressed(false);
    }
    const bool hadCapture = captured_ != nullptr;
    captured_ = nullptr;
    if (hadCapture) { window_.captureMouse(false); }

    // Drop hover so nothing stays highlighted under a menu or another window.
    if (Widget* h = hovered_) {
        hovered_ = nullptr;
        deliverEnterLeave(h, nullptr);
    }
    mouseInside_ = false;

    if (overlay_) { overlay_->dismissPopup(); }
    invalidate();
}

/**
 * @brief Records window activation; inactive windows show no tooltip.
 */
void RootView::setWindowActive(bool active) {
    if (windowActive_ == active) { return; }
    windowActive_ = active;
    if (!active) {
        cancelTooltipTimer();
        if (overlay_ && overlay_->tooltipVisible()) { overlay_->hideTooltip(); }
    }
    invalidate();
}

/**
 * @brief The cursor the window should show right now.
 */
CursorKind RootView::currentCursor() const {
    if (captured_) { return captured_->cursor(); }
    if (hovered_) { return hovered_->cursor(); }
    return CursorKind::Arrow;
}

/**
 * @brief Forwards WM_NCHITTEST to the chrome provider (None without one).
 */
ChromeHit RootView::chromeHitTest(Point rootPt) const {
    return chrome_ ? chrome_->chromeHitTest(rootPt) : ChromeHit::None;
}

/**
 * @brief Asks the widget under @p rootPt whether it needs the pointer there.
 *
 * Used by the window's non-client hit test: a control drawn inside the resize
 * band (the overlay scrollbar) would otherwise be unclickable, because the
 * frame swallows those points before any client message is generated.
 */
bool RootView::wantsPointerAt(Point rootPt) {
    for (Widget* w = hitAtRoot(rootPt); w != nullptr; w = w->parent()) {
        if (w->wantsPointerAt(w->fromRoot(rootPt))) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// notifications
// ---------------------------------------------------------------------------

/**
 * @brief Tells every widget the theme changed, then the app callback, then repaints.
 */
void RootView::themeChanged() {
    timeline_.setReducedMotion(ThemeManager::readReducedMotion());
    notifyTheme(this);
    invalidate();
    if (onThemeChanged) { onThemeChanged(); }
}

/**
 * @brief Tells every widget the DPI changed and schedules a full layout.
 */
void RootView::dpiChanged() {
    notifyDpi(this);
    invalidateLayout();
}

/**
 * @brief Drops every pointer into @p w's subtree (called before it leaves the tree).
 */
void RootView::widgetRemoved(Widget* w) {
    if (!w) { return; }

    // Capture and press simply end; no release event is delivered.
    if (captured_ && isSelfOrAncestor(captured_, w)) {
        captured_ = nullptr;
        window_.captureMouse(false);
    }
    if (pressedWidget_ && isSelfOrAncestor(pressedWidget_, w)) {
        pressedWidget_ = nullptr;
    }

    // Hover retreats to the surviving parent; the next move re-evaluates it.
    if (hovered_ && isSelfOrAncestor(hovered_, w)) {
        Widget* previous = hovered_;
        Widget* survivor = w->parent();
        if (survivor == this) { survivor = nullptr; }
        hovered_ = survivor;
        deliverEnterLeave(previous, survivor);
    }

    if (tooltipWidget_ && isSelfOrAncestor(tooltipWidget_, w)) {
        cancelTooltipTimer();
        if (overlay_ && overlay_->tooltipVisible()) { overlay_->hideTooltip(); }
    }

    // Tickers inside the subtree stop ticking.
    tickers_.erase(std::remove_if(tickers_.begin(), tickers_.end(),
                                  [w](Widget* t) { return !t || isSelfOrAncestor(t, w); }),
                   tickers_.end());

    focus_.widgetRemoved(w);
}

// ---------------------------------------------------------------------------
// tooltip timer
// ---------------------------------------------------------------------------

/**
 * @brief Arms the 600 ms hover timer for @p w; the tooltip shows if the pointer is still there.
 */
void RootView::startTooltipTimer(Widget* w, Point) {
    cancelTooltipTimer();
    if (!w) { return; }
    tooltipWidget_ = w;
    tooltipTimer_ = timeline_.addTimerIn(kTooltipDelay, [this]() {
        tooltipTimer_ = 0;
        Widget* target = tooltipWidget_;
        tooltipWidget_ = nullptr;

        // Still hovering the same widget, window active, no popup, nothing pressed.
        if (!target || !overlay_ || target != hovered_ || captured_ || pressedWidget_) { return; }
        if (!windowActive_ || overlay_->hasPopup()) { return; }
        const std::wstring text = target->tooltip();
        if (text.empty()) { return; }

        // Anchor below the pointer glyph so the bubble never hides under the cursor.
        overlay_->showTooltip(text, {lastMouseRoot_.x, lastMouseRoot_.y + kCursorHeight});
        requestFrame();
    });
}

/**
 * @brief Cancels a pending tooltip timer (no-op when none is armed).
 */
void RootView::cancelTooltipTimer() {
    if (tooltipTimer_ != 0) {
        timeline_.cancelTimer(tooltipTimer_);
        tooltipTimer_ = 0;
    }
    tooltipWidget_ = nullptr;
}

} // namespace hh::ui
