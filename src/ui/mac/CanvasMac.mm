// ---------------------------------------------------------------------------
// CanvasMac.mm - the Canvas drawing API on CoreGraphics.
//
// The host view is flipped (y grows down), so dips map 1:1 onto points and
// the geometry code below is a line-for-line port of the Direct2D Canvas:
// same hairline snapping, same "stroke inside the rect" rule, same round
// caps, same shadow rings. Colours are sRGB: the fill/stroke colour spaces
// are pinned to sRGB at begin(), so a design colour looks identical on a
// P3 display and in a PNG capture.
//
// State stack: every push is one CGContextSaveGState; group opacity adds a
// transparency layer so overlapping children fade as one (D2D layer parity).
// ---------------------------------------------------------------------------
#include "ui/gfx/Canvas.h"

#include "core/Logger.h"
#include "ui/gfx/PathIconsInternal.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/mac/MacTextLayout.h"

#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Canvas";
constexpr float kPi = 3.14159265358979323846f;

/// The sRGB colour space, created once for the process.
CGColorSpaceRef srgbSpace() {
    static CGColorSpaceRef s_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    return s_space;
}

/**
 * @brief Point on a circle for an angle in degrees measured clockwise from 12 o'clock.
 */
Point pointOnCircle(Point center, float radius, float deg) {
    const float rad = deg * kPi / 180.0f;
    return {center.x + radius * std::sin(rad), center.y - radius * std::cos(rad)};
}

/**
 * @brief Clamps a corner radius so it never exceeds half the rect.
 */
float clampRadius(const Rect& r, float radius) {
    return std::clamp(radius, 0.0f, std::max(0.0f, std::min(r.w, r.h) * 0.5f));
}

/// Rect -> CGRect.
CGRect cg(const Rect& r) {
    return CGRectMake(r.x, r.y, r.w, r.h);
}

/// Fill colour (sRGB components, straight alpha).
void setFill(CGContextRef ctx, const Color& c) {
    const CGFloat comps[4] = {c.r, c.g, c.b, c.a};
    CGContextSetFillColor(ctx, comps);
}

/// Stroke colour (sRGB components, straight alpha).
void setStroke(CGContextRef ctx, const Color& c) {
    const CGFloat comps[4] = {c.r, c.g, c.b, c.a};
    CGContextSetStrokeColor(ctx, comps);
}

/// Adds a rounded rect (or a plain rect for radius 0) to the current path.
void addRoundedRect(CGContextRef ctx, const Rect& r, float radius) {
    if (radius <= 0.0f) {
        CGContextAddRect(ctx, cg(r));
        return;
    }
    CGPathRef path = CGPathCreateWithRoundedRect(cg(r), radius, radius, nullptr);
    if (path != nullptr) {
        CGContextAddPath(ctx, path);
        CGPathRelease(path);
    }
}

/// Normalises an angle into [0, 2pi).
double wrapAngle(double a) {
    const double twoPi = 2.0 * kPi;
    a = std::fmod(a, twoPi);
    return a < 0.0 ? a + twoPi : a;
}

/**
 * @brief Adds the arc from @p from to @p to on the circle (c, r) that passes
 *        through @p via (the bulge side), whichever direction that takes.
 */
void addArcThrough(CGMutablePathRef path, Point c, float r, Point from, Point to, Point via) {
    const double a0 = std::atan2(from.y - c.y, from.x - c.x);
    const double a1 = std::atan2(to.y - c.y, to.x - c.x);
    const double av = std::atan2(via.y - c.y, via.x - c.x);
    // Increasing angle from a0: does it meet 'via' before 'to'?
    const double spanTo = wrapAngle(a1 - a0);
    const double spanVia = wrapAngle(av - a0);
    const bool increasing = spanVia <= spanTo;
    // CoreGraphics' "clockwise" flag means decreasing angle in user space.
    CGPathAddArc(path, nullptr, c.x, c.y, r, a0, a1, increasing ? false : true);
}

} // namespace

