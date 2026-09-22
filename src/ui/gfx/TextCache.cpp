// ---------------------------------------------------------------------------
// TextCache.cpp - DirectWrite formats, layouts and font metrics, cached.
// ---------------------------------------------------------------------------
#include "ui/gfx/TextCache.h"

#include "core/Logger.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Text";

// DirectWrite layouts need a finite box; this is "unbounded" for our purposes.
constexpr float kUnbounded = 1.0e6f;

// Segoe UI design metrics used when the font face cannot be queried.
constexpr float kFallbackAscentEm = 1.079f;
constexpr float kFallbackDescentEm = 0.251f;

/**
 * @brief Preferred and fallback family names per TextStyle::Family, in enum order.
 */
struct FamilyChoice {
    const wchar_t* preferred;
    const wchar_t* fallback;
};
constexpr FamilyChoice kFamilyChoices[4] = {
    {L"Segoe UI Variable Text", L"Segoe UI"},      // Family::Text
    {L"Segoe UI Variable Display", L"Segoe UI"},   // Family::Display
    {L"Segoe UI Variable Small", L"Segoe UI"},     // Family::Small
    {L"Cascadia Mono", L"Consolas"},               // Family::Mono
};

/**
 * @brief Folds a hash into another (boost-style combine).
 */
size_t hashCombine(size_t seed, size_t value) noexcept {
    return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
}

/**
 * @brief Upper-cases a string with the invariant locale (handles surrogates).
 */
std::wstring toUpperInvariant(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(std::min<size_t>(text.size(), static_cast<size_t>(INT32_MAX)));
    // LCMapStringEx may expand some characters; ask for the required size first.
    const int needed = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text.data(), length, nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0) {
        return std::wstring(text);
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text.data(), length, out.data(), needed, nullptr, nullptr, 0);
    if (written <= 0) {
        return std::wstring(text);
    }
    out.resize(static_cast<size_t>(written));
    return out;
}

/**
 * @brief True when the code unit is a high surrogate (first half of a pair).
 */
bool isHighSurrogate(wchar_t c) noexcept {
    return c >= 0xD800 && c <= 0xDBFF;
}

/**
 * @brief True when the code unit is a low surrogate (second half of a pair).
 */
bool isLowSurrogate(wchar_t c) noexcept {
    return c >= 0xDC00 && c <= 0xDFFF;
}

} // namespace

/**
 * @brief Hashes the layout key (text, style, width, line cap, trimming).
 */
size_t TextCache::LayoutKeyHash::operator()(const LayoutKey& k) const noexcept {
    size_t h = std::hash<std::wstring>{}(k.text);
    h = hashCombine(h, std::hash<uint64_t>{}(k.style));
    h = hashCombine(h, std::hash<uint32_t>{}(std::bit_cast<uint32_t>(k.width)));
    h = hashCombine(h, std::hash<int>{}(k.lines));
    h = hashCombine(h, std::hash<int>{}(static_cast<int>(k.trim)));
    return h;
}

/**
 * @brief Stores the factory and resolves the four family names once.
 */
void TextCache::init(IDWriteFactory3* factory) {
    shutdown();
    if (factory == nullptr) {
        HH_LOG_ERROR(kLog, L"init: null DirectWrite factory");
        return;
    }
    factory_ = factory;

    // Installed fonts decide between the variable family and the classic one.
    ComPtr<IDWriteFontCollection> collection;
    HRESULT hr = factory_->GetSystemFontCollection(collection.GetAddressOf(), FALSE);
    if (FAILED(hr) || !collection) {
        HH_LOG_WARN(kLog, L"GetSystemFontCollection failed (hr=0x{:08X}); using fallback families", static_cast<uint32_t>(hr));
    }

    for (size_t i = 0; i < 4; ++i) {
        const FamilyChoice& choice = kFamilyChoices[i];
        std::wstring resolved = choice.fallback;
        if (collection) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            hr = collection->FindFamilyName(choice.preferred, &index, &exists);
            if (SUCCEEDED(hr) && exists) {
                resolved = choice.preferred;
            } else {
                // Verify the fallback too; if even that is absent DirectWrite
                // will substitute on its own, so we just log it.
                hr = collection->FindFamilyName(choice.fallback, &index, &exists);
                if (FAILED(hr) || !exists) {
                    HH_LOG_WARN(kLog, L"neither '{}' nor '{}' is installed; DirectWrite will substitute", choice.preferred, choice.fallback);
                }
            }
        }
        families_[i] = resolved;
    }
    HH_LOG_DEBUG(kLog, L"font families: text='{}' display='{}' small='{}' mono='{}'", families_[0], families_[1], families_[2], families_[3]);
}

