// ---------------------------------------------------------------------------
// Geometry.h - value types for the UI (all coordinates in dips).
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <d2d1_1.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hh::ui {

struct Point {
    float x = 0.0f;
    float y = 0.0f;
    Point() = default;
    Point(float px, float py) : x(px), y(py) {}
    Point operator+(const Point& o) const { return {x + o.x, y + o.y}; }
    Point operator-(const Point& o) const { return {x - o.x, y - o.y}; }
    Point operator*(float s) const { return {x * s, y * s}; }
    [[nodiscard]] D2D1_POINT_2F toD2D() const { return D2D1::Point2F(x, y); }
};

struct Size {
    float w = 0.0f;
    float h = 0.0f;
    Size() = default;
    Size(float width, float height) : w(width), h(height) {}
    [[nodiscard]] bool isEmpty() const { return w <= 0.0f || h <= 0.0f; }
};

struct Insets {
    float top = 0.0f, right = 0.0f, bottom = 0.0f, left = 0.0f;
    Insets() = default;
    Insets(float t, float r, float b, float l) : top(t), right(r), bottom(b), left(l) {}
    static Insets all(float v) { return {v, v, v, v}; }
    static Insets symmetric(float horizontal, float vertical) { return {vertical, horizontal, vertical, horizontal}; }
    [[nodiscard]] float horizontal() const { return left + right; }
    [[nodiscard]] float vertical() const { return top + bottom; }
};

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    Rect() = default;
    Rect(float px, float py, float width, float height) : x(px), y(py), w(width), h(height) {}
    static Rect fromLTRB(float l, float t, float r, float b) { return {l, t, r - l, b - t}; }

    [[nodiscard]] float left() const { return x; }
    [[nodiscard]] float top() const { return y; }
    [[nodiscard]] float right() const { return x + w; }
    [[nodiscard]] float bottom() const { return y + h; }
    [[nodiscard]] Point origin() const { return {x, y}; }
    [[nodiscard]] Size size() const { return {w, h}; }
    [[nodiscard]] Point center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    [[nodiscard]] bool isEmpty() const { return w <= 0.0f || h <= 0.0f; }
    [[nodiscard]] bool contains(Point p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }

    [[nodiscard]] Rect inset(const Insets& i) const { return {x + i.left, y + i.top, w - i.horizontal(), h - i.vertical()}; }
    [[nodiscard]] Rect inset(float v) const { return {x + v, y + v, w - 2 * v, h - 2 * v}; }
    [[nodiscard]] Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    [[nodiscard]] Rect withSize(float nw, float nh) const { return {x, y, nw, nh}; }
    [[nodiscard]] Rect intersect(const Rect& o) const {
        const float l = std::max(x, o.x), t = std::max(y, o.y);
        const float r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return (r > l && b > t) ? fromLTRB(l, t, r, b) : Rect{};
    }
    [[nodiscard]] Rect united(const Rect& o) const {
        if (isEmpty()) return o;
        if (o.isEmpty()) return *this;
        return fromLTRB(std::min(x, o.x), std::min(y, o.y), std::max(right(), o.right()), std::max(bottom(), o.bottom()));
    }
    [[nodiscard]] D2D1_RECT_F toD2D() const { return D2D1::RectF(x, y, x + w, y + h); }
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

/**
 * @brief Straight-alpha float colour.
 */
struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    Color() = default;
    Color(float red, float green, float blue, float alpha = 1.0f) : r(red), g(green), b(blue), a(alpha) {}

    /// 0xRRGGBB + alpha.
    static Color fromHex(uint32_t rgb, float alpha = 1.0f) {
        return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
    }
    /// 0xAARRGGBB.
    static Color fromArgb(uint32_t argb) {
        return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f};
    }
    static Color transparent() { return {0, 0, 0, 0}; }
    static Color white(float alpha = 1.0f) { return {1, 1, 1, alpha}; }
    static Color black(float alpha = 1.0f) { return {0, 0, 0, alpha}; }

    [[nodiscard]] Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
    [[nodiscard]] Color scaledAlpha(float factor) const { return {r, g, b, a * factor}; }
    static Color lerp(const Color& from, const Color& to, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return {from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t, from.a + (to.a - from.a) * t};
    }
    /// Relative luminance (sRGB approximation) for "is this light?" decisions.
    [[nodiscard]] float luminance() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
    [[nodiscard]] D2D1_COLOR_F toD2D() const { return D2D1::ColorF(r, g, b, a); }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

/**
 * @brief Layout constraints (dips). Infinity means "unbounded".
 */
struct Constraints {
    float minW = 0.0f, maxW = 1e9f, minH = 0.0f, maxH = 1e9f;
    static Constraints loose(Size max) { return {0, max.w, 0, max.h}; }
    static Constraints tight(Size s) { return {s.w, s.w, s.h, s.h}; }
    static Constraints unbounded() { return {}; }
    [[nodiscard]] Size constrain(Size s) const {
        return {std::clamp(s.w, minW, maxW), std::clamp(s.h, minH, maxH)};
    }
    [[nodiscard]] bool hasBoundedWidth() const { return maxW < 1e8f; }
    [[nodiscard]] bool hasBoundedHeight() const { return maxH < 1e8f; }
};

/**
 * @brief Dip <-> physical pixel conversion and pixel snapping.
 */
struct DipScale {
    float dpi = 96.0f;
    float scale = 1.0f;
    static DipScale fromDpi(float dpiValue) { DipScale s; s.dpi = dpiValue > 0 ? dpiValue : 96.0f; s.scale = s.dpi / 96.0f; return s; }
    [[nodiscard]] float toPx(float dip) const { return dip * scale; }
    [[nodiscard]] float toDip(float px) const { return px / scale; }
    [[nodiscard]] int toPxInt(float dip) const { return static_cast<int>(std::lround(dip * scale)); }
    /// Rounds a dip value so it lands on a physical pixel boundary.
    [[nodiscard]] float snap(float dip) const { return std::round(dip * scale) / scale; }
    /// Exactly one physical pixel expressed in dips.
    [[nodiscard]] float hairline() const { return 1.0f / scale; }
    [[nodiscard]] Rect snap(const Rect& r) const {
        const float l = snap(r.x), t = snap(r.y);
        return Rect::fromLTRB(l, t, snap(r.right()), snap(r.bottom()));
    }
};

} // namespace hh::ui
