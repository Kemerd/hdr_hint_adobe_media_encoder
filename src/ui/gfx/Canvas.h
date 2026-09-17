// ---------------------------------------------------------------------------
// Canvas.h - the per-frame drawing API every widget paints with.
//
// Widgets never hold COM objects; they describe shapes/text/colours through
// this class, which owns a single cached solid brush and re-uses layouts from
// the TextCache. All coordinates are dips.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Handle.h"
#include "ui/gfx/Geometry.h"
#include "ui/gfx/PathIcons.h"
#include "ui/gfx/TextCache.h"
#include "ui/gfx/TextStyle.h"
#include "ui/theme/Theme.h"

#include <d2d1_2.h>

#include <string>
#include <vector>

namespace hh::ui {

class Canvas {
public:
    Canvas() = default;
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    /// Starts a frame on a context that is already inside BeginDraw.
    void begin(ID2D1DeviceContext1* ctx, const DipScale& scale, const Theme& theme, TextCache& text);
    /// Pops any leftover layers/clips (defensive) and forgets the context.
    void end();

    [[nodiscard]] ID2D1DeviceContext1* ctx() const noexcept { return ctx_.Get(); }
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

    // ---- text ---------------------------------------------------------------
    /// Draws text inside @p bounds with alignment and trimming. Returns the laid-out size.
    Size drawText(std::wstring_view text, const TextStyle& style, const Rect& bounds, const Color& c,
                  HAlign h = HAlign::Left, VAlign v = VAlign::Center, Trimming trimming = Trimming::End, int maxLines = 1);
    Size measureText(std::wstring_view text, const TextStyle& style, float maxWidth = 0.0f, int maxLines = 1);
    void drawTextLayout(IDWriteTextLayout* layout, Point origin, const Color& c);

    // ---- icons --------------------------------------------------------------
    /// Draws a vector icon centred in @p bounds with the given stroke.
    void drawIcon(IconId id, const Rect& bounds, const Color& c, float strokeWidth = 1.5f);

    // ---- bitmaps ------------------------------------------------------------
    void drawBitmap(ID2D1Bitmap* bitmap, const Rect& dest, float opacity = 1.0f);

    // ---- state stack --------------------------------------------------------
    void pushClip(const Rect& r);
    void pushRoundedClip(const Rect& r, float radius);
    void pushOpacity(float opacity);
    void pushTransform(const D2D1_MATRIX_3X2_F& m);      ///< multiplied onto the current transform
    void pop();

    /// Cached solid brush set to @p c.
    ID2D1SolidColorBrush* brush(const Color& c);
    /// Drops device-dependent objects (device loss).
    void resetDeviceResources();

private:
    enum class StackKind { Clip, Layer, Transform };
    struct StackEntry { StackKind kind; D2D1_MATRIX_3X2_F savedTransform{}; };

    ComPtr<ID2D1DeviceContext1> ctx_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> roundStroke_;
    DipScale scale_;
    const Theme* theme_ = nullptr;
    TextCache* text_ = nullptr;
    std::vector<StackEntry> stack_;
    uint32_t brushGeneration_ = 0;
};

} // namespace hh::ui
