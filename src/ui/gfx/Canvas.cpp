// ---------------------------------------------------------------------------
// Canvas.cpp - the per-frame drawing API (Direct2D behind a dip interface).
// ---------------------------------------------------------------------------
#include "ui/gfx/Canvas.h"

#include "core/Logger.h"
#include "ui/gfx/PathIconsInternal.h"
#include "ui/gfx/TextMeasure.h"

#include <algorithm>
#include <cmath>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Canvas";
constexpr float kPi = 3.14159265358979323846f;

// Colour fonts on, clipping to the layout box so trimmed text never bleeds.
constexpr D2D1_DRAW_TEXT_OPTIONS kTextOptions =
    static_cast<D2D1_DRAW_TEXT_OPTIONS>(D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);

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

} // namespace

/**
 * @brief Ends any open frame and drops the device objects.
 */
Canvas::~Canvas() {
    end();
    resetDeviceResources();
}

/**
 * @brief Adopts the context for this frame and prepares the cached brush/stroke style.
 */
void Canvas::begin(ID2D1DeviceContext1* ctx, const DipScale& scale, const Theme& theme, TextCache& text) {
    if (ctx == nullptr) {
        HH_LOG_WARN(kLog, L"begin: null device context");
        end();
        return;
    }

    // A different context means a different device: the brush is stale.
    if (ctx_ && ctx_.Get() != ctx) {
        resetDeviceResources();
    }
    stack_.clear();
    ctx_ = ctx;
    scale_ = scale;
    theme_ = &theme;
    text_ = &text;

    // One solid brush is recoloured for every fill/stroke of the frame.
    if (!brush_) {
        const HRESULT hr = ctx_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brush_.GetAddressOf());
        if (FAILED(hr) || !brush_) {
            HH_LOG_ERROR(kLog, L"CreateSolidColorBrush failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
            brush_.Reset();
        }
        ++brushGeneration_;
    }

    // Round caps/joins for every polyline glyph and arc.
    if (!roundStroke_) {
        ComPtr<ID2D1Factory> factory;
        ctx_->GetFactory(factory.GetAddressOf());
        if (factory) {
            const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                D2D1_DASH_STYLE_SOLID, 0.0f);
            const HRESULT hr = factory->CreateStrokeStyle(&props, nullptr, 0, roundStroke_.GetAddressOf());
            if (FAILED(hr)) {
                HH_LOG_WARN(kLog, L"CreateStrokeStyle failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
                roundStroke_.Reset();
            }
        }
    }
}

/**
 * @brief Pops whatever a widget forgot to pop and forgets the context.
 */
void Canvas::end() {
    if (ctx_) {
        if (!stack_.empty()) {
            HH_LOG_DEBUG(kLog, L"end: {} unbalanced push(es) popped", stack_.size());
        }
        while (!stack_.empty()) {
            pop();
        }
    }
    stack_.clear();
    ctx_.Reset();
    theme_ = nullptr;
    text_ = nullptr;
}

// ---- shapes -----------------------------------------------------------------

/**
 * @brief Solid axis-aligned rectangle.
 */