// ===========================================================================
// Frame
// ===========================================================================

Canvas::~Canvas() {
    end();
    resetDeviceResources();
}

void Canvas::begin(NativeDrawContext ctx, const DipScale& scale, const Theme& theme, TextCache& text) {
    if (ctx == nullptr) {
        HH_LOG_WARN(kLog, L"begin: null CGContext");
        end();
        return;
    }
    stack_.clear();
    ctx_ = ctx;
    scale_ = scale;
    theme_ = &theme;
    text_ = &text;

    // Design colours are sRGB; pin both colour spaces for the whole frame.
    CGContextSetFillColorSpace(ctx_, srgbSpace());
    CGContextSetStrokeColorSpace(ctx_, srgbSpace());
    CGContextSetShouldAntialias(ctx_, true);
    CGContextSetAllowsAntialiasing(ctx_, true);
    CGContextSetInterpolationQuality(ctx_, kCGInterpolationHigh);
}

NativeDrawContext Canvas::ctx() const noexcept {
    return ctx_;
}

void Canvas::end() {
    if (ctx_ != nullptr) {
        if (!stack_.empty()) {
            HH_LOG_DEBUG(kLog, L"end: {} unbalanced push(es) popped", stack_.size());
        }
        while (!stack_.empty()) {
            pop();
        }
    }
    stack_.clear();
    ctx_ = nullptr;
    theme_ = nullptr;
    text_ = nullptr;
}

void Canvas::resetDeviceResources() {
    // CoreGraphics has no device objects to lose.
}

// ===========================================================================
// Shapes
// ===========================================================================

void Canvas::fillRect(const Rect& r, const Color& c) {
    if (ctx_ == nullptr || r.isEmpty() || c.a <= 0.0f) {
        return;
    }
    setFill(ctx_, c);
    CGContextFillRect(ctx_, cg(r));
}

void Canvas::fillRoundedRect(const Rect& r, float radius, const Color& c) {
    if (ctx_ == nullptr || r.isEmpty() || c.a <= 0.0f) {
        return;
    }
    setFill(ctx_, c);
    const float rad = clampRadius(r, radius);
    if (rad <= 0.0f) {
        CGContextFillRect(ctx_, cg(r));
        return;
    }
    CGContextBeginPath(ctx_);
    addRoundedRect(ctx_, r, rad);
    CGContextFillPath(ctx_);
}

void Canvas::strokeRect(const Rect& r, const Color& c, float width) {
    if (ctx_ == nullptr || r.isEmpty() || c.a <= 0.0f) {
        return;
    }
    // Hairlines are snapped so the single pixel row is fully covered.
    const bool hair = width <= 0.0f;
    const float w = hair ? scale_.hairline() : width;
    const Rect base = hair ? scale_.snap(r) : r;
    const Rect inner = base.inset(w * 0.5f);
    if (inner.w <= 0.0f || inner.h <= 0.0f) {
        setFill(ctx_, c);
        CGContextFillRect(ctx_, cg(base));
        return;
    }
    setStroke(ctx_, c);
    CGContextSetLineWidth(ctx_, w);
    CGContextStrokeRect(ctx_, cg(inner));
}

void Canvas::strokeRoundedRect(const Rect& r, float radius, const Color& c, float width) {
    if (ctx_ == nullptr || r.isEmpty() || c.a <= 0.0f) {
        return;
    }
    const bool hair = width <= 0.0f;
    const float w = hair ? scale_.hairline() : width;
    const Rect base = hair ? scale_.snap(r) : r;
    const Rect inner = base.inset(w * 0.5f);
    if (inner.w <= 0.0f || inner.h <= 0.0f) {
        return;
    }
    // The visible outer edge keeps the requested radius; the path centre is
    // half a stroke inside so its radius shrinks by the same amount.
    const float rad = clampRadius(inner, std::max(0.0f, radius - w * 0.5f));
    setStroke(ctx_, c);
    CGContextSetLineWidth(ctx_, w);
    CGContextBeginPath(ctx_);
    addRoundedRect(ctx_, inner, rad);
    CGContextStrokePath(ctx_);
}

