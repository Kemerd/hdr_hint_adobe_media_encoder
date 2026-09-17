// ---------------------------------------------------------------------------
// TextCache.h - DirectWrite formats and layouts, cached.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Handle.h"
#include "ui/gfx/Geometry.h"
#include "ui/gfx/TextStyle.h"

#include <dwrite_3.h>

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

namespace hh::ui {

using platform::ComPtr;

/**
 * @brief Resolves font families once, caches IDWriteTextFormat per style and
 *        a bounded LRU of IDWriteTextLayout per (text, style, width, lines).
 *        DirectWrite objects are device-independent: they survive device loss.
 */
class TextCache {
public:
    /// Resolves family names ("Segoe UI Variable Text" -> "Segoe UI" fallback, "Cascadia Mono" -> "Consolas").
    void init(IDWriteFactory3* factory);
    void shutdown();

    [[nodiscard]] IDWriteFactory3* factory() const noexcept { return factory_.Get(); }
    /// The resolved family name for a style family.
    [[nodiscard]] const std::wstring& familyName(TextStyle::Family f) const;

    /// Cached format for a style (never null after init()).
    IDWriteTextFormat* format(const TextStyle& style);

    /**
     * @brief Cached layout. maxWidth <= 0 means unbounded (single line).
     * @param maxLines  0 = unlimited; otherwise trimming applies at the last line
     */
    ComPtr<IDWriteTextLayout> layout(std::wstring_view text, const TextStyle& style, float maxWidth,
                                     Trimming trimming = Trimming::End, int maxLines = 1);

    /// Measures text (width/height in dips) with the same rules as layout().
    Size measure(std::wstring_view text, const TextStyle& style, float maxWidth = 0.0f, int maxLines = 1);

    /// Baseline-to-top distance and full line height for a style (from real font metrics).
    struct LineMetrics { float ascent = 0; float descent = 0; float lineHeight = 0; float baseline = 0; };
    LineMetrics lineMetrics(const TextStyle& style);

    /// Applies middle-ellipsis manually (DirectWrite only trims at the end).
    std::wstring ellipsizeMiddle(std::wstring_view text, const TextStyle& style, float maxWidth);

    /// Drops every cached layout (fonts changed / theme font switch).
    void clearLayouts();

private:
    struct LayoutKey {
        std::wstring text; uint64_t style = 0; float width = 0; int lines = 0; Trimming trim = Trimming::End;
        bool operator==(const LayoutKey& o) const { return style == o.style && width == o.width && lines == o.lines && trim == o.trim && text == o.text; }
    };
    struct LayoutKeyHash { size_t operator()(const LayoutKey& k) const noexcept; };
    struct LayoutEntry { ComPtr<IDWriteTextLayout> layout; std::list<LayoutKey>::iterator lru; };

    ComPtr<IDWriteFactory3> factory_;
    std::wstring families_[4];
    std::unordered_map<uint64_t, ComPtr<IDWriteTextFormat>> formats_;
    std::unordered_map<LayoutKey, LayoutEntry, LayoutKeyHash> layouts_;
    std::list<LayoutKey> lru_;
    std::unordered_map<uint64_t, LineMetrics> metrics_;
    size_t maxLayouts_ = 512;
};

} // namespace hh::ui
