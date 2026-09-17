// ---------------------------------------------------------------------------
// Card.h - rounded elevated container with animated insert/remove.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <functional>

namespace hh::ui {

class Card : public Widget {
public:
    Card();

    void setSelected(bool on);
    [[nodiscard]] bool selected() const noexcept { return selected_; }
    void setHoverable(bool on) { hoverable_ = on; }
    /// Red outline for failed states.
    void setDestructiveOutline(bool on) { destructive_ = on; invalidate(); }
    void setRadius(float r) { radius_ = r; invalidate(); }

    /// Grows from 0 height + fades in (call right after adding to the tree).
    void animateIn();
    /// Shrinks + fades out, then runs @p done (which usually removes the card).
    void animateOut(std::function<void()> done);
    [[nodiscard]] bool removing() const noexcept { return removing_; }

    std::function<void(const MouseEvent&)> onClick;
    std::function<void(Point rootPt)> onContextMenu;   ///< right-click

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    [[nodiscard]] bool interactive() const override { return true; }
    bool onMouseDown(const MouseEvent& e) override;
    bool onMouseUp(const MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;

private:
    bool selected_ = false;
    bool hoverable_ = true;
    bool destructive_ = false;
    bool removing_ = false;
    float radius_ = 10.0f;
    Animatable<float> heightScale_{1.0f};   ///< 0..1 multiplier applied in measure()
    Animatable<float> hover_{0.0f};
};

} // namespace hh::ui
