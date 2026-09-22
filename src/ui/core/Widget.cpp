// ---------------------------------------------------------------------------
// Widget.cpp - the retained widget tree: ownership, stack layout, painting
// and hit-testing.
//
// Every container is a stack of its children along one axis. The two-pass
// measurement below first sizes the rigid children, then hands the flexible
// ones (flexGrow > 0) an equal-per-unit share of whatever is left, so a row
// of "flex 1" controls always ends up the same width no matter how long
// their labels are.
// ---------------------------------------------------------------------------
#include "ui/core/Widget.h"

#include "core/Logger.h"
#include "ui/core/RootView.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Widget";

/// Anything above this is treated as "unbounded" by the stack layout.
constexpr float kUnbounded = 1e8f;

/**
 * @brief One visible child as seen by the stack layout of its parent.
 */
struct ChildBox {
    Widget* widget = nullptr;
    Size size;                              ///< measured size without margins
    Insets margin;
    float flex = 0.0f;                      ///< > 0 when the child shares leftover space
    std::optional<CrossAlign> crossAlign;
    bool fixedMain = false;                 ///< a fixed LayoutParams size on the main axis
    bool fixedCross = false;                ///< a fixed LayoutParams size on the cross axis
};

/**
 * @brief Result of one measurement of a stack's children.
 */
struct StackMeasure {
    std::vector<ChildBox> boxes;
    float rigidMain = 0.0f;                 ///< sum of non-flex sizes + all margins + spacing
    float flexNeeded = 0.0f;                ///< main size the flex children need (see below)
    float totalFlex = 0.0f;
    float crossMax = 0.0f;                  ///< widest child (including margins)
};

/// Main-axis extent of a size for the given orientation.
float mainOf(const Size& s, bool horizontal) { return horizontal ? s.w : s.h; }
/// Cross-axis extent of a size for the given orientation.
float crossOf(const Size& s, bool horizontal) { return horizontal ? s.h : s.w; }
/// Main-axis extent of an inset pair for the given orientation.
float mainOf(const Insets& i, bool horizontal) { return horizontal ? i.horizontal() : i.vertical(); }
/// Cross-axis extent of an inset pair for the given orientation.
float crossOf(const Insets& i, bool horizontal) { return horizontal ? i.vertical() : i.horizontal(); }

/// Clamps a possibly-negative or NaN size component to something sane.
float sane(float v) { return (std::isfinite(v) && v > 0.0f) ? v : 0.0f; }

/**
 * @brief Builds the constraints a child is measured with.
 *
 * Fixed width/height become tight constraints so the child sees its final
 * size (a wrapping label needs the real width to compute its height), and
 * minimum sizes raise the lower bound.
 */
Constraints childConstraints(const LayoutParams& lp, float maxW, float maxH) {
    Constraints cc;
    cc.minW = 0.0f;
    cc.minH = 0.0f;
    cc.maxW = std::max(0.0f, maxW - lp.margin.horizontal());
    cc.maxH = std::max(0.0f, maxH - lp.margin.vertical());

    // Fixed sizes win over everything else.
    if (lp.width) { const float w = sane(*lp.width); cc.minW = w; cc.maxW = w; }
    if (lp.height) { const float h = sane(*lp.height); cc.minH = h; cc.maxH = h; }

    // Minimum sizes only raise the floor (and the ceiling if it was lower).
    if (lp.minWidth && !lp.width) { cc.minW = std::max(cc.minW, sane(*lp.minWidth)); cc.maxW = std::max(cc.maxW, cc.minW); }
    if (lp.minHeight && !lp.height) { cc.minH = std::max(cc.minH, sane(*lp.minHeight)); cc.maxH = std::max(cc.maxH, cc.minH); }
    return cc;
}

/**
 * @brief Measures every visible child of a stack in two passes.
 *
 * Pass 1 measures the rigid children with loose constraints. Pass 2 measures
 * the flexible children with their share of the remaining main-axis space as
 * the ceiling (when the container is bounded), which keeps them from asking
 * for more than they will be given. `flexNeeded` is the main-axis size the
 * container must have so that every flex child's share covers its measured
 * size; it is what an unbounded parent (a ScrollView) will see.
 */
