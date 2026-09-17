// ---------------------------------------------------------------------------
// PathIcons.cpp - every IconId drawn from lines, arcs and rounded rects.
//
// Icons are described in a unit square (0..1) and mapped into the glyph box
// so a single definition scales to any size. Polylines go through one path
// geometry per call so joins are round and continuous; a handful of lines
// per icon per frame is nothing for Direct2D.
// ---------------------------------------------------------------------------
#include "ui/gfx/PathIconsInternal.h"

#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace hh::ui {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/**
 * @brief Maps unit-square coordinates into the glyph box and holds the drawing state.
 */
struct Glyph {
    Canvas& c;
    Rect box;            ///< the glyph square in local dips
    Color colour;
    float stroke;        ///< stroke width in dips

    /// Unit coordinate -> local dip point.
    [[nodiscard]] Point p(float u, float v) const { return {box.x + u * box.w, box.y + v * box.h}; }
    /// Unit length -> dips.
    [[nodiscard]] float len(float u) const { return u * box.w; }
    /// Unit rect -> local dip rect.
    [[nodiscard]] Rect r(float u, float v, float w, float h) const { return {box.x + u * box.w, box.y + v * box.h, w * box.w, h * box.h}; }
};

/**
 * @brief Round cap/join stroke style, cached per Direct2D factory.
 *
 * Stroke styles are factory resources (device-independent), so one instance
 * serves every frame until the factory itself is replaced.
 */
ID2D1StrokeStyle* roundStrokeStyle(Canvas& c) {
    static ComPtr<ID2D1StrokeStyle> cached;
    static ID2D1Factory* cachedFactory = nullptr;
    if (c.ctx() == nullptr) {
        return nullptr;
    }
    ComPtr<ID2D1Factory> factory;
    c.ctx()->GetFactory(factory.GetAddressOf());
    if (!factory) {
        return nullptr;
    }
    // Rebuild when the factory changed (device recreation with a new factory).
    if (!cached || cachedFactory != factory.Get()) {
        cached.Reset();
        const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        if (FAILED(factory->CreateStrokeStyle(&props, nullptr, 0, cached.GetAddressOf()))) {
            cached.Reset();
            cachedFactory = nullptr;
            return nullptr;
        }
        cachedFactory = factory.Get();
    }
    return cached.Get();
}

/**
 * @brief Builds a path geometry from points (optionally closed / filled).
 */
ComPtr<ID2D1PathGeometry> buildPath(Canvas& c, const std::vector<Point>& pts, bool closed, bool filled) {
    if (c.ctx() == nullptr || pts.size() < 2) {
        return nullptr;
    }
    ComPtr<ID2D1Factory> factory;
    c.ctx()->GetFactory(factory.GetAddressOf());
    if (!factory) {
        return nullptr;
    }
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(geometry.GetAddressOf())) || !geometry) {
        return nullptr;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.GetAddressOf())) || !sink) {
        return nullptr;
    }
    // One figure through every point; filled figures are always closed.
    sink->BeginFigure(pts[0].toD2D(), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < pts.size(); ++i) {
        sink->AddLine(pts[i].toD2D());
    }
    sink->EndFigure((closed || filled) ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) {
        return nullptr;
    }
    return geometry;
}

/**
 * @brief Strokes a polyline with round caps/joins.
 */
void polyline(const Glyph& g, std::initializer_list<Point> pts, bool closed = false) {
    std::vector<Point> v(pts);
    if (v.size() == 2) {
        // Two points: the plain line already has round caps.
        g.c.drawLine(v[0], v[1], g.colour, g.stroke);
        return;
    }
    ComPtr<ID2D1PathGeometry> geo = buildPath(g.c, v, closed, false);
    ID2D1SolidColorBrush* b = g.c.brush(g.colour);
    if (!geo || b == nullptr || g.c.ctx() == nullptr) {
        return;
    }
    g.c.ctx()->DrawGeometry(geo.Get(), b, g.stroke, roundStrokeStyle(g.c));
}

/**
 * @brief Fills a polygon.
 */