/**
 * @brief Drops every cached object and the factory reference.
 */
void TextCache::shutdown() {
    layouts_.clear();
    lru_.clear();
    formats_.clear();
    metrics_.clear();
    for (std::wstring& f : families_) {
        f.clear();
    }
    factory_.Reset();
}

/**
 * @brief The resolved family name for a style family (empty before init()).
 */
const std::wstring& TextCache::familyName(TextStyle::Family f) const {
    static const std::wstring kEmpty;
    const size_t index = static_cast<size_t>(f);
    if (index >= 4) {
        return kEmpty;
    }
    return families_[index];
}

/**
 * @brief Creates (once) the IDWriteTextFormat for a style.
 */
IDWriteTextFormat* TextCache::format(const TextStyle& style) {
    if (!factory_) {
        return nullptr;
    }
    const uint64_t key = style.key();
    auto it = formats_.find(key);
    if (it != formats_.end() && it->second) {
        return it->second.Get();
    }

    // Family, weight, style and size come straight from the tokens.
    const std::wstring& family = familyName(style.family);
    const float size = std::max(1.0f, style.size);
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = factory_->CreateTextFormat(family.empty() ? L"Segoe UI" : family.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(style.weight),
                                            style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", fmt.GetAddressOf());
    if (FAILED(hr) || !fmt) {
        HH_LOG_ERROR(kLog, L"CreateTextFormat('{}', {}) failed (hr=0x{:08X})", family, size, static_cast<uint32_t>(hr));
        return nullptr;
    }

    // Wrapping is a style decision; alignment is always top-left and the
    // canvas positions the layout for centre/right/bottom alignment.
    fmt->SetWordWrapping(style.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    // Uniform line spacing keeps every label the same height regardless of
    // which fallback font a particular glyph came from.
    const LineMetrics lm = lineMetrics(style);
    fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lm.lineHeight, lm.baseline);

    IDWriteTextFormat* raw = fmt.Get();
    formats_[key] = std::move(fmt);
    return raw;
}

/**
 * @brief Returns the cached layout for the parameters, creating it on a miss.
 */
ComPtr<IDWriteTextLayout> TextCache::layout(std::wstring_view text, const TextStyle& style, float maxWidth,
                                            Trimming trimming, int maxLines) {
    if (!factory_) {
        return nullptr;
    }
    LayoutKey key;
    key.text.assign(text.data(), text.size());
    key.style = style.key();
    key.width = (maxWidth > 0.0f) ? maxWidth : 0.0f;
    key.lines = std::max(0, maxLines);
    key.trim = trimming;

    // Cache hit: bump to the front of the LRU list.
    auto found = layouts_.find(key);
    if (found != layouts_.end()) {
        if (found->second.lru != lru_.begin()) {
            lru_.splice(lru_.begin(), lru_, found->second.lru);
        }
        return found->second.layout;
    }

    IDWriteTextFormat* fmt = format(style);
    if (fmt == nullptr) {
        return nullptr;
    }

    // Apply the uppercase transform before DirectWrite sees the text.
    const std::wstring shaped = style.uppercase ? toUpperInvariant(text) : std::wstring(text);
    const float layoutWidth = (key.width > 0.0f) ? key.width : kUnbounded;
    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = factory_->CreateTextLayout(shaped.c_str(), static_cast<UINT32>(shaped.size()), fmt, layoutWidth, kUnbounded,
                                            layout.GetAddressOf());
    if (FAILED(hr) || !layout) {
        HH_LOG_ERROR(kLog, L"CreateTextLayout failed (hr=0x{:08X}) for {} chars", static_cast<uint32_t>(hr), shaped.size());
        return nullptr;
    }

    // A single line never wraps; a capped multi-line layout stops at the cap
    // (trimming then applies on the last visible line).
    if (key.lines == 1) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (key.lines > 0) {
        const LineMetrics lm = lineMetrics(style);
        layout->SetMaxHeight(lm.lineHeight * static_cast<float>(key.lines));
    }

    // DirectWrite only trims at the end; Middle is pre-shaped by
    // ellipsizeMiddle() and gets the same end trimming as a safety net.
    if (trimming != Trimming::None) {
        DWRITE_TRIMMING trim = {};
        trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        trim.delimiter = 0;
        trim.delimiterCount = 0;
        ComPtr<IDWriteInlineObject> sign;
        hr = factory_->CreateEllipsisTrimmingSign(layout.Get(), sign.GetAddressOf());
        if (FAILED(hr)) {
            HH_LOG_WARN(kLog, L"CreateEllipsisTrimmingSign failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
            sign.Reset();
        }
        layout->SetTrimming(&trim, sign.Get());
    }

    // Tracking (letter spacing) lives on IDWriteTextLayout1.
    if (style.letterSpacing != 0.0f && !shaped.empty()) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(layout.As(&layout1)) && layout1) {
            const DWRITE_TEXT_RANGE range = {0, static_cast<UINT32>(shaped.size())};
            layout1->SetCharacterSpacing(0.0f, style.letterSpacing, 0.0f, range);
        }
    }

    // Insert at the front of the LRU and evict from the back when over budget.
    lru_.push_front(key);
    LayoutEntry entry;
    entry.layout = layout;
    entry.lru = lru_.begin();
    layouts_.emplace(std::move(key), std::move(entry));
    while (layouts_.size() > maxLayouts_ && !lru_.empty()) {
        layouts_.erase(lru_.back());
        lru_.pop_back();
    }
    return layout;
}