void Canvas::drawLine(Point a, Point b, const Color& c, float width) {
    if (ctx_ == nullptr || c.a <= 0.0f) {
        return;
    }
    const float w = (width <= 0.0f) ? scale_.hairline() : width;
    setStroke(ctx_, c);
    CGContextSetLineWidth(ctx_, w);
    CGContextSetLineCap(ctx_, kCGLineCapRound);
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, a.x, a.y);
    CGContextAddLineToPoint(ctx_, b.x, b.y);
    CGContextStrokePath(ctx_);
}

void Canvas::drawHairline(Point a, Point b, const Color& c) {
    if (ctx_ == nullptr || c.a <= 0.0f) {
        return;
    }
    const float h = scale_.hairline();
    const float half = h * 0.5f;
    Point p0 = a;
    Point p1 = b;
    if (a.y == b.y) {
        // Horizontal: land on one pixel row, butt caps at snapped x.
        const float y = scale_.snap(a.y) + half;
        p0 = {scale_.snap(a.x), y};
        p1 = {scale_.snap(b.x), y};
    } else if (a.x == b.x) {
        // Vertical: land on one pixel column.
        const float x = scale_.snap(a.x) + half;
        p0 = {x, scale_.snap(a.y)};
        p1 = {x, scale_.snap(b.y)};
    } else {
        // Diagonal hairlines cannot be pixel-exact; centre them at least.
        p0 = {scale_.snap(a.x) + half, scale_.snap(a.y) + half};
        p1 = {scale_.snap(b.x) + half, scale_.snap(b.y) + half};
    }
    setStroke(ctx_, c);
    CGContextSetLineWidth(ctx_, h);
    CGContextSetLineCap(ctx_, kCGLineCapButt);
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, p0.x, p0.y);
    CGContextAddLineToPoint(ctx_, p1.x, p1.y);
    CGContextStrokePath(ctx_);
}

void Canvas::fillCircle(Point center, float radius, const Color& c) {
    if (ctx_ == nullptr || radius <= 0.0f || c.a <= 0.0f) {
        return;
    }
    setFill(ctx_, c);
    CGContextFillEllipseInRect(ctx_, CGRectMake(center.x - radius, center.y - radius, radius * 2.0f, radius * 2.0f));
}

void Canvas::strokeCircle(Point center, float radius, const Color& c, float width) {
    if (ctx_ == nullptr || radius <= 0.0f || c.a <= 0.0f) {
        return;
    }
    const float w = (width <= 0.0f) ? scale_.hairline() : width;
    const float r = std::max(0.0f, radius - w * 0.5f);
    if (r <= 0.0f) {
        fillCircle(center, radius, c);
        return;
    }
    setStroke(ctx_, c);
    CGContextSetLineWidth(ctx_, w);
    CGContextStrokeEllipseInRect(ctx_, CGRectMake(center.x - r, center.y - r, r * 2.0f, r * 2.0f));
}

void Canvas::fillEllipse(const Rect& bounds, const Color& c) {
    if (ctx_ == nullptr || bounds.isEmpty() || c.a <= 0.0f) {
        return;
    }
    setFill(ctx_, c);
    CGContextFillEllipseInRect(ctx_, cg(bounds));
}