void polygon(const Glyph& g, std::initializer_list<Point> pts) {
    std::vector<Point> v(pts);
    ComPtr<ID2D1PathGeometry> geo = buildPath(g.c, v, true, true);
    ID2D1SolidColorBrush* b = g.c.brush(g.colour);
    if (!geo || b == nullptr || g.c.ctx() == nullptr) {
        return;
    }
    g.c.ctx()->FillGeometry(geo.Get(), b, nullptr);
}

/**
 * @brief Strokes the almond (eye) outline: two symmetric arcs between the corners.
 */
void almond(const Glyph& g, Point left, Point right, float bulge) {
    if (g.c.ctx() == nullptr) {
        return;
    }
    ComPtr<ID2D1Factory> factory;
    g.c.ctx()->GetFactory(factory.GetAddressOf());
    if (!factory) {
        return;
    }
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(geometry.GetAddressOf())) || !geometry) {
        return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.GetAddressOf())) || !sink) {
        return;
    }
    // The arc radius follows from the chord and the requested bulge (sagitta).
    const float chord = std::max(0.001f, right.x - left.x);
    const float half = chord * 0.5f;
    const float s = std::max(0.001f, bulge);
    const float radius = (half * half + s * s) / (2.0f * s);
    sink->BeginFigure(left.toD2D(), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(right.toD2D(), D2D1::SizeF(radius, radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(left.toD2D(), D2D1::SizeF(radius, radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) {
        return;
    }
    ID2D1SolidColorBrush* b = g.c.brush(g.colour);
    if (b == nullptr) {
        return;
    }
    g.c.ctx()->DrawGeometry(geometry.Get(), b, g.stroke, nullptr);
}

/**
 * @brief Arrow head: two barbs trailing behind the tip along the travel direction.
 */
void arrowHead(const Glyph& g, Point tip, float dirX, float dirY, float length) {
    const float mag = std::sqrt(dirX * dirX + dirY * dirY);
    if (mag <= 0.0001f) {
        return;
    }
    const float ux = dirX / mag;
    const float uy = dirY / mag;
    // Rotate the reversed direction by +-35 degrees for the two barbs.
    const float a = 35.0f * kPi / 180.0f;
    const float ca = std::cos(a);
    const float sa = std::sin(a);
    const Point b1 = {tip.x - (ux * ca - uy * sa) * length, tip.y - (ux * sa + uy * ca) * length};
    const Point b2 = {tip.x - (ux * ca + uy * sa) * length, tip.y - (-ux * sa + uy * ca) * length};
    polyline(g, {b1, tip, b2});
}

/**
 * @brief Circle outline with the glyph stroke.
 */
void ring(const Glyph& g, Point center, float radius) {
    g.c.strokeCircle(center, radius, g.colour, g.stroke);
}

// ---- individual icons --------------------------------------------------------

void drawClose(const Glyph& g) {
    polyline(g, {g.p(0.08f, 0.08f), g.p(0.92f, 0.92f)});
    polyline(g, {g.p(0.92f, 0.08f), g.p(0.08f, 0.92f)});
}

void drawMinimize(const Glyph& g) {
    polyline(g, {g.p(0.05f, 0.5f), g.p(0.95f, 0.5f)});
}

void drawMaximize(const Glyph& g) {
    g.c.strokeRoundedRect(g.r(0.05f, 0.05f, 0.9f, 0.9f), g.len(0.14f), g.colour, g.stroke);
}

void drawRestore(const Glyph& g) {
    // Back window: only the edges not hidden by the front one.
    polyline(g, {g.p(0.3f, 0.3f), g.p(0.3f, 0.02f), g.p(0.98f, 0.02f), g.p(0.98f, 0.7f), g.p(0.7f, 0.7f)});
    g.c.strokeRoundedRect(g.r(0.02f, 0.3f, 0.68f, 0.68f), g.len(0.1f), g.colour, g.stroke);
}

void drawChevronDown(const Glyph& g) {
    polyline(g, {g.p(0.18f, 0.34f), g.p(0.5f, 0.66f), g.p(0.82f, 0.34f)});
}

void drawChevronRight(const Glyph& g) {
    polyline(g, {g.p(0.34f, 0.18f), g.p(0.66f, 0.5f), g.p(0.34f, 0.82f)});
}

void drawCheck(const Glyph& g) {
    polyline(g, {g.p(0.12f, 0.55f), g.p(0.4f, 0.82f), g.p(0.9f, 0.22f)});
}

void drawPlay(const Glyph& g) {
    polygon(g, {g.p(0.22f, 0.08f), g.p(0.92f, 0.5f), g.p(0.22f, 0.92f)});
}

void drawPause(const Glyph& g) {
    const float rad = g.len(0.06f);
    g.c.fillRoundedRect(g.r(0.18f, 0.08f, 0.24f, 0.84f), rad, g.colour);
    g.c.fillRoundedRect(g.r(0.58f, 0.08f, 0.24f, 0.84f), rad, g.colour);
}

void drawStop(const Glyph& g) {
    g.c.fillRoundedRect(g.r(0.14f, 0.14f, 0.72f, 0.72f), g.len(0.12f), g.colour);
}

void drawRefresh(const Glyph& g) {
    // 270 degree arc with the gap at the top-right, arrow at the arc's end.
    const Point center = g.p(0.5f, 0.5f);
    const float radius = g.len(0.4f);
    const float startDeg = 30.0f;
    const float sweep = 270.0f;
    g.c.strokeArc(center, radius, startDeg, sweep, g.colour, g.stroke);
    const float endRad = (startDeg + sweep) * kPi / 180.0f;
    const Point tip = {center.x + radius * std::sin(endRad), center.y - radius * std::cos(endRad)};
    // Clockwise travel direction at the end angle is the derivative of the arc point.
    arrowHead(g, tip, std::cos(endRad), std::sin(endRad), g.len(0.24f));
}

void drawFolder(const Glyph& g) {
    polyline(g, {g.p(0.04f, 0.2f), g.p(0.36f, 0.2f), g.p(0.46f, 0.32f), g.p(0.96f, 0.32f), g.p(0.96f, 0.86f), g.p(0.04f, 0.86f)}, true);
}

void drawEye(const Glyph& g) {
    almond(g, g.p(0.04f, 0.5f), g.p(0.96f, 0.5f), g.len(0.3f));
    ring(g, g.p(0.5f, 0.5f), g.len(0.15f));
}

void drawTrash(const Glyph& g) {
    polyline(g, {g.p(0.08f, 0.22f), g.p(0.92f, 0.22f)});
    polyline(g, {g.p(0.36f, 0.22f), g.p(0.36f, 0.08f), g.p(0.64f, 0.08f), g.p(0.64f, 0.22f)});
    polyline(g, {g.p(0.18f, 0.22f), g.p(0.25f, 0.94f), g.p(0.75f, 0.94f), g.p(0.82f, 0.22f)});
    polyline(g, {g.p(0.41f, 0.4f), g.p(0.43f, 0.78f)});
    polyline(g, {g.p(0.59f, 0.4f), g.p(0.57f, 0.78f)});
}

void drawCopy(const Glyph& g) {
    polyline(g, {g.p(0.34f, 0.3f), g.p(0.34f, 0.04f), g.p(0.96f, 0.04f), g.p(0.96f, 0.66f), g.p(0.7f, 0.66f)});
    g.c.strokeRoundedRect(g.r(0.04f, 0.34f, 0.62f, 0.62f), g.len(0.1f), g.colour, g.stroke);
}

void drawExternalLink(const Glyph& g) {
    polyline(g, {g.p(0.42f, 0.08f), g.p(0.08f, 0.08f), g.p(0.08f, 0.92f), g.p(0.92f, 0.92f), g.p(0.92f, 0.58f)});
    polyline(g, {g.p(0.5f, 0.5f), g.p(0.92f, 0.08f)});
    polyline(g, {g.p(0.6f, 0.08f), g.p(0.92f, 0.08f), g.p(0.92f, 0.4f)});
}

void drawPlus(const Glyph& g) {
    polyline(g, {g.p(0.5f, 0.1f), g.p(0.5f, 0.9f)});
    polyline(g, {g.p(0.1f, 0.5f), g.p(0.9f, 0.5f)});
}

void drawMinus(const Glyph& g) {
    polyline(g, {g.p(0.1f, 0.5f), g.p(0.9f, 0.5f)});
}

void drawWarning(const Glyph& g) {
    polyline(g, {g.p(0.5f, 0.06f), g.p(0.96f, 0.9f), g.p(0.04f, 0.9f)}, true);
    polyline(g, {g.p(0.5f, 0.38f), g.p(0.5f, 0.62f)});
    g.c.fillCircle(g.p(0.5f, 0.77f), g.stroke * 0.62f, g.colour);
}

void drawInfo(const Glyph& g) {
    ring(g, g.p(0.5f, 0.5f), g.len(0.46f));
    g.c.fillCircle(g.p(0.5f, 0.3f), g.stroke * 0.62f, g.colour);
    polyline(g, {g.p(0.5f, 0.45f), g.p(0.5f, 0.72f)});
}

void drawError(const Glyph& g) {
    ring(g, g.p(0.5f, 0.5f), g.len(0.46f));
    polyline(g, {g.p(0.32f, 0.32f), g.p(0.68f, 0.68f)});
    polyline(g, {g.p(0.68f, 0.32f), g.p(0.32f, 0.68f)});
}

void drawSuccess(const Glyph& g) {
    ring(g, g.p(0.5f, 0.5f), g.len(0.46f));
    polyline(g, {g.p(0.28f, 0.52f), g.p(0.44f, 0.68f), g.p(0.72f, 0.36f)});
}

void drawFilmFrame(const Glyph& g) {
    // 3:2 frame centred vertically in the square.
    const float frameH = 2.0f / 3.0f;
    const float top = (1.0f - frameH) * 0.5f;
    g.c.strokeRoundedRect(g.r(0.0f, top, 1.0f, frameH), g.len(0.1f), g.colour, g.stroke);
    // Three sprocket holes down each side.
    const float hole = 0.09f;
    for (int i = 0; i < 3; ++i) {
        const float y = top + frameH * (0.18f + 0.32f * static_cast<float>(i)) - hole * 0.5f;
        g.c.fillRoundedRect(g.r(0.09f, y, hole, hole), g.len(0.02f), g.colour);
        g.c.fillRoundedRect(g.r(1.0f - 0.09f - hole, y, hole, hole), g.len(0.02f), g.colour);
    }
    // A small play triangle in the picture area.
    const float cy = 0.5f;
    polygon(g, {g.p(0.42f, cy - 0.13f), g.p(0.64f, cy), g.p(0.42f, cy + 0.13f)});
}

void drawAppMark(const Glyph& g) {
    // Rounded square with the accent -> PQ purple gradient and a white H.
    const Theme& t = g.c.theme();
    const Rect square = g.r(0.0f, 0.0f, 1.0f, 1.0f);
    g.c.fillLinearGradient(square, t.accent, t.tonePQ, true, g.len(0.24f));
    Glyph h = g;
    h.colour = Color::white();
    h.stroke = std::max(g.stroke, g.len(0.11f));
    polyline(h, {h.p(0.3f, 0.26f), h.p(0.3f, 0.74f)});
    polyline(h, {h.p(0.7f, 0.26f), h.p(0.7f, 0.74f)});
    polyline(h, {h.p(0.3f, 0.5f), h.p(0.7f, 0.5f)});
}

void drawSearch(const Glyph& g) {
    ring(g, g.p(0.42f, 0.42f), g.len(0.32f));
    polyline(g, {g.p(0.66f, 0.66f), g.p(0.94f, 0.94f)});
}

void drawGear(const Glyph& g) {
    const Point center = g.p(0.5f, 0.5f);
    ring(g, center, g.len(0.24f));
    // Eight short radial teeth.
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * (kPi / 4.0f);
        const float sx = std::sin(a);
        const float cy = -std::cos(a);
        const Point inner = {center.x + sx * g.len(0.34f), center.y + cy * g.len(0.34f)};
        const Point outer = {center.x + sx * g.len(0.48f), center.y + cy * g.len(0.48f)};
        polyline(g, {inner, outer});
    }
}

void drawBook(const Glyph& g) {
    polyline(g, {g.p(0.5f, 0.9f), g.p(0.06f, 0.8f), g.p(0.06f, 0.1f), g.p(0.5f, 0.2f)});
    polyline(g, {g.p(0.5f, 0.9f), g.p(0.94f, 0.8f), g.p(0.94f, 0.1f), g.p(0.5f, 0.2f)});
    polyline(g, {g.p(0.5f, 0.2f), g.p(0.5f, 0.9f)});
}

void drawDock(const Glyph& g) {
    g.c.strokeRoundedRect(g.r(0.05f, 0.1f, 0.9f, 0.8f), g.len(0.1f), g.colour, g.stroke);
    polyline(g, {g.p(0.05f, 0.62f), g.p(0.95f, 0.62f)});
    g.c.fillRoundedRect(g.r(0.05f, 0.62f, 0.9f, 0.28f), g.len(0.08f), g.colour.scaledAlpha(0.45f));
}

void drawUndock(const Glyph& g) {
    polyline(g, {g.p(0.5f, 0.3f), g.p(0.06f, 0.3f), g.p(0.06f, 0.94f), g.p(0.7f, 0.94f), g.p(0.7f, 0.5f)});
    polyline(g, {g.p(0.06f, 0.46f), g.p(0.7f, 0.46f)});
    polyline(g, {g.p(0.5f, 0.5f), g.p(0.94f, 0.06f)});
    polyline(g, {g.p(0.62f, 0.06f), g.p(0.94f, 0.06f), g.p(0.94f, 0.38f)});
}

void drawPin(const Glyph& g) {
    polyline(g, {g.p(0.3f, 0.08f), g.p(0.7f, 0.08f)});
    polyline(g, {g.p(0.38f, 0.08f), g.p(0.38f, 0.42f), g.p(0.2f, 0.62f), g.p(0.8f, 0.62f), g.p(0.62f, 0.42f), g.p(0.62f, 0.08f)});
    polyline(g, {g.p(0.5f, 0.62f), g.p(0.5f, 0.96f)});
}

} // namespace

/**
 * @brief Dispatches to the per-icon routine (see PathIconsInternal.h).
 */
void drawIconShape(Canvas& c, IconId id, const Rect& bounds, const Color& colour, float stroke) {
    if (c.ctx() == nullptr || id == IconId::None || bounds.isEmpty()) {
        return;
    }
    // The glyph square: 60 % of the shorter side, centred. The app mark is a
    // filled tile rather than a line glyph, so it takes most of its bounds.
    const float side = std::max(0.0f, std::min(bounds.w, bounds.h));
    // Callers pass the box they want the glyph to fill; keep a small optical
    // margin so round strokes never touch the edge. The app mark is a filled
    // tile and may use almost all of it.
    const float fraction = (id == IconId::AppMark) ? 0.9f : 0.85f;
    const float box = side * fraction;
    if (box <= 0.0f) {
        return;
    }
    const Point centre = bounds.center();
    Glyph g{c, Rect{centre.x - box * 0.5f, centre.y - box * 0.5f, box, box}, colour, std::max(0.5f, stroke)};

    switch (id) {
    case IconId::Close: drawClose(g); break;
    case IconId::Minimize: drawMinimize(g); break;
    case IconId::Maximize: drawMaximize(g); break;
    case IconId::Restore: drawRestore(g); break;
    case IconId::ChevronDown: drawChevronDown(g); break;
    case IconId::ChevronRight: drawChevronRight(g); break;
    case IconId::Check: drawCheck(g); break;
    case IconId::Play: drawPlay(g); break;
    case IconId::Pause: drawPause(g); break;
    case IconId::Stop: drawStop(g); break;
    case IconId::Refresh: drawRefresh(g); break;
    case IconId::Folder: drawFolder(g); break;
    case IconId::Eye: drawEye(g); break;
    case IconId::Trash: drawTrash(g); break;
    case IconId::Copy: drawCopy(g); break;
    case IconId::ExternalLink: drawExternalLink(g); break;
    case IconId::Plus: drawPlus(g); break;
    case IconId::Minus: drawMinus(g); break;
    case IconId::Warning: drawWarning(g); break;
    case IconId::Info: drawInfo(g); break;
    case IconId::Error: drawError(g); break;
    case IconId::Success: drawSuccess(g); break;
    case IconId::FilmFrame: drawFilmFrame(g); break;
    case IconId::AppMark: drawAppMark(g); break;
    case IconId::Search: drawSearch(g); break;
    case IconId::Gear: drawGear(g); break;
    case IconId::Book: drawBook(g); break;
    case IconId::Dock: drawDock(g); break;
    case IconId::Undock: drawUndock(g); break;
    case IconId::Pin: drawPin(g); break;
    case IconId::None: default: break;
    }
}

} // namespace hh::ui
