// ---------------------------------------------------------------------------
// FocusManager.cpp - keyboard focus and Tab traversal over the widget tree.
//
// Traversal order is a depth-first walk of the visible, enabled widgets that
// report focusable(); it wraps at both ends. The focus ring is only drawn
// after keyboard navigation (keyboardMode), mirroring macOS behaviour.
// ---------------------------------------------------------------------------
#include "ui/core/FocusManager.h"

#include "core/Logger.h"
#include "ui/core/RootView.h"
#include "ui/core/Widget.h"

#include <algorithm>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Focus";

/// True when @p ancestor is @p w itself or one of its ancestors.
bool isSelfOrAncestor(const Widget* w, const Widget* ancestor) {
    for (const Widget* cur = w; cur; cur = cur->parent()) {
        if (cur == ancestor) { return true; }
    }
    return false;
}

} // namespace

/**
 * @brief Moves focus to @p w (nullptr clears) and notifies both widgets.
 *
 * A widget that lives in another tree (or none) cannot take focus; the
 * request is treated as "clear" so a stale pointer never becomes focused.
 */
void FocusManager::focus(Widget* w) {
    if (w && w->root() != &root_) {
        HH_LOG_WARN(kLog, L"focus request for a widget outside this root; clearing focus instead");
        w = nullptr;
    }
    if (w == focused_) { return; }

    // Tell the old widget first so a TextField can commit before the new one starts.
    Widget* old = focused_;
    focused_ = w;
    if (old) {
        old->onFocusChanged(false);
        old->invalidate();
    }
    if (w) {
        w->onFocusChanged(true);
        w->invalidate();
    }
}

/**
 * @brief Focuses the next focusable widget in depth-first order (wraps).
 */
void FocusManager::moveNext() {
    std::vector<Widget*> order;
    collect(&root_, order);
    if (order.empty()) { return; }

    // Start from the current widget when it is still in the ring, else from the first.
    size_t next = 0;
    auto it = std::find(order.begin(), order.end(), focused_);
    if (it != order.end()) {
        const size_t idx = static_cast<size_t>(it - order.begin());
        next = (idx + 1) % order.size();
    }
    focus(order[next]);
}

/**
 * @brief Focuses the previous focusable widget in depth-first order (wraps).
 */
void FocusManager::movePrev() {
    std::vector<Widget*> order;
    collect(&root_, order);
    if (order.empty()) { return; }

    // Without a current widget Shift+Tab lands on the last one, like every native toolkit.
    size_t prev = order.size() - 1;
    auto it = std::find(order.begin(), order.end(), focused_);
    if (it != order.end()) {
        const size_t idx = static_cast<size_t>(it - order.begin());
        prev = (idx == 0) ? order.size() - 1 : idx - 1;
    }
    focus(order[prev]);
}

/**
 * @brief Drops the focused pointer when its widget is no longer in this tree.
 */
void FocusManager::clearIfDetached() {
    if (!focused_) { return; }
    if (focused_->root() != &root_) {
        // The widget already left; it cannot be notified safely.
        focused_ = nullptr;
    }
}

/**
 * @brief Clears focus when @p w (or one of its ancestors) is removed or hidden.
 */
void FocusManager::widgetRemoved(Widget* w) {
    if (!w || !focused_) { return; }
    if (!isSelfOrAncestor(focused_, w)) { return; }

    // The widget is still alive at this point, so it gets a proper blur.
    Widget* old = focused_;
    focused_ = nullptr;
    old->onFocusChanged(false);
    old->invalidate();
}

/**
 * @brief Depth-first collection of the visible, enabled, focusable widgets.
 */
void FocusManager::collect(Widget* w, std::vector<Widget*>& out) const {
    if (!w || !w->visible() || !w->enabled()) { return; }
    if (w->focusable()) { out.push_back(w); }
    for (const auto& child : w->children()) {
        if (child) { collect(child.get(), out); }
    }
}

} // namespace hh::ui