void Canvas::strokeArc(Point center, float radius, float startDeg, float sweepDeg, const Color& c, float width) {
    if (ctx_ == nullptr || radius <= 0.0f || c.a <= 0.0f || sweepDeg == 0.0f) {
        return;
    }
    const float w = (width <= 0.0f) ? scale_.hairline() : width;
    setStroke(ctx_, c);
    CGContextSetLineWidth(ctx_, w);
    CGContextSetLineCap(ctx_, kCGLineCapRound);
    CGContextSetLineJoin(ctx_, kCGLineJoinRound);

    // A full turn is just a circle.
    if (std::fabs(sweepDeg) >= 360.0f) {
        CGContextStrokeEllipseInRect(ctx_, CGRectMake(center.x - radius, center.y - radius, radius * 2.0f, radius * 2.0f));
        return;
    }
    // "Clockwise from 12 o'clock" in a y-down space: angle 0 points up (-y),
    // and increasing CG angles turn visually clockwise.
    const double start = (static_cast<double>(startDeg) - 90.0) * kPi / 180.0;
    const double end = (static_cast<double>(startDeg + sweepDeg) - 90.0) * kPi / 180.0;
    CGContextBeginPath(ctx_);
    const Point p0 = pointOnCircle(center, radius, startDeg);
    CGContextMoveToPoint(ctx_, p0.x, p0.y);
    CGContextAddArc(ctx_, center.x, center.y, radius, start, end, sweepDeg > 0.0f ? 0 : 1);
    CGContextStrokePath(ctx_);
}

void Canvas::fillLinearGradient(const Rect& r, const Color& from, const Color& to, bool vertical, float radius) {
    if (ctx_ == nullptr || r.isEmpty()) {
        return;
    }
    const CGFloat comps[8] = {from.r, from.g, from.b, from.a, to.r, to.g, to.b, to.a};
    const CGFloat locations[2] = {0.0, 1.0};
    CGGradientRef gradient = CGGradientCreateWithColorComponents(srgbSpace(), comps, locations, 2);
    if (gradient == nullptr) {
        return;
    }
    CGContextSaveGState(ctx_);
    CGContextBeginPath(ctx_);
    addRoundedRect(ctx_, r, clampRadius(r, radius));
    CGContextClip(ctx_);
    // The gradient runs top-to-bottom or left-to-right from the rect's origin.
    const CGPoint start = CGPointMake(r.x, r.y);
    const CGPoint finish = vertical ? CGPointMake(r.x, r.bottom()) : CGPointMake(r.right(), r.y);
    CGContextDrawLinearGradient(ctx_, gradient, start, finish, kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    CGContextRestoreGState(ctx_);
    CGGradientRelease(gradient);
}

void Canvas::drawShadow(const Rect& r, float radius, float blur, const Color& c, float offsetY) {
    if (ctx_ == nullptr || r.isEmpty() || c.a <= 0.0f) {
        return;
    }
    const Rect base = r.offset(0.0f, offsetY);
    const float spread = std::max(0.0f, blur);
    if (spread <= 0.0f) {
        fillRoundedRect(base, radius, c);
        return;
    }
    // Ring i expands by (i+1)/8 of the blur; alpha fades quadratically so the
    // overlap of all rings gives a smooth centre and a feathered edge.
    constexpr int kRings = 8;
    for (int i = 0; i < kRings; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kRings);
        const float expand = spread * static_cast<float>(i + 1) / static_cast<float>(kRings);
        const float alpha = c.a * (1.0f - t) * (1.0f - t) / static_cast<float>(kRings);
        fillRoundedRect(base.inset(-expand), radius + expand, c.withAlpha(alpha));
    }
}

void Canvas::drawFocusRing(const Rect& r, float radius, const Color& c) {
    if (ctx_ == nullptr || c.a <= 0.0f) {
        return;
    }
    strokeRoundedRect(r.inset(-2.0f), radius + 2.0f, c, 2.0f);
}

// ===========================================================================
// Paths
// ===========================================================================

