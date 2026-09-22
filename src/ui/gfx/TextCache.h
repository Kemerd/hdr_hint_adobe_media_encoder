// ---------------------------------------------------------------------------
// TextCache.h - text formats and layouts, cached.
//
// Windows: DirectWrite formats (IDWriteTextFormat) per style and a bounded
//          LRU of IDWriteTextLayout objects.
// macOS:   CoreText fonts per style and a bounded LRU of MacTextLayout
//          objects (CTLine / CTFrame based, see ui/mac/MacTextLayout.h).
//
// Both backends share the same rules: single-line layouts never wrap, a
// line cap trims at the last visible line, middle ellipsis is pre-shaped by
// ellipsizeMiddle(), letter spacing and upper-casing are applied at layout
// time, and every line has the uniform height lineMetrics() reports.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/gfx/Geometry.h"
#include "ui/gfx/TextStyle.h"

#if defined(_WIN32)
#include "platform/Handle.h"

#include <dwrite_3.h>
#endif

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hh::ui {

#if defined(_WIN32)
using platform::ComPtr;
/// A laid-out block of text (DirectWrite).
using TextLayoutRef = ComPtr<IDWriteTextLayout>;
#else
class MacTextLayout;
/// A laid-out block of text (CoreText, defined in ui/mac/MacTextLayout.h).
using TextLayoutRef = std::shared_ptr<MacTextLayout>;
#endif

/**
 * @brief Resolves font families once, caches a format per style and a
 *        bounded LRU of layouts per (text, style, width, lines).
 *        Text objects are device-independent: they survive device loss.
 */
class TextCache {
public:
#if defined(_WIN32)
    /// Resolves family names ("Segoe UI Variable Text" -> "Segoe UI" fallback, "Cascadia Mono" -> "Consolas").
    void init(IDWriteFactory3* factory);
    [[nodiscard]] IDWriteFactory3* factory() const noexcept { return factory_.Get(); }
    /// Cached format for a style (never null after init()).
    IDWriteTextFormat* format(const TextStyle& style);
#else
    /// Resolves the system fonts (SF Pro Text / Display, SF Mono -> Menlo).
    void init();
#endif
    void shutdown();
    /// True once init() succeeded, i.e. real layouts can be built.
#if defined(_WIN32)
    [[nodiscard]] bool ready() const noexcept { return factory_ != nullptr; }
#else
    [[nodiscard]] bool ready() const noexcept { return initialised_; }
#endif

    /// The resolved family name for a style family.
    [[nodiscard]] const std::wstring& familyName(TextStyle::Family f) const;

    /**
     * @brief Cached layout. maxWidth <= 0 means unbounded (single line).
     * @param maxLines  0 = unlimited; otherwise trimming applies at the last line
     */
    TextLayoutRef layout(std::wstring_view text, const TextStyle& style, float maxWidth,
                         Trimming trimming = Trimming::End, int maxLines = 1);

    /// Measures text (width/height in dips) with the same rules as layout().
    Size measure(std::wstring_view text, const TextStyle& style, float maxWidth = 0.0f, int maxLines = 1);

    /// Baseline-to-top distance and full line height for a style (from real font metrics).
    struct LineMetrics { float ascent = 0; float descent = 0; float lineHeight = 0; float baseline = 0; };
    LineMetrics lineMetrics(const TextStyle& style);

    /// Applies middle-ellipsis manually (the layout engines only trim at the end).
    std::wstring ellipsizeMiddle(std::wstring_view text, const TextStyle& style, float maxWidth);

    /// Drops every cached layout (fonts changed / theme font switch).
    void clearLayouts();

    // ---- layout queries (the text field's caret and selection) ----------------
    //
    // Positions are indices into the string handed to layout(); coordinates
    // are relative to the layout's top-left corner.

    /// Laid-out size: width including trailing whitespace, and height.
    static Size layoutSize(const TextLayoutRef& layout);
    /// X of the caret in front of character @p pos (0 on failure).
    static float caretX(const TextLayoutRef& layout, size_t pos);
    /**
     * @brief Character position nearest to (x, y), trailing half included.
     * @return false when the layout cannot answer (callers then estimate)
     */
    static bool hitTest(const TextLayoutRef& layout, float x, float y, size_t& position);
    /**
     * @brief Highlight rectangles covering [pos, pos + length).
     * @return false when the layout cannot answer (callers then approximate)
     */
    static bool rangeRects(const TextLayoutRef& layout, size_t pos, size_t length, std::vector<Rect>& out);

private:
    struct LayoutKey {
        std::wstring text; uint64_t style = 0; float width = 0; int lines = 0; Trimming trim = Trimming::End;
        bool operator==(const LayoutKey& o) const { return style == o.style && width == o.width && lines == o.lines && trim == o.trim && text == o.text; }
    };
    struct LayoutKeyHash { size_t operator()(const LayoutKey& k) const noexcept; };
    struct LayoutEntry { TextLayoutRef layout; std::list<LayoutKey>::iterator lru; };

#if defined(_WIN32)
    ComPtr<IDWriteFactory3> factory_;
    std::unordered_map<uint64_t, ComPtr<IDWriteTextFormat>> formats_;
#else
    /// Creates the (uncached) layout on a cache miss.
    TextLayoutRef createLayout(std::wstring_view text, const TextStyle& style, float maxWidth, Trimming trimming, int maxLines);
    /// Cached CTFontRef for a style (retained by the cache; never null after init()).
    const void* font(const TextStyle& style);
    bool initialised_ = false;
    std::unordered_map<uint64_t, const void*> fonts_;   ///< CTFontRef per style key (retained)
#endif
    std::wstring families_[4];
    std::unordered_map<LayoutKey, LayoutEntry, LayoutKeyHash> layouts_;
    std::list<LayoutKey> lru_;
    std::unordered_map<uint64_t, LineMetrics> metrics_;
    size_t maxLayouts_ = 512;
};

} // namespace hh::ui
