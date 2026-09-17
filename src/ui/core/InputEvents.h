// ---------------------------------------------------------------------------
// InputEvents.h - input records delivered to widgets (coordinates in dips).
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"
#include "ui/gfx/Geometry.h"

namespace hh::ui {

enum class MouseButton { Left, Right, Middle };

struct Modifiers {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    static Modifiers current();     ///< from GetKeyState
};

struct MouseEvent {
    Point pos;            ///< widget-local
    Point rootPos;        ///< root-relative
    MouseButton button = MouseButton::Left;
    Modifiers mods;
    int clickCount = 1;   ///< 2 for double-click
};

struct WheelEvent {
    Point pos;
    Point rootPos;
    float delta = 0.0f;         ///< raw WHEEL_DELTA units (120 per notch), vertical
    float deltaX = 0.0f;        ///< horizontal (WM_MOUSEHWHEEL)
    bool precise = false;       ///< touchpad / precision device
    int linesPerNotch = 3;      ///< SPI_GETWHEELSCROLLLINES (-1 = page)
    Modifiers mods;
};

struct KeyEvent {
    UINT vk = 0;                ///< virtual key
    Modifiers mods;
    bool repeat = false;
};

enum class CursorKind { Arrow, Hand, IBeam, SizeNS, SizeWE, SizeNWSE, SizeNESW, SizeAll, No };

} // namespace hh::ui