void Canvas::strokePolyline(const Point* points, size_t count, bool closed, const Color& c, float width) {
    if (ctx_ == nullptr || points == nullptr || count < 2 || c.a <= 0.0f) {
        return;
    }
    if (count == 2) {
        // Two points: the plain line already has round caps.
        drawLine(points[0], points[1], c, width);
        return;
    }
    setStroke(ctx_, c);
    CGContextSetLineWidth(ctx_, (width <= 0.0f) ? scale_.hairline() : width);
    CGContextSetLineCap(ctx_, kCGLineCapRound);
    CGContextSetLineJoin(ctx_, kCGLineJoinRound);
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, points[0].x, points[0].y);
    for (size_t i = 1; i < count; ++i) {
        CGContextAddLineToPoint(ctx_, points[i].x, points[i].y);
    }
    if (closed) {
        CGContextClosePath(ctx_);
    }
    CGContextStrokePath(ctx_);
}

void Canvas::fillPolygon(const Point* points, size_t count, const Color& c) {
    if (ctx_ == nullptr || points == nullptr || count < 3 || c.a <= 0.0f) {
        return;
    }
    setFill(ctx_, c);
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, points[0].x, points[0].y);
    for (size_t i = 1; i < count; ++i) {
        CGContextAddLineToPoint(ctx_, points[i].x, points[i].y);
    }
    CGContextClosePath(ctx_);
    CGContextFillPath(ctx_);
}

void Canvas::strokeLens(Point left, Point right, float bulge, const Color& c, float width) {
    if (ctx_ == nullptr || c.a <= 0.0f) {
        return;
    }
    // Same geometry as the D2D version: two arcs whose sagitta is the bulge.
    const float dx = right.x - left.x;
    const float dy = right.y - left.y;
    const float chord = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
    const float half = chord * 0.5f;
    const float s = std::max(0.001f, bulge);
    const float radius = (half * half + s * s) / (2.0f * s);
    const Point mid{(left.x + right.x) * 0.5f, (left.y + right.y) * 0.5f};
    const Point u{dx / chord, dy / chord};
    // Clockwise travel left -> right bulges to the left of the direction of
    // travel in a y-down space (upwards for a horizontal chord).
    const Point n{u.y, -u.x};
    const float off = radius - s;
    const Point upperCentre{mid.x - n.x * off, mid.y - n.y * off};
    const Point lowerCentre{mid.x + n.x * off, mid.y + n.y * off};
    const Point upperVia{mid.x + n.x * s, mid.y + n.y * s};
    const Point lowerVia{mid.x - n.x * s, mid.y - n.y * s};

    CGMutablePathRef path = CGPathCreateMutable();
    if (path == nullptr) {
        return;
    }
    CGPathMoveToPoint(path, nullptr, left.x, left.y);
    addArcThrough(path, upperCentre, radius, left, right, upperVia);
    addArcThrough(path, lowerCentre, radius, right, left, lowerVia);
    CGPathCloseSubpath(path);

    setStroke(ctx_, c);
    CGContextSetLineWidth(ctx_, width);
    CGContextSetLineCap(ctx_, kCGLineCapButt);
    CGContextSetLineJoin(ctx_, kCGLineJoinMiter);
    CGContextBeginPath(ctx_);
    CGContextAddPath(ctx_, path);
    CGContextStrokePath(ctx_);
    CGPathRelease(path);
}

// ===========================================================================
// Text
// ===========================================================================

Size Canvas::drawText(std::wstring_view text, const TextStyle& style, const Rect& bounds, const Color& c, HAlign h,
                      VAlign v, Trimming trimming, int maxLines) {
    if (ctx_ == nullptr || text_ == nullptr || text.empty()) {
        return {};
    }
    const float maxWidth = (bounds.w > 0.0f) ? bounds.w : 0.0f;
    TextLayoutRef layout = text_->layout(text, style, maxWidth, trimming, maxLines);
    if (!layout) {
        return {};
    }
    const Size m = TextCache::layoutSize(layout);

    // Horizontal alignment offsets the origin by the measured text width.
    Point origin = bounds.origin();
    switch (h) {
    case HAlign::Center: origin.x = bounds.x + (bounds.w - m.w) * 0.5f; break;
    case HAlign::Right: origin.x = bounds.right() - m.w; break;
    case HAlign::Left: default: break;
    }
    switch (v) {
    case VAlign::Center: origin.y = bounds.y + (bounds.h - m.h) * 0.5f; break;
    case VAlign::Bottom: origin.y = bounds.bottom() - m.h; break;
    case VAlign::Top: default: break;
    }

    // Snapping the origin keeps baselines on whole pixels (sharper text).
    origin.y = scale_.snap(origin.y);
    if (c.a > 0.0f) {
        drawTextLayout(layout, origin, c);
    }
    return m;
}

