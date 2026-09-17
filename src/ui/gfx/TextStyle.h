// ---------------------------------------------------------------------------
// TextStyle.h - typography tokens.
//
// Sizes are dips. Body is 14 (Segoe UI Variable's x-height is smaller than
// SF Pro's, so Apple's 13 reads too small on Windows).
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <dwrite_3.h>

#include <cstdint>
#include <string>

namespace hh::ui {

enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Center, Bottom };
enum class Trimming { None, End, Middle };

struct TextStyle {
    enum class Family { Text, Display, Small, Mono };

    Family family = Family::Text;
    float size = 14.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    bool italic = false;
    float lineHeight = 0.0f;        ///< 0 = automatic (1.3 x size)
    float letterSpacing = 0.0f;     ///< dips added between characters
    bool uppercase = false;         ///< transform text before layout
    bool wrap = false;              ///< word wrap (else single line)

    /// Key for caching formats (all fields that affect the IDWriteTextFormat).
    [[nodiscard]] uint64_t key() const noexcept;
    bool operator==(const TextStyle& o) const noexcept {
        return family == o.family && size == o.size && weight == o.weight && italic == o.italic &&
               lineHeight == o.lineHeight && letterSpacing == o.letterSpacing && uppercase == o.uppercase && wrap == o.wrap;
    }
};

/// Named styles (Apple HIG hierarchy mapped to Windows metrics).
namespace typography {
inline TextStyle largeTitle() { return {TextStyle::Family::Display, 22.0f, DWRITE_FONT_WEIGHT_BOLD}; }
inline TextStyle title() { return {TextStyle::Family::Display, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}; }
inline TextStyle headline() { return {TextStyle::Family::Text, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}; }
inline TextStyle body() { return {TextStyle::Family::Text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL}; }
inline TextStyle bodyStrong() { return {TextStyle::Family::Text, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}; }
inline TextStyle callout() { return {TextStyle::Family::Text, 13.0f, DWRITE_FONT_WEIGHT_NORMAL}; }
inline TextStyle calloutStrong() { return {TextStyle::Family::Text, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}; }
inline TextStyle caption() { return {TextStyle::Family::Small, 12.0f, DWRITE_FONT_WEIGHT_NORMAL}; }
inline TextStyle captionStrong() { return {TextStyle::Family::Small, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}; }
inline TextStyle chip() { TextStyle s{TextStyle::Family::Small, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}; s.letterSpacing = 0.4f; s.uppercase = true; return s; }
inline TextStyle mono() { return {TextStyle::Family::Mono, 12.0f, DWRITE_FONT_WEIGHT_NORMAL}; }
inline TextStyle bodyWrap() { TextStyle s = body(); s.wrap = true; return s; }
inline TextStyle calloutWrap() { TextStyle s = callout(); s.wrap = true; return s; }
} // namespace typography

} // namespace hh::ui
