// ---------------------------------------------------------------------------
// Canvas.h - the per-frame drawing API every widget paints with.
//
// Widgets never hold backend objects; they describe shapes/text/colours
// through this class. All coordinates are dips.
//
//   Windows  Direct2D: a cached solid brush, layouts from the TextCache.
//   macOS    CoreGraphics + CoreText on the view's CGContext (points = dips).
//
// The drawing calls are identical on both platforms; only begin() takes the
// platform's native context and a few Windows-only extras (raw D2D matrices,
// bitmaps, the brush) stay behind _WIN32 for the Win32 window code.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/gfx/Geometry.h"
#include "ui/gfx/PathIcons.h"
#include "ui/gfx/TextCache.h"
#include "ui/gfx/TextStyle.h"
#include "ui/theme/Theme.h"

#if defined(_WIN32)
#include "platform/Handle.h"

#include <d2d1_2.h>
#else
// CGContextRef without pulling CoreGraphics into every widget.
struct CGContext;
#endif

#include <cstddef>
#include <string>
#include <vector>

namespace hh::ui {

#if defined(_WIN32)
/// The context a frame draws into.
using NativeDrawContext = ID2D1DeviceContext1*;
#else
/// The context a frame draws into (a CGContextRef).
using NativeDrawContext = CGContext*;
#endif

class Canvas {
public:
    Canvas() = default;
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    /// Starts a frame on a context that is ready to draw (inside BeginDraw on Windows).
    void begin(NativeDrawContext ctx, const DipScale& scale, const Theme& theme, TextCache& text);
    /// Pops any leftover layers/clips (defensive) and forgets the context.
    void end();

    /// The native context of the current frame (nullptr outside begin()/end()).
    [[nodiscard]] NativeDrawContext ctx() const noexcept;
    [[nodiscard]] const DipScale& scale() const noexcept { return scale_; }
    [[nodiscard]] const Theme& theme() const noexcept { return *theme_; }
    [[nodiscard]] TextCache& text() const noexcept { return *text_; }

    // ---- shapes -------------------------------------------------------------
    void fillRect(const Rect& r, const Color& c);
    void fillRoundedRect(const Rect& r, float radius, const Color& c);
    void strokeRect(const Rect& r, const Color& c, float width = 0.0f);              ///< width 0 = hairline
    void strokeRoundedRect(const Rect& r, float radius, const Color& c, float width = 0.0f);
    void drawLine(Point a, Point b, const Color& c, float width = 0.0f);
    /// One physical pixel line snapped to the pixel grid (separators).
    void drawHairline(Point a, Point b, const Color& c);
    void fillCircle(Point center, float radius, const Color& c);
    void strokeCircle(Point center, float radius, const Color& c, float width = 0.0f);
    void fillEllipse(const Rect& bounds, const Color& c);
    /// Arc of a circle (degrees, clockwise from 12 o'clock) - spinners.
    void strokeArc(Point center, float radius, float startDeg, float sweepDeg, const Color& c, float width);
    void fillLinearGradient(const Rect& r, const Color& from, const Color& to, bool vertical, float radius = 0.0f);
    /// Soft shadow under a rounded rect (stacked translucent rings, no effects).
    void drawShadow(const Rect& r, float radius, float blur, const Color& c, float offsetY = 2.0f);
    /// Focus ring: 2 dip stroke just outside the rect.
    void drawFocusRing(const Rect& r, float radius, const Color& c);

    // ---- paths (icons) --------------------------------------------------------
    /// Polyline through @p count points with round caps and joins (closed = back to the start).
    void strokePolyline(const Point* points, size_t count, bool closed, const Color& c, float width);
    /// Filled polygon through @p count points.
    void fillPolygon(const Point* points, size_t count, const Color& c);
    /// The almond / eye outline: two symmetric arcs between @p left and @p right, @p bulge dips high.
    void strokeLens(Point left, Point right, float bulge, const Color& c, float width);

    // ---- text ---------------------------------------------------------------
    /// Draws text inside @p bounds with alignment and trimming. Returns the laid-out size.
    Size drawText(std::wstring_view text, const TextStyle& style, const Rect& bounds, const Color& c,
                  HAlign h = HAlign::Left, VAlign v = VAlign::Center, Trimming trimming = Trimming::End, int maxLines = 1);
    Size measureText(std::wstring_view text, const TextStyle& style, float maxWidth = 0.0f, int maxLines = 1);
    /// Draws a layout from the TextCache with its top-left corner at @p origin.
    void drawTextLayout(const TextLayoutRef& layout, Point origin, const Color& c);

    // ---- icons --------------------------------------------------------------
    /// Draws a vector icon centred in @p bounds with the given stroke.
    void drawIcon(IconId id, const Rect& bounds, const Color& c, float strokeWidth = 1.5f);

    // ---- state stack --------------------------------------------------------
    void pushClip(const Rect& r);
    void pushRoundedClip(const Rect& r, float radius);
    void pushOpacity(float opacity);
    /// Multiplied onto the current transform (local space first).
    void pushTransform(const Transform2D& m);
    void pop();

    /// Drops device-dependent objects (device loss).
    void resetDeviceResources();

#if defined(_WIN32)
    // ---- Direct2D extras for the Win32 window code --------------------------
    void drawTextLayout(IDWriteTextLayout* layout, Point origin, const Color& c);
    void drawBitmap(ID2D1Bitmap* bitmap, const Rect& dest, float opacity = 1.0f);
    void pushTransform(const D2D1_MATRIX_3X2_F& m);
    /// Cached solid brush set to @p c.
    ID2D1SolidColorBrush* brush(const Color& c);
#endif

private:
    DipScale scale_;
    const Theme* theme_ = nullptr;
    TextCache* text_ = nullptr;

#if defined(_WIN32)
    enum class StackKind { Clip, Layer, Transform };
    struct StackEntry { StackKind kind; D2D1_MATRIX_3X2_F savedTransform{}; };

    ComPtr<ID2D1DeviceContext1> ctx_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> roundStroke_;
    std::vector<StackEntry> stack_;
    uint32_t brushGeneration_ = 0;
#else
    /// Every push is a CGContextSaveGState (opacity pushes add a transparency layer).
    enum class StackKind { State, Layer };
    CGContext* ctx_ = nullptr;
    std::vector<StackKind> stack_;
#endif
};

} // namespace hh::ui