void Canvas::fillRect(const Rect& r, const Color& c) {
    if (!ctx_ || r.isEmpty() || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* b = brush(c);
    if (b == nullptr) {
        return;
    }
    ctx_->FillRectangle(r.toD2D(), b);
}

/**
 * @brief Solid rounded rectangle (radius clamped to half the rect).
 */
void Canvas::fillRoundedRect(const Rect& r, float radius, const Color& c) {
    if (!ctx_ || r.isEmpty() || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* b = brush(c);
    if (b == nullptr) {
        return;
    }
    const float rad = clampRadius(r, radius);
    if (rad <= 0.0f) {
        ctx_->FillRectangle(r.toD2D(), b);
        return;
    }
    ctx_->FillRoundedRectangle(D2D1::RoundedRect(r.toD2D(), rad, rad), b);
}

/**
 * @brief Rectangle outline kept inside the rect; width 0 = one physical pixel.
 */
void Canvas::strokeRect(const Rect& r, const Color& c, float width) {
    if (!ctx_ || r.isEmpty() || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* b = brush(c);
    if (b == nullptr) {
        return;
    }
    // Hairlines are snapped so the single pixel row is fully covered.
    const bool hair = width <= 0.0f;
    const float w = hair ? scale_.hairline() : width;
    const Rect base = hair ? scale_.snap(r) : r;
    const Rect inner = base.inset(w * 0.5f);
    if (inner.w <= 0.0f || inner.h <= 0.0f) {
        ctx_->FillRectangle(base.toD2D(), b);
        return;
    }
    ctx_->DrawRectangle(inner.toD2D(), b, w, nullptr);
}

/**
 * @brief Rounded outline kept inside the rect; width 0 = one physical pixel.
 */
void Canvas::strokeRoundedRect(const Rect& r, float radius, const Color& c, float width) {
    if (!ctx_ || r.isEmpty() || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* b = brush(c);
    if (b == nullptr) {
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
    if (rad <= 0.0f) {
        ctx_->DrawRectangle(inner.toD2D(), b, w, nullptr);
        return;
    }
    ctx_->DrawRoundedRectangle(D2D1::RoundedRect(inner.toD2D(), rad, rad), b, w, nullptr);
}

/**
 * @brief Line with round caps; width 0 = one physical pixel.
 */
void Canvas::drawLine(Point a, Point b, const Color& c, float width) {
    if (!ctx_ || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* br = brush(c);
    if (br == nullptr) {
        return;
    }
    const float w = (width <= 0.0f) ? scale_.hairline() : width;
    ctx_->DrawLine(a.toD2D(), b.toD2D(), br, w, roundStroke_.Get());
}

/**
 * @brief One-pixel separator snapped to the grid (crisp at any DPI).
 */
void Canvas::drawHairline(Point a, Point b, const Color& c) {
    if (!ctx_ || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* br = brush(c);
    if (br == nullptr) {
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
    ctx_->DrawLine(p0.toD2D(), p1.toD2D(), br, h, nullptr);
}

/**
 * @brief Filled circle.
 */
void Canvas::fillCircle(Point center, float radius, const Color& c) {
    if (!ctx_ || radius <= 0.0f || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* b = brush(c);
    if (b == nullptr) {
        return;
    }
    ctx_->FillEllipse(D2D1::Ellipse(center.toD2D(), radius, radius), b);
}

/**
 * @brief Circle outline kept inside the radius; width 0 = hairline.
 */
void Canvas::strokeCircle(Point center, float radius, const Color& c, float width) {
    if (!ctx_ || radius <= 0.0f || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* b = brush(c);
    if (b == nullptr) {
        return;
    }
    const float w = (width <= 0.0f) ? scale_.hairline() : width;
    const float r = std::max(0.0f, radius - w * 0.5f);
    if (r <= 0.0f) {
        ctx_->FillEllipse(D2D1::Ellipse(center.toD2D(), radius, radius), b);
        return;
    }
    ctx_->DrawEllipse(D2D1::Ellipse(center.toD2D(), r, r), b, w, nullptr);
}

/**
 * @brief Filled ellipse inscribed in the bounds.
 */
void Canvas::fillEllipse(const Rect& bounds, const Color& c) {
    if (!ctx_ || bounds.isEmpty() || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* b = brush(c);
    if (b == nullptr) {
        return;
    }
    ctx_->FillEllipse(D2D1::Ellipse(bounds.center().toD2D(), bounds.w * 0.5f, bounds.h * 0.5f), b);
}

/**
 * @brief Arc of a circle; angles in degrees clockwise from 12 o'clock.
 */
void Canvas::strokeArc(Point center, float radius, float startDeg, float sweepDeg, const Color& c, float width) {
    if (!ctx_ || radius <= 0.0f || c.a <= 0.0f || sweepDeg == 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* b = brush(c);
    if (b == nullptr) {
        return;
    }
    const float w = (width <= 0.0f) ? scale_.hairline() : width;

    // A full turn is just a circle; arcs cannot express 360 degrees anyway.
    if (std::fabs(sweepDeg) >= 360.0f) {
        ctx_->DrawEllipse(D2D1::Ellipse(center.toD2D(), radius, radius), b, w, roundStroke_.Get());
        return;
    }
    ComPtr<ID2D1Factory> factory;
    ctx_->GetFactory(factory.GetAddressOf());
    if (!factory) {
        return;
    }
    ComPtr<ID2D1PathGeometry> geometry;
    HRESULT hr = factory->CreatePathGeometry(geometry.GetAddressOf());
    if (FAILED(hr) || !geometry) {
        return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    hr = geometry->Open(sink.GetAddressOf());
    if (FAILED(hr) || !sink) {
        return;
    }

    // Open figure from the start angle sweeping to the end angle.
    const Point start = pointOnCircle(center, radius, startDeg);
    const Point finish = pointOnCircle(center, radius, startDeg + sweepDeg);
    sink->BeginFigure(start.toD2D(), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(finish.toD2D(), D2D1::SizeF(radius, radius), 0.0f,
                                  sweepDeg > 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                  std::fabs(sweepDeg) > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    hr = sink->Close();
    if (FAILED(hr)) {
        return;
    }
    ctx_->DrawGeometry(geometry.Get(), b, w, roundStroke_.Get());
}

/**
 * @brief Two-stop linear gradient fill (vertical or horizontal), optionally rounded.
 */
void Canvas::fillLinearGradient(const Rect& r, const Color& from, const Color& to, bool vertical, float radius) {
    if (!ctx_ || r.isEmpty()) {
        return;
    }
    const D2D1_GRADIENT_STOP stops[2] = {{0.0f, from.toD2D()}, {1.0f, to.toD2D()}};
    ComPtr<ID2D1GradientStopCollection> collection;
    HRESULT hr = ctx_->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, collection.GetAddressOf());
    if (FAILED(hr) || !collection) {
        return;
    }
    // The gradient runs top-to-bottom or left-to-right from the rect's origin.
    const D2D1_POINT_2F start = D2D1::Point2F(r.x, r.y);
    const D2D1_POINT_2F finish = vertical ? D2D1::Point2F(r.x, r.bottom()) : D2D1::Point2F(r.right(), r.y);
    ComPtr<ID2D1LinearGradientBrush> gradient;
    hr = ctx_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(start, finish), collection.Get(), gradient.GetAddressOf());
    if (FAILED(hr) || !gradient) {
        return;
    }
    const float rad = clampRadius(r, radius);
    if (rad > 0.0f) {
        ctx_->FillRoundedRectangle(D2D1::RoundedRect(r.toD2D(), rad, rad), gradient.Get());
    } else {
        ctx_->FillRectangle(r.toD2D(), gradient.Get());
    }
}

/**
 * @brief Soft shadow: eight expanding rounded rects with quadratic alpha falloff.
 */
void Canvas::drawShadow(const Rect& r, float radius, float blur, const Color& c, float offsetY) {
    if (!ctx_ || r.isEmpty() || c.a <= 0.0f) {
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

/**
 * @brief Two-dip ring just outside the rect (keyboard focus).
 */
void Canvas::drawFocusRing(const Rect& r, float radius, const Color& c) {
    if (!ctx_ || c.a <= 0.0f) {
        return;
    }
    strokeRoundedRect(r.inset(-2.0f), radius + 2.0f, c, 2.0f);
}

// ---- text -------------------------------------------------------------------

/**
 * @brief Draws aligned, trimmed text inside the bounds and returns its size.
 */
Size Canvas::drawText(std::wstring_view text, const TextStyle& style, const Rect& bounds, const Color& c, HAlign h, VAlign v,
                      Trimming trimming, int maxLines) {
    if (!ctx_ || text_ == nullptr || text.empty()) {
        return {};
    }
    const float maxWidth = (bounds.w > 0.0f) ? bounds.w : 0.0f;
    ComPtr<IDWriteTextLayout> layout = text_->layout(text, style, maxWidth, trimming, maxLines);
    if (!layout) {
        return {};
    }
    DWRITE_TEXT_METRICS m = {};
    if (FAILED(layout->GetMetrics(&m))) {
        return {};
    }

    // Horizontal alignment offsets the origin by the measured text width.
    Point origin = bounds.origin();
    switch (h) {
    case HAlign::Center: origin.x = bounds.x + (bounds.w - m.width) * 0.5f; break;
    case HAlign::Right: origin.x = bounds.right() - m.width; break;
    case HAlign::Left: default: break;
    }
    switch (v) {
    case VAlign::Center: origin.y = bounds.y + (bounds.h - m.height) * 0.5f; break;
    case VAlign::Bottom: origin.y = bounds.bottom() - m.height; break;
    case VAlign::Top: default: break;
    }

    // Snapping the origin keeps baselines on whole pixels (sharper text).
    origin.y = scale_.snap(origin.y);
    if (c.a > 0.0f) {
        ID2D1SolidColorBrush* b = brush(c);
        if (b != nullptr) {
            ctx_->DrawTextLayout(origin.toD2D(), layout.Get(), b, kTextOptions);
        }
    }
    return {std::max(0.0f, m.widthIncludingTrailingWhitespace), std::max(0.0f, m.height)};
}

/**
 * @brief Measures through the frame's cache (or the shared one when detached).
 */
Size Canvas::measureText(std::wstring_view text, const TextStyle& style, float maxWidth, int maxLines) {
    if (text_ != nullptr) {
        return text_->measure(text, style, maxWidth, maxLines);
    }
    return measureTextShared(text, style, maxWidth, maxLines);
}

/**
 * @brief Draws a prepared layout at the origin.
 */
void Canvas::drawTextLayout(IDWriteTextLayout* layout, Point origin, const Color& c) {
    if (!ctx_ || layout == nullptr || c.a <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* b = brush(c);
    if (b == nullptr) {
        return;
    }
    ctx_->DrawTextLayout(origin.toD2D(), layout, b, kTextOptions);
}

// ---- icons ------------------------------------------------------------------

/**
 * @brief Vector icon centred in the bounds.
 */
void Canvas::drawIcon(IconId id, const Rect& bounds, const Color& c, float strokeWidth) {
    if (!ctx_ || theme_ == nullptr || id == IconId::None || bounds.isEmpty()) {
        return;
    }
    drawIconShape(*this, id, bounds, c, std::max(0.5f, strokeWidth));
}

// ---- bitmaps ----------------------------------------------------------------

/**
 * @brief Stretches a bitmap into the destination rect with linear filtering.
 */
void Canvas::drawBitmap(ID2D1Bitmap* bitmap, const Rect& dest, float opacity) {
    if (!ctx_ || bitmap == nullptr || dest.isEmpty() || opacity <= 0.0f) {
        return;
    }
    const D2D1_RECT_F d = dest.toD2D();
    ctx_->DrawBitmap(bitmap, &d, std::clamp(opacity, 0.0f, 1.0f), D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
}

// ---- state stack --------------------------------------------------------------

/**
 * @brief Axis-aligned clip (antialiased edges).
 */
void Canvas::pushClip(const Rect& r) {
    if (!ctx_) {
        return;
    }
    // A degenerate rect still needs a matching pop, so push an empty clip.
    const Rect safe = {r.x, r.y, std::max(0.0f, r.w), std::max(0.0f, r.h)};
    ctx_->PushAxisAlignedClip(safe.toD2D(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    stack_.push_back({StackKind::Clip, {}});
}

/**
 * @brief Rounded-rect clip via a geometry-masked layer.
 */
void Canvas::pushRoundedClip(const Rect& r, float radius) {
    if (!ctx_) {
        return;
    }
    const Rect safe = {r.x, r.y, std::max(0.0f, r.w), std::max(0.0f, r.h)};
    const float rad = clampRadius(safe, radius);
    ComPtr<ID2D1Factory> factory;
    ctx_->GetFactory(factory.GetAddressOf());
    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    if (factory) {
        factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(safe.toD2D(), rad, rad), geometry.GetAddressOf());
    }
    if (!geometry) {
        // Without the mask geometry fall back to the plain clip so the pop still matches.
        pushClip(safe);
        return;
    }
    const D2D1_LAYER_PARAMETERS1 params = D2D1::LayerParameters1(safe.toD2D(), geometry.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                                  D2D1::Matrix3x2F::Identity(), 1.0f, nullptr,
                                                                  D2D1_LAYER_OPTIONS1_NONE);
    ctx_->PushLayer(&params, nullptr);
    stack_.push_back({StackKind::Layer, {}});
}

/**
 * @brief Group opacity via a layer.
 */
void Canvas::pushOpacity(float opacity) {
    if (!ctx_) {
        return;
    }
    const D2D1_LAYER_PARAMETERS1 params = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                                  D2D1::Matrix3x2F::Identity(), std::clamp(opacity, 0.0f, 1.0f),
                                                                  nullptr, D2D1_LAYER_OPTIONS1_NONE);
    ctx_->PushLayer(&params, nullptr);
    stack_.push_back({StackKind::Layer, {}});
}

/**
 * @brief Pre-multiplies a matrix onto the current transform (local space first).
 */
void Canvas::pushTransform(const D2D1_MATRIX_3X2_F& m) {
    if (!ctx_) {
        return;
    }
    D2D1_MATRIX_3X2_F current = D2D1::Matrix3x2F::Identity();
    ctx_->GetTransform(&current);
    const D2D1::Matrix3x2F combined = *D2D1::Matrix3x2F::ReinterpretBaseType(&m) * *D2D1::Matrix3x2F::ReinterpretBaseType(&current);
    ctx_->SetTransform(combined);
    stack_.push_back({StackKind::Transform, current});
}

/**
 * @brief Reverses the most recent push.
 */
void Canvas::pop() {
    if (!ctx_ || stack_.empty()) {
        return;
    }
    const StackEntry entry = stack_.back();
    stack_.pop_back();
    switch (entry.kind) {
    case StackKind::Clip: ctx_->PopAxisAlignedClip(); break;
    case StackKind::Layer: ctx_->PopLayer(); break;
    case StackKind::Transform: ctx_->SetTransform(entry.savedTransform); break;
    default: break;
    }
}

/**
 * @brief The cached solid brush recoloured to the given colour.
 */
ID2D1SolidColorBrush* Canvas::brush(const Color& c) {
    if (!ctx_) {
        return nullptr;
    }
    if (!brush_) {
        const HRESULT hr = ctx_->CreateSolidColorBrush(c.toD2D(), brush_.GetAddressOf());
        if (FAILED(hr) || !brush_) {
            brush_.Reset();
            return nullptr;
        }
        ++brushGeneration_;
        return brush_.Get();
    }
    brush_->SetColor(c.toD2D());
    return brush_.Get();
}

/**
 * @brief Drops the brush and stroke style (device loss / device change).
 */
void Canvas::resetDeviceResources() {
    brush_.Reset();
    roundStroke_.Reset();
    ++brushGeneration_;
}

} // namespace hh::ui