Size Canvas::measureText(std::wstring_view text, const TextStyle& style, float maxWidth, int maxLines) {
    if (text_ != nullptr) {
        return text_->measure(text, style, maxWidth, maxLines);
    }
    return measureTextShared(text, style, maxWidth, maxLines);
}

void Canvas::drawTextLayout(const TextLayoutRef& layout, Point origin, const Color& c) {
    if (ctx_ == nullptr || !layout || c.a <= 0.0f) {
        return;
    }
    // Glyphs take the context's fill colour (kCTForegroundColorFromContextAttributeName).
    setFill(ctx_, c);
    layout->draw(ctx_, origin);
}

// ===========================================================================
// Icons
// ===========================================================================

void Canvas::drawIcon(IconId id, const Rect& bounds, const Color& c, float strokeWidth) {
    if (ctx_ == nullptr || theme_ == nullptr || id == IconId::None || bounds.isEmpty()) {
        return;
    }
    drawIconShape(*this, id, bounds, c, std::max(0.5f, strokeWidth));
}

// ===========================================================================
// State stack
// ===========================================================================

void Canvas::pushClip(const Rect& r) {
    if (ctx_ == nullptr) {
        return;
    }
    // A degenerate rect still needs a matching pop, so clip to an empty rect.
    const Rect safe = {r.x, r.y, std::max(0.0f, r.w), std::max(0.0f, r.h)};
    CGContextSaveGState(ctx_);
    CGContextClipToRect(ctx_, cg(safe));
    stack_.push_back(StackKind::State);
}

void Canvas::pushRoundedClip(const Rect& r, float radius) {
    if (ctx_ == nullptr) {
        return;
    }
    const Rect safe = {r.x, r.y, std::max(0.0f, r.w), std::max(0.0f, r.h)};
    CGContextSaveGState(ctx_);
    CGContextBeginPath(ctx_);
    addRoundedRect(ctx_, safe, clampRadius(safe, radius));
    CGContextClip(ctx_);
    stack_.push_back(StackKind::State);
}

void Canvas::pushOpacity(float opacity) {
    if (ctx_ == nullptr) {
        return;
    }
    const float o = std::clamp(opacity, 0.0f, 1.0f);
    CGContextSaveGState(ctx_);
    if (o >= 0.999f) {
        // Fully opaque: a layer would only cost memory.
        stack_.push_back(StackKind::State);
        return;
    }
    CGContextSetAlpha(ctx_, o);
    CGContextBeginTransparencyLayer(ctx_, nullptr);
    stack_.push_back(StackKind::Layer);
}

void Canvas::pushTransform(const Transform2D& m) {
    if (ctx_ == nullptr) {
        return;
    }
    CGContextSaveGState(ctx_);
    // Row-vector Transform2D maps 1:1 onto CGAffineTransform {a, b, c, d, tx, ty}.
    CGContextConcatCTM(ctx_, CGAffineTransformMake(m.m11, m.m12, m.m21, m.m22, m.dx, m.dy));
    stack_.push_back(StackKind::State);
}

void Canvas::pop() {
    if (ctx_ == nullptr || stack_.empty()) {
        return;
    }
    const StackKind kind = stack_.back();
    stack_.pop_back();
    if (kind == StackKind::Layer) {
        CGContextEndTransparencyLayer(ctx_);
    }
    CGContextRestoreGState(ctx_);
}

} // namespace hh::ui