/**
 * @brief Width/height of the laid-out text in dips.
 */
Size TextCache::measure(std::wstring_view text, const TextStyle& style, float maxWidth, int maxLines) {
    ComPtr<IDWriteTextLayout> l = layout(text, style, maxWidth, Trimming::End, maxLines);
    if (!l) {
        // No factory: mirror the estimate the shared helpers use.
        const float size = std::max(1.0f, style.size);
        float width = static_cast<float>(text.size()) * size * 0.55f;
        if (maxWidth > 0.0f) {
            width = std::min(width, maxWidth);
        }
        return {width, 1.3f * size * static_cast<float>(std::max(1, maxLines))};
    }
    DWRITE_TEXT_METRICS m = {};
    const HRESULT hr = l->GetMetrics(&m);
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"GetMetrics failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return {};
    }
    return {std::max(0.0f, m.widthIncludingTrailingWhitespace), std::max(0.0f, m.height)};
}

/**
 * @brief Ascent/descent/line height/baseline for a style from the real font face.
 */
TextCache::LineMetrics TextCache::lineMetrics(const TextStyle& style) {
    const uint64_t key = style.key();
    auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        return it->second;
    }

    const float size = std::max(1.0f, style.size);
    LineMetrics lm;
    bool resolved = false;

    // Walk collection -> family -> font -> face to read the design metrics.
    if (factory_) {
        ComPtr<IDWriteFontCollection> collection;
        HRESULT hr = factory_->GetSystemFontCollection(collection.GetAddressOf(), FALSE);
        const std::wstring& family = familyName(style.family);
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(hr) && collection && !family.empty()) {
            hr = collection->FindFamilyName(family.c_str(), &index, &exists);
        }
        ComPtr<IDWriteFontFamily> fontFamily;
        if (SUCCEEDED(hr) && collection && exists) {
            hr = collection->GetFontFamily(index, fontFamily.GetAddressOf());
        }
        ComPtr<IDWriteFont> font;
        if (SUCCEEDED(hr) && fontFamily) {
            hr = fontFamily->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(style.weight), DWRITE_FONT_STRETCH_NORMAL,
                                                  style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                                  font.GetAddressOf());
        }
        ComPtr<IDWriteFontFace> face;
        if (SUCCEEDED(hr) && font) {
            hr = font->CreateFontFace(face.GetAddressOf());
        }
        if (SUCCEEDED(hr) && face) {
            DWRITE_FONT_METRICS fm = {};
            face->GetMetrics(&fm);
            if (fm.designUnitsPerEm > 0) {
                const float scale = size / static_cast<float>(fm.designUnitsPerEm);
                lm.ascent = static_cast<float>(fm.ascent) * scale;
                lm.descent = static_cast<float>(fm.descent) * scale;
                const float lineGap = static_cast<float>(fm.lineGap) * scale;
                lm.lineHeight = std::max(1.3f * size, lm.ascent + lm.descent + lineGap);
                resolved = true;
            }
        }
    }

    // Segoe UI design values keep labels sane when the face is unavailable.
    if (!resolved) {
        lm.ascent = kFallbackAscentEm * size;
        lm.descent = kFallbackDescentEm * size;
        lm.lineHeight = std::max(1.3f * size, lm.ascent + lm.descent);
    }

    // An explicit style line height wins; the baseline centres the glyph box.
    if (style.lineHeight > 0.0f) {
        lm.lineHeight = style.lineHeight;
    }
    lm.baseline = lm.ascent + (lm.lineHeight - (lm.ascent + lm.descent)) * 0.5f;

    metrics_[key] = lm;
    return lm;
}

/**
 * @brief Drops characters from the middle until the text fits, keeping the extension.
 */
