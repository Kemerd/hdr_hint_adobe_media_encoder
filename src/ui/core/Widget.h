// ---------------------------------------------------------------------------
// Widget.h - base class of the retained widget tree.
//
// Coordinates: frame() is parent-relative, bounds() is {0,0,w,h}; input is
// delivered in local coordinates. A widget never owns COM objects: it
// describes what to draw through Canvas.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/anim/Animatable.h"
#include "ui/core/InputEvents.h"
#include "ui/core/Layout.h"
#include "ui/gfx/Canvas.h"
#include "ui/gfx/Geometry.h"

#include <memory>
#include <string>
#include <vector>

namespace hh::ui {

class RootView;
class Timeline;
struct Theme;

class Widget {
public:
    Widget();
    virtual ~Widget();
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // ---- tree ---------------------------------------------------------------
    [[nodiscard]] Widget* parent() const noexcept { return parent_; }
    Widget* addChild(std::unique_ptr<Widget> child);
    /// Typed convenience: `auto* b = add(std::make_unique<Button>());`
    template <class T> T* add(std::unique_ptr<T> child) { T* raw = child.get(); addChild(std::move(child)); return raw; }
    Widget* insertChild(size_t index, std::unique_ptr<Widget> child);
    std::unique_ptr<Widget> removeChild(Widget* child);
    void clearChildren();
    [[nodiscard]] const std::vector<std::unique_ptr<Widget>>& children() const noexcept { return children_; }
    [[nodiscard]] RootView* root() const noexcept { return root_; }
    [[nodiscard]] bool attached() const noexcept { return root_ != nullptr; }

    // ---- layout -------------------------------------------------------------
    [[nodiscard]] LayoutParams& layoutParams() noexcept { return layoutParams_; }
    [[nodiscard]] const LayoutParams& layoutParams() const noexcept { return layoutParams_; }
    [[nodiscard]] StackParams& stack() noexcept { return stack_; }
    [[nodiscard]] const StackParams& stack() const noexcept { return stack_; }

    /// Preferred size within constraints (default: stack of children, or preferredSize_).
    virtual Size measure(const Constraints& c);
    /// Assigns the frame and lays out children (default: stack layout).
    virtual void layout(const Rect& frame);
    /// Hook after the frame changed (subclasses compute internal rects here).
    virtual void onLayout() {}

    [[nodiscard]] const Rect& frame() const noexcept { return frame_; }
    [[nodiscard]] Rect bounds() const noexcept { return {0, 0, frame_.w, frame_.h}; }
    [[nodiscard]] Point toRoot(Point local) const;
    [[nodiscard]] Point fromRoot(Point rootPt) const;
    [[nodiscard]] Rect frameInRoot() const;
    void setPreferredSize(Size s) { preferredSize_ = s; invalidateLayout(); }
    [[nodiscard]] Size preferredSize() const noexcept { return preferredSize_; }

    void invalidate();          ///< repaint
    void invalidateLayout();    ///< re-measure + re-layout before the next paint
    [[nodiscard]] bool needsLayout() const noexcept { return needsLayout_; }

    // ---- paint --------------------------------------------------------------
    /// Paints self, then children (clipped when stack().clipsChildren), then overlay.
    virtual void paint(Canvas& c);
    virtual void paintSelf(Canvas&) {}
    /// Painted after the children (focus rings, badges).
    virtual void paintOverlay(Canvas&) {}

    // ---- hit-testing / input ------------------------------------------------
    /// Deepest visible widget at @p local (children in reverse order, then self).
    virtual Widget* hitTest(Point local);
    virtual bool hitTestSelf(Point local) const;
    /// Widgets that only pass events through (containers) return false here.
    [[nodiscard]] virtual bool interactive() const { return false; }

    virtual bool onMouseDown(const MouseEvent&) { return false; }
    virtual bool onMouseUp(const MouseEvent&) { return false; }
    virtual bool onMouseMove(const MouseEvent&) { return false; }
    virtual void onMouseEnter() {}
    virtual void onMouseLeave() {}
    virtual bool onDoubleClick(const MouseEvent&) { return false; }
    virtual bool onWheel(const WheelEvent&) { return false; }
    virtual bool onKeyDown(const KeyEvent&) { return false; }
    virtual bool onKeyUp(const KeyEvent&) { return false; }
    virtual bool onChar(char32_t) { return false; }
    [[nodiscard]] virtual bool focusable() const { return false; }
    virtual void onFocusChanged(bool) {}
    [[nodiscard]] virtual CursorKind cursor() const { return CursorKind::Arrow; }
    [[nodiscard]] virtual std::wstring tooltip() const { return {}; }
    virtual void onThemeChanged() {}
    virtual void onDpiChanged() {}
    /// Called when the widget is attached to / detached from a root.
    virtual void onAttached() {}
    virtual void onDetached() {}
    /// Called once per frame while registered via setWantsFrameTicks(true).
    virtual void onFrame(double) {}

    // ---- state --------------------------------------------------------------
    [[nodiscard]] bool hovered() const noexcept { return hovered_; }
    [[nodiscard]] bool pressed() const noexcept { return pressed_; }
    [[nodiscard]] bool focused() const noexcept;
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    void setEnabled(bool on);
    void setVisible(bool on);
    [[nodiscard]] Animatable<float>& opacity() noexcept { return opacity_; }
    void setWantsFrameTicks(bool on);
    [[nodiscard]] bool wantsFrameTicks() const noexcept { return wantsTicks_; }

    /// Optional identifier for tests/screenshot driving.
    std::string id;

    // ---- helpers for subclasses --------------------------------------------
    [[nodiscard]] Timeline* timeline() const;
    [[nodiscard]] const Theme* theme() const;
    /// True when the pointer is over the widget while pressed (click completes on release).
    [[nodiscard]] bool pressInside() const noexcept { return pressInside_; }

protected:
    friend class RootView;
    void setHovered(bool on);
    void setPressed(bool on);
    void setPressInside(bool on) noexcept { pressInside_ = on; }
    void attachTo(RootView* root);
    void measureChildrenStack(const Constraints& c, Size& out);
    void layoutChildrenStack();

    Rect frame_;
    Size preferredSize_;
    LayoutParams layoutParams_;
    StackParams stack_;
    Animatable<float> opacity_{1.0f};

private:
    Widget* parent_ = nullptr;
    RootView* root_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
    bool hovered_ = false;
    bool pressed_ = false;
    bool pressInside_ = false;
    bool enabled_ = true;
    bool visible_ = true;
    bool needsLayout_ = true;
    bool wantsTicks_ = false;
    bool effectiveHorizontal_ = false;   ///< wrapIfNarrowerThan result of the last layout
};

/// Empty flexible spacer (flexGrow = 1).
class Spacer : public Widget {
public:
    explicit Spacer(float flex = 1.0f) { layoutParams().flexGrow = flex; }
    Size measure(const Constraints&) override { return {0, 0}; }
};

/// Fixed-size gap.
class Gap : public Widget {
public:
    explicit Gap(float size) : size_(size) {}
    Size measure(const Constraints&) override { return {size_, size_}; }
private:
    float size_;
};

} // namespace hh::ui
