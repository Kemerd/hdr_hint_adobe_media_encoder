// ---------------------------------------------------------------------------
// Divider.cpp - hairline separator.
//
// One physical pixel, snapped to the grid by Canvas::drawHairline, inset from
// both ends by the amount given at construction (16 dip for grouped lists).
// ---------------------------------------------------------------------------
#include "ui/controls/Divider.h"

#include "ui/theme/Theme.h"

#include <algorithm>

namespace hh::ui {

namespace {

/// Space reserved for the line in the cross axis (dips). The painted line is
/// always one physical pixel; reserving one dip keeps layout DPI-independent.
constexpr float kThickness = 1.0f;

} // namespace

/**
 * @brief Reserves one dip across the line and nothing along it.
 *
 * The parent stack stretches the divider along its cross axis (the default
 * CrossAlign::Stretch), and tight constraints are honoured through
 * Constraints::constrain(), so the divider fills whatever it is given.
 */
Size Divider::measure(const Constraints& c) {
    const Size natural = vertical_ ? Size{kThickness, 0.0f} : Size{0.0f, kThickness};
    return c.constrain(natural);
}

/**
 * @brief Draws the hairline centred in the frame, inset at both ends.
 */
void Divider::paintSelf(Canvas& c) {
    const Rect b = bounds();
    if (b.isEmpty()) {
        return;
    }
    const Theme& t = c.theme();
    const Color color = strong_ ? t.separatorStrong : t.separator;

    // Never let a large inset flip the line direction; clamp to the frame.
    const float inset = std::max(0.0f, inset_);

    if (vertical_) {
        // Vertical line through the horizontal centre of the frame.
        const float x = b.x + b.w * 0.5f;
        const float y0 = b.y + inset;
        const float y1 = b.bottom() - inset;
        if (y1 <= y0) {
            return;
        }
        c.drawHairline({x, y0}, {x, y1}, color);
        return;
    }

    // Horizontal line through the vertical centre of the frame.
    const float y = b.y + b.h * 0.5f;
    const float x0 = b.x + inset;
    const float x1 = b.right() - inset;
    if (x1 <= x0) {
        return;
    }
    c.drawHairline({x0, y}, {x1, y}, color);
}

} // namespace hh::ui
