// ---------------------------------------------------------------------------
// FocusManager.h - keyboard focus and Tab traversal.
// ---------------------------------------------------------------------------
#pragma once

#include <vector>

namespace hh::ui {

class Widget;
class RootView;

class FocusManager {
public:
    explicit FocusManager(RootView& root) : root_(root) {}

    /// Moves focus (nullptr clears). Notifies both widgets.
    void focus(Widget* w);
    [[nodiscard]] Widget* focused() const noexcept { return focused_; }
    /// Depth-first next/previous focusable visible enabled widget (wraps).
    void moveNext();
    void movePrev();
    /// Focus ring is only drawn after keyboard navigation (like macOS).
    void setKeyboardMode(bool on) noexcept { keyboardMode_ = on; }
    [[nodiscard]] bool keyboardMode() const noexcept { return keyboardMode_; }
    /// Clears focus when the focused widget left the tree.
    void clearIfDetached();
    /// Called by RootView when a widget is removed.
    void widgetRemoved(Widget* w);

private:
    void collect(Widget* w, std::vector<Widget*>& out) const;

    RootView& root_;
    Widget* focused_ = nullptr;
    bool keyboardMode_ = false;
};

} // namespace hh::ui
