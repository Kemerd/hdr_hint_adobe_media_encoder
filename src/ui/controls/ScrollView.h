// ---------------------------------------------------------------------------
// ScrollView.h - vertical scrolling with spring wheel scrolling, touchpad
// inertia, rubber-band overscroll and a fading overlay scrollbar.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <memory>

namespace hh::ui {

class ScrollView : public Widget {
public:
    ScrollView();

    Widget* setContent(std::unique_ptr<Widget> content);
    template <class T> T* setContentAs(std::unique_ptr<T> content) { T* raw = content.get(); setContent(std::move(content)); return raw; }
    [[nodiscard]] Widget* content() const noexcept { return content_; }

    [[nodiscard]] float offset() const noexcept { return offset_.value(); }
    [[nodiscard]] float maxOffset() const noexcept { return maxOffset_; }
    void scrollTo(float y, bool animated = true);
    void scrollBy(float dy, bool animated = true);
    void scrollToBottom(bool animated = true);
    /// Scrolls the minimum amount so @p w (a descendant) is fully visible.
    void scrollIntoView(Widget* w);
    /// Extra bottom padding so the last item can scroll above a footer/toast.
    void setContentInsets(const Insets& insets) { insets_ = insets; invalidateLayout(); }

    Size measure(const Constraints& c) override;
    void layout(const Rect& frame) override;
    void paint(Canvas& c) override;
    void paintOverlay(Canvas& c) override;
    Widget* hitTest(Point local) override;
    [[nodiscard]] bool interactive() const override { return true; }
    [[nodiscard]] bool wantsPointerAt(Point local) const override;
    bool onWheel(const WheelEvent& e) override;
    bool onMouseDown(const MouseEvent& e) override;
    bool onMouseUp(const MouseEvent& e) override;
    bool onMouseMove(const MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    void onFrame(double now) override;

private:
    [[nodiscard]] Rect thumbRect() const;
    [[nodiscard]] bool nearRightEdge(Point local) const;
    void clampAndSettle();
    void showBar();
    [[nodiscard]] float displayedOffset() const;

    Widget* content_ = nullptr;
    Animatable<float> offset_{0.0f};
    float target_ = 0.0f;
    float maxOffset_ = 0.0f;
    float velocity_ = 0.0f;           ///< dips per second (touchpad inertia)
    double lastInputAt_ = 0.0;
    double lastPreciseAt_ = 0.0;
    bool inertia_ = false;
    bool overscrolling_ = false;
    float overscroll_ = 0.0f;         ///< signed raw overscroll distance
    Animatable<float> barOpacity_{0.0f};
    TimerId barTimer_ = 0;
    bool barHover_ = false;
    bool draggingThumb_ = false;
    float dragStartY_ = 0.0f;
    float dragStartOffset_ = 0.0f;
    Insets insets_;
    float contentHeight_ = 0.0f;
};

} // namespace hh::ui
