// ---------------------------------------------------------------------------
// TextMeasure.cpp - shared measurement helpers (see TextMeasure.h).
// ---------------------------------------------------------------------------
#include "ui/gfx/TextMeasure.h"

#include <algorithm>

namespace hh::ui {

namespace {

// The one cache registered by the window. Plain pointer: the window owns the
// cache and clears this before it destroys it.
TextCache* g_shared = nullptr;

// Estimated glyph metrics for Segoe UI when no DirectWrite cache is around.
// The average advance of Segoe UI at 14 dip is roughly 0.55 em; the ascent
// and descent are the real design values (2210 / 514 over 2048 units).
constexpr float kAvgAdvanceEm = 0.55f;
constexpr float kAscentEm = 1.079f;
constexpr float kDescentEm = 0.251f;

} // namespace

/**
 * @brief Returns the registered shared cache (nullptr when none).
 */
TextCache* sharedTextCache() {
    return g_shared;
}

/**
 * @brief Registers the cache every widget measures with.
 */
void setSharedTextCache(TextCache* cache) {
    g_shared = cache;
}

/**
 * @brief Measures text through the shared cache, or estimates when it is absent.
 */
Size measureTextShared(std::wstring_view text, const TextStyle& style, float maxWidth, int maxLines) {
    // Sizes below one dip make no sense and would only produce degenerate rects.
    const float size = std::max(1.0f, style.size);

    // The real thing when the window has registered its DirectWrite cache.
    if (g_shared != nullptr && g_shared->factory() != nullptr) {
        return g_shared->measure(text, style, maxWidth, maxLines);
    }

    // Estimate: every character advances by a fixed fraction of the em, plus
    // the configured letter spacing; the width is capped at maxWidth when
    // one was given (the text would wrap or trim there).
    const float chars = static_cast<float>(text.size());
    float width = chars * (size * kAvgAdvanceEm + std::max(0.0f, style.letterSpacing));
    if (maxWidth > 0.0f) {
        width = std::min(width, maxWidth);
    }

    // Height follows the line count (wrapped text may need several lines,
    // but without a layout engine the cap is the best estimate we have).
    const int lines = std::max(1, maxLines);
    const float lineHeight = (style.lineHeight > 0.0f) ? style.lineHeight : 1.3f * size;
    return {width, lineHeight * static_cast<float>(lines)};
}

/**
 * @brief Line metrics through the shared cache, or Segoe UI design values.
 */
TextCache::LineMetrics lineMetricsShared(const TextStyle& style) {
    if (g_shared != nullptr && g_shared->factory() != nullptr) {
        return g_shared->lineMetrics(style);
    }

    // Fallback from the design metrics of Segoe UI scaled by the size.
    const float size = std::max(1.0f, style.size);
    TextCache::LineMetrics m;
    m.ascent = kAscentEm * size;
    m.descent = kDescentEm * size;
    m.lineHeight = (style.lineHeight > 0.0f) ? style.lineHeight : std::max(1.3f * size, m.ascent + m.descent);

    // Centre the glyph box inside the line, exactly as TextCache does.
    m.baseline = m.ascent + (m.lineHeight - (m.ascent + m.descent)) * 0.5f;
    return m;
}

} // namespace hh::ui
