// ---------------------------------------------------------------------------
// Layout.h - the one layout model: every Widget is a stack of its children.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/gfx/Geometry.h"

#include <optional>

namespace hh::ui {

enum class Axis { Vertical, Horizontal };
enum class CrossAlign { Start, Center, End, Stretch };
enum class Justify { Start, Center, End, SpaceBetween };

/**
 * @brief Per-child parameters read by the parent's stack layout.
 */
struct LayoutParams {
    float flexGrow = 0.0f;              ///< share of leftover main-axis space
    std::optional<float> width;         ///< fixed width (dips)
    std::optional<float> height;        ///< fixed height (dips)
    std::optional<float> minWidth;
    std::optional<float> minHeight;
    Insets margin;
    std::optional<CrossAlign> crossAlign; ///< overrides the parent's default
};

/**
 * @brief Container parameters of a Widget.
 */
struct StackParams {
    Axis axis = Axis::Vertical;
    float spacing = 0.0f;
    Insets padding;
    CrossAlign crossAlign = CrossAlign::Stretch;
    Justify justify = Justify::Start;
    bool clipsChildren = false;
    /// When the available main-axis width is below this, a horizontal stack lays out vertically instead.
    float wrapIfNarrowerThan = 0.0f;
};

} // namespace hh::ui