std::wstring TextCache::ellipsizeMiddle(std::wstring_view text, const TextStyle& style, float maxWidth) {
    std::wstring full(text);
    if (full.empty() || maxWidth <= 0.0f) {
        return full;
    }
    if (measure(full, style, 0.0f, 1).w <= maxWidth) {
        return full;
    }

    // Keep a short file extension intact so "…_v3.mkv" still reads as an mkv.
    std::wstring ext;
    const size_t dot = full.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0 && full.size() - dot <= 8) {
        ext = full.substr(dot);
        full.erase(dot);
    }
    const std::wstring ellipsis = L"…";

    // Builds "head + … + tail + ext" keeping `keep` characters of the base,
    // never splitting a surrogate pair.
    const auto candidate = [&](size_t keep) {
        keep = std::min(keep, full.size());
        size_t headLen = keep / 2;
        size_t tailStart = full.size() - (keep - headLen);
        if (headLen > 0 && headLen < full.size() && isHighSurrogate(full[headLen - 1]) && isLowSurrogate(full[headLen])) {
            --headLen;
        }
        if (tailStart > 0 && tailStart < full.size() && isLowSurrogate(full[tailStart]) && isHighSurrogate(full[tailStart - 1])) {
            ++tailStart;
        }
        tailStart = std::max(tailStart, headLen);
        return full.substr(0, headLen) + ellipsis + full.substr(tailStart) + ext;
    };

    // Binary search for the largest keep count that still fits.
    size_t lo = 0;
    size_t hi = full.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(candidate(mid), style, 0.0f, 1).w <= maxWidth) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return candidate(lo);
}

/**
 * @brief Forgets every layout (formats and metrics survive).
 */
void TextCache::clearLayouts() {
    layouts_.clear();
    lru_.clear();
}

// ---- layout queries ------------------------------------------------------------

/**
 * @brief GetMetrics: width including trailing whitespace, and height.
 */
Size TextCache::layoutSize(const TextLayoutRef& layout) {
    if (!layout) {
        return {};
    }
    DWRITE_TEXT_METRICS m = {};
    if (FAILED(layout->GetMetrics(&m))) {
        return {};
    }
    return {std::max(0.0f, m.widthIncludingTrailingWhitespace), std::max(0.0f, m.height)};
}

/**
 * @brief HitTestTextPosition on the leading edge of @p pos.
 */
float TextCache::caretX(const TextLayoutRef& layout, size_t pos) {
    if (!layout) {
        return 0.0f;
    }
    FLOAT x = 0.0f;
    FLOAT y = 0.0f;
    DWRITE_HIT_TEST_METRICS m{};
    const HRESULT hr = layout->HitTestTextPosition(static_cast<UINT32>(pos), FALSE, &x, &y, &m);
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"HitTestTextPosition({}) failed: 0x{:08X}", pos, static_cast<unsigned>(hr));
        return 0.0f;
    }
    return x;
}

/**
 * @brief HitTestPoint, with the trailing half of a glyph mapping to the next position.
 */
bool TextCache::hitTest(const TextLayoutRef& layout, float x, float y, size_t& position) {
    if (!layout) {
        return false;
    }
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    const HRESULT hr = layout->HitTestPoint(x, y, &trailing, &inside, &m);
    if (FAILED(hr)) {
        HH_LOG_WARN(kLog, L"HitTestPoint failed: 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    position = static_cast<size_t>(m.textPosition) + (trailing ? static_cast<size_t>(m.length) : 0);
    return true;
}

/**
 * @brief HitTestTextRange: one rectangle per glyph run of the range.
 */
bool TextCache::rangeRects(const TextLayoutRef& layout, size_t pos, size_t length, std::vector<Rect>& out) {
    out.clear();
    if (!layout || length == 0) {
        return false;
    }
    UINT32 count = 0;
    HRESULT hr = layout->HitTestTextRange(static_cast<UINT32>(pos), static_cast<UINT32>(length), 0.0f, 0.0f, nullptr, 0, &count);
    if ((hr != E_NOT_SUFFICIENT_BUFFER && FAILED(hr)) || count == 0) {
        return false;
    }
    std::vector<DWRITE_HIT_TEST_METRICS> runs(count);
    hr = layout->HitTestTextRange(static_cast<UINT32>(pos), static_cast<UINT32>(length), 0.0f, 0.0f, runs.data(), count, &count);
    if (FAILED(hr)) {
        return false;
    }
    for (UINT32 i = 0; i < count && i < runs.size(); ++i) {
        const DWRITE_HIT_TEST_METRICS& r = runs[i];
        out.push_back({r.left, r.top, std::max(0.0f, r.width), std::max(0.0f, r.height)});
    }
    return true;
}

} // namespace hh::ui
