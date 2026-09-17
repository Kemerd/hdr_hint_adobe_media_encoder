// ---------------------------------------------------------------------------
// Animatable.h - a value that springs towards its target.
//
//   Animatable<float> x;  x.setOwner(this);  x.animateTo(1.0f, springs::snappy);
//   ... in paint: use x.value()
// ---------------------------------------------------------------------------
#pragma once

#include "ui/anim/Spring.h"
#include "ui/anim/Timeline.h"
#include "ui/gfx/Geometry.h"

#include <functional>

namespace hh::ui {

class Widget;

/// Component access for the animated types.
template <class T> struct AnimTraits;
template <> struct AnimTraits<float> {
    static constexpr int N = 1;
    static float get(const float& v, int) { return v; }
    static void set(float& v, int, float x) { v = x; }
};
template <> struct AnimTraits<Point> {
    static constexpr int N = 2;
    static float get(const Point& p, int i) { return i == 0 ? p.x : p.y; }
    static void set(Point& p, int i, float x) { if (i == 0) p.x = x; else p.y = x; }
};
template <> struct AnimTraits<Color> {
    static constexpr int N = 4;
    static float get(const Color& c, int i) { return i == 0 ? c.r : i == 1 ? c.g : i == 2 ? c.b : c.a; }
    static void set(Color& c, int i, float x) { if (i == 0) c.r = x; else if (i == 1) c.g = x; else if (i == 2) c.b = x; else c.a = x; }
};
template <> struct AnimTraits<Rect> {
    static constexpr int N = 4;
    static float get(const Rect& r, int i) { return i == 0 ? r.x : i == 1 ? r.y : i == 2 ? r.w : r.h; }
    static void set(Rect& r, int i, float x) { if (i == 0) r.x = x; else if (i == 1) r.y = x; else if (i == 2) r.w = x; else r.h = x; }
};

template <class T>
class Animatable final : public IAnimation {
public:
    Animatable() = default;
    explicit Animatable(T initial) : value_(initial), target_(initial) {}
    ~Animatable() override { detach(); }
    Animatable(const Animatable&) = delete;
    Animatable& operator=(const Animatable&) = delete;

    /// The widget to invalidate every tick, and whose timeline drives the springs.
    void setOwner(Widget* owner) { owner_ = owner; }

    [[nodiscard]] const T& value() const noexcept { return value_; }
    [[nodiscard]] const T& target() const noexcept { return target_; }
    [[nodiscard]] bool animating() const noexcept { return timeline_ != nullptr; }

    /// Jumps immediately (no animation).
    void set(const T& v);
    /// Springs towards @p target; @p done runs once settled.
    void animateTo(const T& target, const SpringParams& params = springs::snappy, std::function<void()> done = {});
    /// Stops in place.
    void stop();

    bool sample(double now) override;

private:
    Timeline* resolveTimeline();
    void invalidateOwner();
    void detach();

    T value_{};
    T target_{};
    SpringSolver solvers_[AnimTraits<T>::N];
    Widget* owner_ = nullptr;
    Timeline* timeline_ = nullptr;
    std::function<void()> done_;
};

} // namespace hh::ui
