// ---------------------------------------------------------------------------
// PathIconsInternal.h - the icon rasteriser Canvas::drawIcon delegates to.
//
// Kept out of PathIcons.h so widgets only see the IconId enum; the drawing
// routine needs the full Canvas.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/gfx/Canvas.h"
#include "ui/gfx/Geometry.h"
#include "ui/gfx/PathIcons.h"

namespace hh::ui {

/**
 * @brief Draws one icon centred inside @p bounds.
 *
 * Every glyph lives in a square of min(w, h) * 0.6 centred in the bounds so
 * icons of the same nominal size line up optically. Strokes use the given
 * width with round caps and joins; fills use @p colour directly.
 *
 * @param c       the frame's canvas (must be inside begin()/end())
 * @param id      which icon; IconId::None draws nothing
 * @param bounds  the icon's box in the current local space (dips)
 * @param colour  stroke/fill colour
 * @param stroke  stroke width in dips (>= 0.5)
 */
void drawIconShape(Canvas& c, IconId id, const Rect& bounds, const Color& colour, float stroke);

} // namespace hh::ui
