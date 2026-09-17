// ---------------------------------------------------------------------------
// Theme.h - colour tokens (Apple HIG semantic palette, dark + light).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/gfx/Geometry.h"

namespace hh::ui {

struct Theme {
    // surfaces
    Color windowBackground;      ///< opaque fallback / docked paint
    Color elevated;              ///< cards (translucent white/black tint)
    Color elevatedOpaque;        ///< popups, toasts (opaque)
    Color fillSecondary, fillTertiary, fillQuaternary;   ///< control backgrounds
    Color separator, separatorStrong;
    // text
    Color labelPrimary, labelSecondary, labelTertiary, labelQuaternary;
    // semantic
    Color accent, accentText, success, warning, destructive, info;
    // domain
    Color tonePQ, toneHLG, toneSDR;
    // misc
    Color shadow;
    Color focusRing;
    Color scrollThumb;
    bool isDark = true;
    bool translucent = true;     ///< window clear is transparent (Mica shows through)

    /// The two built-in palettes.
    static Theme dark();
    static Theme light();
    /// Opaque docked variant: background = AME's panel colour (or the palette default).
    static Theme docked(const Theme& base, const Color& panelBackground);
    /// Applies a system accent colour (and picks a readable accentText).
    void applyAccent(const Color& accent);
};

} // namespace hh::ui
