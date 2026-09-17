// ---------------------------------------------------------------------------
// TextMeasure.h - text measurement without a Canvas.
//
// Widgets measure themselves in Widget::measure(), long before a frame has a
// device context. The window registers its TextCache here once, and every
// text control routes its measurement through these helpers. When no cache is
// registered (unit tests, widgets measured before the window exists) the
// helpers fall back to a conservative estimate so layout never divides by
// zero or produces empty rects.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/gfx/Geometry.h"
#include "ui/gfx/TextCache.h"
#include "ui/gfx/TextStyle.h"

#include <string_view>

namespace hh::ui {

/// The process-wide TextCache used for measuring outside of paint (may be null).
TextCache* sharedTextCache();

/// Registers (or clears with nullptr) the shared measurement cache.
void setSharedTextCache(TextCache* cache);

/**
 * @brief Measures text with the same rules as TextCache::measure().
 * @param maxWidth  0 = unbounded (single line); otherwise wrapping/trimming width
 * @param maxLines  0 = unlimited; otherwise the line cap used for the height
 * @return width/height in dips (estimated when no cache is registered)
 */
Size measureTextShared(std::wstring_view text, const TextStyle& style, float maxWidth = 0.0f, int maxLines = 1);

/// Line metrics (ascent/descent/lineHeight/baseline) for a style, estimated when no cache exists.
TextCache::LineMetrics lineMetricsShared(const TextStyle& style);

} // namespace hh::ui
