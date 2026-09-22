// ---------------------------------------------------------------------------
// InputEvents.h - input records delivered to widgets (coordinates in dips).
//
// Key codes are Win32 virtual keys on every platform: widgets compare
// against VK_RETURN, 'A', VK_OEM_COMMA and friends. The macOS window host
// translates NSEvent key codes into this vocabulary, and maps Command to
// Modifiers::ctrl so Cmd+C / Cmd+V / Cmd+A work through the same shortcuts
// Ctrl+C / Ctrl+V / Ctrl+A do on Windows.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"
#include "ui/gfx/Geometry.h"

#if !defined(_WIN32)
// ---- Win32 virtual-key codes (the subset the toolkit uses, same values) ----
inline constexpr UINT VK_BACK = 0x08;
inline constexpr UINT VK_TAB = 0x09;
inline constexpr UINT VK_RETURN = 0x0D;
inline constexpr UINT VK_SHIFT = 0x10;
inline constexpr UINT VK_CONTROL = 0x11;
inline constexpr UINT VK_MENU = 0x12;       // Alt / Option
inline constexpr UINT VK_ESCAPE = 0x1B;
inline constexpr UINT VK_SPACE = 0x20;
inline constexpr UINT VK_PRIOR = 0x21;      // Page Up
inline constexpr UINT VK_NEXT = 0x22;       // Page Down
inline constexpr UINT VK_END = 0x23;
inline constexpr UINT VK_HOME = 0x24;
inline constexpr UINT VK_LEFT = 0x25;
inline constexpr UINT VK_UP = 0x26;
inline constexpr UINT VK_RIGHT = 0x27;
inline constexpr UINT VK_DOWN = 0x28;
inline constexpr UINT VK_DELETE = 0x2E;     // forward delete
inline constexpr UINT VK_F1 = 0x70;
inline constexpr UINT VK_OEM_COMMA = 0xBC;

/// One wheel notch in WheelEvent::delta units (WM_MOUSEWHEEL's WHEEL_DELTA).
inline constexpr int WHEEL_DELTA = 120;
#endif

namespace hh::ui {

enum class MouseButton { Left, Right, Middle };

struct Modifiers {
    bool ctrl = false;    ///< Ctrl on Windows, Command on macOS (the shortcut key)
    bool shift = false;
    bool alt = false;     ///< Alt on Windows, Option on macOS
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
    UINT vk = 0;                ///< virtual key (Win32 numbering on every platform)
    Modifiers mods;
    bool repeat = false;
};

enum class CursorKind { Arrow, Hand, IBeam, SizeNS, SizeWE, SizeNWSE, SizeNESW, SizeAll, No };

} // namespace hh::ui