void measureStack(const std::vector<std::unique_ptr<Widget>>& children, const StackParams& sp, bool horizontal,
                  float innerMaxW, float innerMaxH, StackMeasure& out) {
    out = StackMeasure{};
    innerMaxW = std::max(0.0f, innerMaxW);
    innerMaxH = std::max(0.0f, innerMaxH);
    const float innerMain = horizontal ? innerMaxW : innerMaxH;
    const bool boundedMain = innerMain < kUnbounded;

    // Collect the visible children first so the spacing count is right.
    for (const auto& up : children) {
        Widget* w = up.get();
        if (!w || !w->visible()) { continue; }
        ChildBox box;
        box.widget = w;
        const LayoutParams& lp = w->layoutParams();
        box.margin = lp.margin;
        box.crossAlign = lp.crossAlign;
        box.fixedMain = horizontal ? lp.width.has_value() : lp.height.has_value();
        box.fixedCross = horizontal ? lp.height.has_value() : lp.width.has_value();
        // A fixed main-axis size makes the child rigid even when it asks to grow.
        box.flex = (lp.flexGrow > 0.0f && !box.fixedMain) ? lp.flexGrow : 0.0f;
        out.boxes.push_back(box);
    }
    if (out.boxes.empty()) { return; }

    const float spacingTotal = sane(sp.spacing) * static_cast<float>(out.boxes.size() - 1);
    out.rigidMain = spacingTotal;

    // Pass 1: rigid children, plus the margins of everybody.
    for (ChildBox& box : out.boxes) {
        out.rigidMain += mainOf(box.margin, horizontal);
        if (box.flex > 0.0f) { out.totalFlex += box.flex; continue; }
        const Constraints cc = childConstraints(box.widget->layoutParams(), innerMaxW, innerMaxH);
        Size s = box.widget->measure(cc);
        s.w = sane(s.w);
        s.h = sane(s.h);
        box.size = s;
        out.rigidMain += mainOf(s, horizontal);
        out.crossMax = std::max(out.crossMax, crossOf(s, horizontal) + crossOf(box.margin, horizontal));
    }

    // Pass 2: flexible children get their proportional share as the ceiling.
    if (out.totalFlex > 0.0f) {
        const float remaining = boundedMain ? std::max(0.0f, innerMain - out.rigidMain) : 1e9f;
        for (ChildBox& box : out.boxes) {
            if (box.flex <= 0.0f) { continue; }
            const float share = boundedMain ? remaining * (box.flex / out.totalFlex) : 1e9f;
            const float maxW = horizontal ? share + box.margin.horizontal() : innerMaxW;
            const float maxH = horizontal ? innerMaxH : share + box.margin.vertical();
            const Constraints cc = childConstraints(box.widget->layoutParams(), maxW, maxH);
            Size s = box.widget->measure(cc);
            s.w = sane(s.w);
            s.h = sane(s.h);
            box.size = s;
            // The container needs at least measured * totalFlex / flex so this child's share covers it.
            const float needed = mainOf(s, horizontal) * (out.totalFlex / box.flex);
            out.flexNeeded = std::max(out.flexNeeded, needed);
            out.crossMax = std::max(out.crossMax, crossOf(s, horizontal) + crossOf(box.margin, horizontal));
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// construction / destruction
// ---------------------------------------------------------------------------

/**
 * @brief Wires the opacity animatable to this widget so it can repaint on every tick.
 */
Widget::Widget() {
    opacity_.setOwner(this);
}

/**
 * @brief Detaches the subtree from its root before the children are destroyed.
 *
 * Doing the detach first means every child sees a live root in onDetached()
 * and the root drops any hover/capture/focus pointers into the subtree while
 * the widgets still exist.
 */
Widget::~Widget() {
    // Let the root forget about us (and our descendants) while they are alive.
    if (root_ && root_ != this) {
        root_->widgetRemoved(this);
        if (wantsTicks_) { root_->registerTicker(this, false); }
    }

    // Detach the children (recursively) while the root still exists.
    for (auto& child : children_) {
        if (!child) { continue; }
        child->attachTo(nullptr);
        child->parent_ = nullptr;
    }
    root_ = nullptr;
    parent_ = nullptr;

    // Now destroy them; each child sees no root and stays quiet.
    children_.clear();
}

// ---------------------------------------------------------------------------
// tree
// ---------------------------------------------------------------------------

/**
 * @brief Appends a child (topmost for painting and hit-testing).
 */
Widget* Widget::addChild(std::unique_ptr<Widget> child) {
    return insertChild(children_.size(), std::move(child));
}

/**
 * @brief Inserts a child at @p index (clamped to the child count).
 */
Widget* Widget::insertChild(size_t index, std::unique_ptr<Widget> child) {
    if (!child) {
        HH_LOG_WARN(kLog, L"insertChild called with a null child");
        return nullptr;
    }
    if (child.get() == this) {
        HH_LOG_ERROR(kLog, L"insertChild: a widget cannot be its own child");
        return nullptr;
    }
    if (child->parent_) {
        HH_LOG_ERROR(kLog, L"insertChild: the child already has a parent");
        return nullptr;
    }

    Widget* raw = child.get();
    index = std::min(index, children_.size());
    raw->parent_ = this;
    children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));

    // Attaching registers tickers and fires onAttached() down the subtree.
    raw->attachTo(root_);
    invalidateLayout();
    return raw;
}

/**
 * @brief Removes @p child from the tree and returns ownership to the caller.
 */
std::unique_ptr<Widget> Widget::removeChild(Widget* child) {
    if (!child) { return nullptr; }
    auto it = std::find_if(children_.begin(), children_.end(),
                           [child](const std::unique_ptr<Widget>& up) { return up.get() == child; });
    if (it == children_.end()) {
        HH_LOG_WARN(kLog, L"removeChild: widget is not a child of this widget");
        return nullptr;
    }

    std::unique_ptr<Widget> owned = std::move(*it);
    children_.erase(it);

    // The root must drop its pointers while the parent chain is still intact.
    if (root_) { root_->widgetRemoved(child); }
    child->attachTo(nullptr);
    child->parent_ = nullptr;
    child->hovered_ = false;
    child->pressed_ = false;
    child->pressInside_ = false;

    invalidateLayout();
    return owned;
}

/**
 * @brief Removes and destroys every child.
 */
void Widget::clearChildren() {
    // Remove from the back so indices never shift under us.
    while (!children_.empty()) {
        Widget* last = children_.back().get();
        if (!last) { children_.pop_back(); continue; }
        std::unique_ptr<Widget> gone = removeChild(last);
        gone.reset();
    }
    invalidateLayout();
}

/**
 * @brief Moves the subtree to a (possibly null) root, registering tickers and
 *        firing the attach/detach hooks once per change.
 */
void Widget::attachTo(RootView* root) {
    RootView* old = root_;
    if (old != root) {
        // Leaving the old root: stop ticking there and let the widget clean up.
        if (old) {
            if (wantsTicks_) { old->registerTicker(this, false); }
            onDetached();
            // Any in-flight opacity spring belongs to the old timeline.
            opacity_.stop();
        }
        root_ = root;
        if (root_) {
            if (wantsTicks_) { root_->registerTicker(this, true); }
            onAttached();
        }
        needsLayout_ = true;
    }

    // Recurse so every descendant follows.
    for (auto& child : children_) {
        if (child) { child->attachTo(root); }
    }
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

/**
 * @brief Default measurement: a stack of the visible children, else preferredSize_.
 */
Size Widget::measure(const Constraints& c) {
    bool anyVisible = false;
    for (const auto& child : children_) {
        if (child && child->visible()) { anyVisible = true; break; }
    }

    Size out;
    if (anyVisible) {
        measureChildrenStack(c, out);
    } else {
        out = preferredSize_;
    }
    out.w = sane(out.w);
    out.h = sane(out.h);
    return c.constrain(out);
}

/**
 * @brief Measures the children as a stack and writes the natural size to @p out.
 *
 * The orientation decision (wrapIfNarrowerThan) is stored so layout() uses
 * exactly the same one.
 */
void Widget::measureChildrenStack(const Constraints& c, Size& out) {
    bool horizontal = stack_.axis == Axis::Horizontal;
    if (horizontal && stack_.wrapIfNarrowerThan > 0.0f && c.maxW < stack_.wrapIfNarrowerThan) {
        horizontal = false;
    }
    effectiveHorizontal_ = horizontal;

    const Insets& pad = stack_.padding;
    const float innerMaxW = std::max(0.0f, c.maxW - pad.horizontal());
    const float innerMaxH = std::max(0.0f, c.maxH - pad.vertical());

    StackMeasure m;
    measureStack(children_, stack_, horizontal, innerMaxW, innerMaxH, m);

    // Natural main size: rigid part plus what the flex children need.
    const float main = m.rigidMain + m.flexNeeded;
    const float cross = m.crossMax;
    if (horizontal) {
        out = {main + pad.horizontal(), cross + pad.vertical()};
    } else {
        out = {cross + pad.horizontal(), main + pad.vertical()};
    }
}

/**
 * @brief Stores the frame, arranges the children and notifies the subclass.
 */
void Widget::layout(const Rect& frame) {
    frame_ = frame;
    frame_.w = sane(frame_.w);
    frame_.h = sane(frame_.h);

    // The stack always re-runs: parents may hand us a new frame without a
    // fresh measure, and children may have changed underneath.
    layoutChildrenStack();
    needsLayout_ = false;
    onLayout();
}

/**
 * @brief Places the visible children along the stack axis inside the padding.
 *
 * Flexible children receive max(measured, share of leftover); the leftover
 * is the inner main size minus every rigid size, margin and spacing.
 */
void Widget::layoutChildrenStack() {
    bool anyVisible = false;
    for (const auto& child : children_) {
        if (child && child->visible()) { anyVisible = true; break; }
    }
    if (!anyVisible) { return; }

    // Use the orientation decided during measure (falls back to the axis when no wrap rule exists).
    bool horizontal = stack_.axis == Axis::Horizontal;
    if (horizontal && stack_.wrapIfNarrowerThan > 0.0f) { horizontal = effectiveHorizontal_; }

    const Insets& pad = stack_.padding;
    const float innerW = std::max(0.0f, frame_.w - pad.horizontal());
    const float innerH = std::max(0.0f, frame_.h - pad.vertical());
    const float innerMain = horizontal ? innerW : innerH;
    const float innerCross = horizontal ? innerH : innerW;

    // Re-measure against the real inner size so wrapping children get the final width.
    StackMeasure m;
    measureStack(children_, stack_, horizontal, innerW, innerH, m);
    if (m.boxes.empty()) { return; }

    // Hand out the leftover to the flexible children.
    const float leftover = std::max(0.0f, innerMain - m.rigidMain);
    float used = m.rigidMain;
    std::vector<float> mainSizes(m.boxes.size(), 0.0f);
    for (size_t i = 0; i < m.boxes.size(); ++i) {
        const ChildBox& box = m.boxes[i];
        float mainSize = mainOf(box.size, horizontal);
        if (box.flex > 0.0f && m.totalFlex > 0.0f) {
            const float share = leftover * (box.flex / m.totalFlex);
            mainSize = std::max(mainSize, share);
        }
        mainSizes[i] = mainSize;
        if (box.flex > 0.0f) { used += mainSize; }
    }

    // Whatever is still free is distributed by the justification rule.
    const float free = std::max(0.0f, innerMain - used);
    float cursor = 0.0f;
    float extraGap = 0.0f;
    switch (stack_.justify) {
    case Justify::Start: break;
    case Justify::Center: cursor = free * 0.5f; break;
    case Justify::End: cursor = free; break;
    case Justify::SpaceBetween:
        if (m.boxes.size() > 1) { extraGap = free / static_cast<float>(m.boxes.size() - 1); }
        break;
    }

    // Place every child, converting main/cross back to x/y.
    const float spacing = sane(stack_.spacing);
    for (size_t i = 0; i < m.boxes.size(); ++i) {
        const ChildBox& box = m.boxes[i];
        Widget* w = box.widget;
        if (!w) { continue; }

        const float marginLead = horizontal ? box.margin.left : box.margin.top;
        const float marginTrail = horizontal ? box.margin.right : box.margin.bottom;
        const float marginCrossLead = horizontal ? box.margin.top : box.margin.left;
        const float crossAvail = std::max(0.0f, innerCross - crossOf(box.margin, horizontal));

        // Cross-axis size and position from the alignment rule.
        const CrossAlign align = box.crossAlign.value_or(stack_.crossAlign);
        float crossSize = crossOf(box.size, horizontal);
        float crossPos = 0.0f;
        if (align == CrossAlign::Stretch && !box.fixedCross) {
            crossSize = crossAvail;
        } else if (align == CrossAlign::Center) {
            crossPos = (crossAvail - crossSize) * 0.5f;
        } else if (align == CrossAlign::End) {
            crossPos = crossAvail - crossSize;
        }

        cursor += marginLead;
        Rect fr;
        if (horizontal) {
            fr = {pad.left + cursor, pad.top + marginCrossLead + crossPos, mainSizes[i], crossSize};
        } else {
            fr = {pad.left + marginCrossLead + crossPos, pad.top + cursor, crossSize, mainSizes[i]};
        }
        w->layout(fr);
        cursor += mainSizes[i] + marginTrail + spacing + extraGap;
    }
}

/**
 * @brief Converts a local point to root coordinates by summing frame origins.
 */
Point Widget::toRoot(Point local) const {
    Point p = local;
    for (const Widget* w = this; w; w = w->parent_) {
        p = p + w->frame_.origin();
    }
    return p;
}

/**
 * @brief Converts a root point to this widget's local coordinates.
 */
Point Widget::fromRoot(Point rootPt) const {
    Point p = rootPt;
    for (const Widget* w = this; w; w = w->parent_) {
        p = p - w->frame_.origin();
    }
    return p;
}

/**
 * @brief The frame expressed in root coordinates.
 */
Rect Widget::frameInRoot() const {
    const Point origin = toRoot({0.0f, 0.0f});
    return {origin.x, origin.y, frame_.w, frame_.h};
}

/**
 * @brief Requests a repaint through the root (no-op while detached).
 */
void Widget::invalidate() {
    if (root_) { root_->requestFrame(); }
}

/**
 * @brief Marks this widget and every ancestor dirty, then requests a frame.
 *
 * The walk never stops early: a hidden child can carry a stale dirty flag
 * after the root was cleaned, so only a full walk guarantees the root is marked.
 */
void Widget::invalidateLayout() {
    for (Widget* w = this; w; w = w->parent_) {
        w->needsLayout_ = true;
    }
    invalidate();
}

// ---------------------------------------------------------------------------
// paint
// ---------------------------------------------------------------------------

/**
 * @brief Paints self, then the children in order, then the overlay, inside
 *        an opacity layer when faded and translated to the frame origin.
 */
void Widget::paint(Canvas& c) {
    if (!visible_) { return; }
    const float op = std::clamp(opacity_.value(), 0.0f, 1.0f);
    if (op <= 0.0f) { return; }

    const bool faded = op < 1.0f;
    if (faded) { c.pushOpacity(op); }
    c.pushTransform(Transform2D::translation(frame_.x, frame_.y));

    paintSelf(c);

    // Children paint in our local space; clip them when asked to.
    const bool clip = stack_.clipsChildren;
    if (clip) { c.pushClip(bounds()); }
    for (const auto& child : children_) {
        if (child && child->visible()) { child->paint(c); }
    }
    if (clip) { c.pop(); }

    paintOverlay(c);

    c.pop();                 // transform
    if (faded) { c.pop(); }  // opacity layer
}

// ---------------------------------------------------------------------------
// hit-testing
// ---------------------------------------------------------------------------

/**
 * @brief Deepest interactive widget under @p local: children last-added first, then self.
 */
Widget* Widget::hitTest(Point local) {
    if (!visible_ || !enabled_ || !bounds().contains(local)) { return nullptr; }

    // Topmost child (the last one added) wins.
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        Widget* child = it->get();
        if (!child || !child->visible()) { continue; }
        if (Widget* hit = child->hitTest(local - child->frame().origin())) { return hit; }
    }

    // Containers are transparent unless they declare themselves interactive.
    return (hitTestSelf(local) && interactive()) ? this : nullptr;
}

/**
 * @brief Default self test: inside the bounds.
 */
bool Widget::hitTestSelf(Point local) const {
    return bounds().contains(local);
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

/**
 * @brief True when the root's focus manager points at this widget.
 */
bool Widget::focused() const noexcept {
    return root_ && root_->focus().focused() == this;
}

/**
 * @brief Enables or disables input; a disabled widget loses hover/press.
 */
void Widget::setEnabled(bool on) {
    if (enabled_ == on) { return; }
    enabled_ = on;
    if (!on) {
        hovered_ = false;
        pressed_ = false;
        pressInside_ = false;
        if (root_ && root_ != this) { root_->widgetRemoved(this); }
    }
    invalidateLayout();
}

/**
 * @brief Shows or hides the widget; hiding drops any hover/capture/focus in the subtree.
 */
void Widget::setVisible(bool on) {
    if (visible_ == on) { return; }
    visible_ = on;
    if (!on) {
        hovered_ = false;
        pressed_ = false;
        pressInside_ = false;
        if (root_ && root_ != this) { root_->widgetRemoved(this); }
    }
    invalidateLayout();
}

/**
 * @brief Registers (or unregisters) for per-frame onFrame() calls.
 */
void Widget::setWantsFrameTicks(bool on) {
    if (wantsTicks_ == on) { return; }
    wantsTicks_ = on;
    if (root_) { root_->registerTicker(this, on); }
}

/**
 * @brief Stores the hover flag and repaints.
 */
void Widget::setHovered(bool on) {
    if (hovered_ == on) { return; }
    hovered_ = on;
    invalidate();
}

/**
 * @brief Stores the pressed flag and repaints.
 */
void Widget::setPressed(bool on) {
    if (pressed_ == on) { return; }
    pressed_ = on;
    invalidate();
}

// ---------------------------------------------------------------------------
// helpers for subclasses
// ---------------------------------------------------------------------------

/**
 * @brief The root's timeline, or null while detached.
 */
Timeline* Widget::timeline() const {
    return root_ ? &root_->timeline() : nullptr;
}

/**
 * @brief The root's current theme, or null while detached.
 */
const Theme* Widget::theme() const {
    return root_ ? &root_->theme() : nullptr;
}

} // namespace hh::ui
