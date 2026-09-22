// ---------------------------------------------------------------------------
// TextCacheMac.mm - TextCache and MacTextLayout on CoreText.
//
// Fonts: the system UI font (SF Pro Text / Display, picked by size) for the
// Text / Display / Small families and SF Mono for Mono, at the token's
// weight. Layouts follow the DirectWrite rules documented in TextCache.h and
// MacTextLayout.h so every widget measures the same way on both platforms.
// ---------------------------------------------------------------------------
#include "ui/gfx/TextCache.h"

#include "core/Logger.h"
#include "platform/Utf.h"
#include "ui/mac/MacTextLayout.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Text";

/// SF Pro design metrics used when a font refuses to report its own.
constexpr float kFallbackAscentEm = 0.952f;
constexpr float kFallbackDescentEm = 0.241f;

/// "Unbounded" width for the typesetter's line-break search.
constexpr double kUnboundedWidth = 1.0e7;

/// The single-character ellipsis used for truncation.
constexpr UniChar kEllipsis = 0x2026;

/**
 * @brief Folds a hash into another (boost-style combine), as on Windows.
 */
size_t hashCombine(size_t seed, size_t value) noexcept {
    return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
}

/**
 * @brief FontWeight (CSS scale) -> NSFontWeight (-1..1 scale).
 */
NSFontWeight nsWeight(FontWeight w) noexcept {
    const int v = static_cast<int>(w);
    if (v <= 100) { return NSFontWeightUltraLight; }
    if (v <= 200) { return NSFontWeightThin; }
    if (v <= 300) { return NSFontWeightLight; }
    if (v <= 400) { return NSFontWeightRegular; }
    if (v <= 500) { return NSFontWeightMedium; }
    if (v <= 600) { return NSFontWeightSemibold; }
    if (v <= 700) { return NSFontWeightBold; }
    if (v <= 800) { return NSFontWeightHeavy; }
    return NSFontWeightBlack;
}

/**
 * @brief Creates (+1) the CTFont for a style.
 */
CTFontRef createFontFor(const TextStyle& style) {
    @autoreleasepool {
        const CGFloat size = std::max(1.0f, style.size);
        NSFont* font = nil;
        if (style.family == TextStyle::Family::Mono) {
            font = [NSFont monospacedSystemFontOfSize:size weight:nsWeight(style.weight)];
        } else {
            font = [NSFont systemFontOfSize:size weight:nsWeight(style.weight)];
        }
        if (font != nil && style.italic) {
            NSFont* italic = [[NSFontManager sharedFontManager] convertFont:font toHaveTrait:NSItalicFontMask];
            if (italic != nil) {
                font = italic;
            }
        }
        if (font == nil) {
            return CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr);
        }
        // NSFont is toll-free bridged to CTFontRef.
        return static_cast<CTFontRef>(CFBridgingRetain(font));
    }
}

/**
 * @brief Width of a CTLine including trailing whitespace.
 */
float lineWidth(CTLineRef line) noexcept {
    if (line == nullptr) {
        return 0.0f;
    }
    return static_cast<float>(CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr));
}

/**
 * @brief Owns a CF object for the length of a scope.
 */
struct CfHolder {
    CFTypeRef ref = nullptr;
    explicit CfHolder(CFTypeRef r) : ref(r) {}
    ~CfHolder() {
        if (ref != nullptr) {
            CFRelease(ref);
        }
    }
    CfHolder(const CfHolder&) = delete;
    CfHolder& operator=(const CfHolder&) = delete;
};

} // namespace

// ===========================================================================
// MacTextLayout
// ===========================================================================

MacTextLayout::~MacTextLayout() {
    for (Line& l : lines_) {
        if (l.line != nullptr) {
            CFRelease(l.line);
            l.line = nullptr;
        }
    }
}

/**
 * @brief UTF-32 -> CFString, attributes, typesetter, lines, trimming.
 */
bool MacTextLayout::build(std::wstring_view text, CTFontRef font, float letterSpacing, float lineHeight, float baseline,
                          float maxWidth, bool wrap, bool trim, int maxLines) {
    lineHeight_ = std::max(1.0f, lineHeight);
    baseline_ = baseline;
    maxWidth_ = std::max(0.0f, maxWidth);
    if (font == nullptr) {
        return false;
    }

    // ---- 1. UTF-32 -> UTF-16, remembering where every code point starts ------
    std::vector<UniChar> utf16;
    utf16.reserve(text.size() + 8);
    utf16Of_.assign(text.size() + 1, 0);
    for (size_t i = 0; i < text.size(); ++i) {
        utf16Of_[i] = static_cast<CFIndex>(utf16.size());
        uint32_t cp = static_cast<uint32_t>(text[i]);
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            cp = 0xFFFDu;   // never hand CoreText a broken string
        }
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            utf16.push_back(static_cast<UniChar>(0xD800u + (cp >> 10)));
            utf16.push_back(static_cast<UniChar>(0xDC00u + (cp & 0x3FFu)));
        } else {
            utf16.push_back(static_cast<UniChar>(cp));
        }
    }
    utf16Of_[text.size()] = static_cast<CFIndex>(utf16.size());
    const CFIndex total = static_cast<CFIndex>(utf16.size());

    CFStringRef string = CFStringCreateWithCharacters(kCFAllocatorDefault, utf16.data(), total);
    if (string == nullptr) {
        return false;
    }
    CfHolder stringHolder(string);

    // ---- 2. attributes: font, colour-from-context, tracking -------------------
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    if (attrs == nullptr) {
        return false;
    }
    CfHolder attrsHolder(attrs);
    CFDictionarySetValue(attrs, kCTFontAttributeName, font);
    CFDictionarySetValue(attrs, kCTForegroundColorFromContextAttributeName, kCFBooleanTrue);
    if (letterSpacing != 0.0f) {
        const CGFloat kern = letterSpacing;
        CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &kern);
        if (number != nullptr) {
            CFDictionarySetValue(attrs, kCTKernAttributeName, number);
            CFRelease(number);
        }
    }
    CFAttributedStringRef attributed = CFAttributedStringCreate(kCFAllocatorDefault, string, attrs);
    if (attributed == nullptr) {
        return false;
    }
    CfHolder attributedHolder(attributed);

    // The ellipsis line shares the attributes so it matches the text.
    CFStringRef ellipsisString = CFStringCreateWithCharacters(kCFAllocatorDefault, &kEllipsis, 1);
    CfHolder ellipsisStringHolder(ellipsisString);
    CFAttributedStringRef ellipsisAttributed =
        ellipsisString ? CFAttributedStringCreate(kCFAllocatorDefault, ellipsisString, attrs) : nullptr;
    CfHolder ellipsisAttributedHolder(ellipsisAttributed);
    CTLineRef ellipsis = ellipsisAttributed ? CTLineCreateWithAttributedString(ellipsisAttributed) : nullptr;
    CfHolder ellipsisHolder(ellipsis);

    // Truncates a line to @p width with the ellipsis; the input is consumed.
    const auto truncate = [ellipsis](CTLineRef line, double width) -> CTLineRef {
        if (line == nullptr || width <= 0.0) {
            return line;
        }
        CTLineRef cut = CTLineCreateTruncatedLine(line, width, kCTLineTruncationEnd, ellipsis);
        if (cut == nullptr) {
            // Not even the ellipsis fits: show just the ellipsis.
            if (ellipsis != nullptr) {
                CFRelease(line);
                return static_cast<CTLineRef>(CFRetain(ellipsis));
            }
            return line;
        }
        CFRelease(line);
        return cut;
    };

    // ---- 3. line breaking ----------------------------------------------------------
    if (total == 0) {
        // An empty string is still one (empty) line tall, like DirectWrite.
        CTLineRef empty = CTLineCreateWithAttributedString(attributed);
        lines_.push_back(Line{empty, 0, 0, 0.0f});
        width_ = 0.0f;
        return true;
    }
    CTTypesetterRef typesetter = CTTypesetterCreateWithAttributedString(attributed);
    if (typesetter == nullptr) {
        return false;
    }
    CfHolder typesetterHolder(typesetter);

    const bool softWrap = wrap && maxWidth_ > 0.0f;
    const double breakWidth = softWrap ? static_cast<double>(maxWidth_) : kUnboundedWidth;
    CFIndex start = 0;
    while (start < total) {
        CFIndex count = CTTypesetterSuggestLineBreak(typesetter, start, breakWidth);
        if (count <= 0) {
            count = total - start;   // defensive: always make progress
        }
        CTLineRef line = CTTypesetterCreateLine(typesetter, CFRangeMake(start, count));
        lines_.push_back(Line{line, start, count, lineWidth(line)});
        start += count;

        // Line cap reached with text left over: the last visible line takes
        // the rest of the text and is trimmed, as DirectWrite does it.
        if (maxLines > 0 && static_cast<int>(lines_.size()) >= maxLines && start < total) {
            Line& last = lines_.back();
            const double visibleWidth = maxWidth_ > 0.0f ? static_cast<double>(maxWidth_) : static_cast<double>(last.width);
            CTLineRef rest = CTTypesetterCreateLine(typesetter, CFRangeMake(last.start, total - last.start));
            if (rest != nullptr) {
                if (last.line != nullptr) {
                    CFRelease(last.line);
                }
                last.line = trim ? truncate(rest, visibleWidth) : rest;
                last.length = total - last.start;
                last.width = std::min(lineWidth(last.line), static_cast<float>(visibleWidth));
            }
            break;
        }
    }

    // ---- 4. lines that still overflow the width get the end ellipsis --------------
    if (trim && maxWidth_ > 0.0f) {
        for (Line& l : lines_) {
            if (l.width > maxWidth_ + 0.01f && l.line != nullptr) {
                l.line = truncate(l.line, maxWidth_);
                l.width = lineWidth(l.line);
            }
        }
    }

    width_ = 0.0f;
    for (const Line& l : lines_) {
        width_ = std::max(width_, l.width);
    }
    return true;
}

void MacTextLayout::draw(CGContextRef ctx, Point origin) const {
    if (ctx == nullptr || lines_.empty()) {
        return;
    }
    CGContextSaveGState(ctx);
    // The view is flipped (y down): flip the glyphs back upright.
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1.0, -1.0));
    // Clip to the layout box when a width was given (DWRITE_DRAW_TEXT_OPTIONS_CLIP).
    if (maxWidth_ > 0.0f) {
        CGContextClipToRect(ctx, CGRectMake(origin.x - 1.0, origin.y, maxWidth_ + 2.0, height()));
    }
    for (size_t i = 0; i < lines_.size(); ++i) {
        const Line& l = lines_[i];
        if (l.line == nullptr) {
            continue;
        }
        CGContextSetTextPosition(ctx, origin.x, origin.y + lineHeight_ * static_cast<float>(i) + baseline_);
        CTLineDraw(l.line, ctx);
    }
    CGContextRestoreGState(ctx);
}

CFIndex MacTextLayout::toUtf16(size_t pos) const {
    if (utf16Of_.empty()) {
        return 0;
    }
    pos = std::min(pos, utf16Of_.size() - 1);
    return utf16Of_[pos];
}

size_t MacTextLayout::fromUtf16(CFIndex index) const {
    if (utf16Of_.empty() || index <= 0) {
        return 0;
    }
    // utf16Of_ is sorted: the last code point that starts at or before index.
    const auto it = std::upper_bound(utf16Of_.begin(), utf16Of_.end(), index);
    const size_t pos = static_cast<size_t>(std::distance(utf16Of_.begin(), it));
    return pos == 0 ? 0 : std::min(pos - 1, utf16Of_.size() - 1);
}

size_t MacTextLayout::lineForUtf16(CFIndex index) const {
    for (size_t i = 0; i < lines_.size(); ++i) {
        const Line& l = lines_[i];
        if (index < l.start + l.length) {
            return i;
        }
    }
    return lines_.empty() ? 0 : lines_.size() - 1;
}

float MacTextLayout::caretX(size_t pos) const {
    if (lines_.empty()) {
        return 0.0f;
    }
    const CFIndex u = toUtf16(pos);
    const Line& l = lines_[lineForUtf16(u)];
    if (l.line == nullptr) {
        return 0.0f;
    }
    return static_cast<float>(CTLineGetOffsetForStringIndex(l.line, u, nullptr));
}

size_t MacTextLayout::hitTest(float x, float y) const {
    if (lines_.empty()) {
        return 0;
    }
    const float row = lineHeight_ > 0.0f ? std::floor(y / lineHeight_) : 0.0f;
    const size_t i = static_cast<size_t>(std::clamp(row, 0.0f, static_cast<float>(lines_.size() - 1)));
    const Line& l = lines_[i];
    if (l.line == nullptr) {
        return fromUtf16(l.start);
    }
    CFIndex u = CTLineGetStringIndexForPosition(l.line, CGPointMake(x, 0.0));
    if (u == kCFNotFound) {
        u = l.start;
    }
    return fromUtf16(u);
}

std::vector<Rect> MacTextLayout::rangeRects(size_t pos, size_t length) const {
    std::vector<Rect> out;
    if (length == 0 || lines_.empty()) {
        return out;
    }
    const CFIndex a = toUtf16(pos);
    const CFIndex b = toUtf16(pos + length);
    for (size_t i = 0; i < lines_.size(); ++i) {
        const Line& l = lines_[i];
        const CFIndex s = std::max(a, l.start);
        const CFIndex e = std::min(b, l.start + l.length);
        if (e <= s || l.line == nullptr) {
            continue;
        }
        const float x0 = static_cast<float>(CTLineGetOffsetForStringIndex(l.line, s, nullptr));
        const float x1 = static_cast<float>(CTLineGetOffsetForStringIndex(l.line, e, nullptr));
        out.push_back({std::min(x0, x1), lineHeight_ * static_cast<float>(i), std::fabs(x1 - x0), lineHeight_});
    }
    return out;
}

// ===========================================================================
// TextCache
// ===========================================================================

size_t TextCache::LayoutKeyHash::operator()(const LayoutKey& k) const noexcept {
    size_t h = std::hash<std::wstring>{}(k.text);
    h = hashCombine(h, std::hash<uint64_t>{}(k.style));
    h = hashCombine(h, std::hash<uint32_t>{}(std::bit_cast<uint32_t>(k.width)));
    h = hashCombine(h, std::hash<int>{}(k.lines));
    h = hashCombine(h, std::hash<int>{}(static_cast<int>(k.trim)));
    return h;
}

/**
 * @brief Records the family names (the fonts themselves resolve lazily).
 */
void TextCache::init() {
    shutdown();
    families_[0] = L"SF Pro Text";
    families_[1] = L"SF Pro Display";
    families_[2] = L"SF Pro Text";
    families_[3] = L"SF Mono";
    initialised_ = true;
    HH_LOG_DEBUG(kLog, L"font families: text='{}' display='{}' small='{}' mono='{}'", families_[0], families_[1],
                 families_[2], families_[3]);
}

void TextCache::shutdown() {
    layouts_.clear();
    lru_.clear();
    metrics_.clear();
    for (auto& [key, font] : fonts_) {
        static_cast<void>(key);
        if (font != nullptr) {
            CFRelease(font);
        }
    }
    fonts_.clear();
    for (std::wstring& f : families_) {
        f.clear();
    }
    initialised_ = false;
}

const std::wstring& TextCache::familyName(TextStyle::Family f) const {
    static const std::wstring kEmpty;
    const size_t index = static_cast<size_t>(f);
    if (index >= 4) {
        return kEmpty;
    }
    return families_[index];
}

const void* TextCache::font(const TextStyle& style) {
    const uint64_t key = style.key();
    const auto it = fonts_.find(key);
    if (it != fonts_.end() && it->second != nullptr) {
        return it->second;
    }
    CTFontRef created = createFontFor(style);
    if (created == nullptr) {
        HH_LOG_ERROR(kLog, L"no font for size {}", style.size);
        return nullptr;
    }
    fonts_[key] = created;
    return created;
}

TextLayoutRef TextCache::createLayout(std::wstring_view text, const TextStyle& style, float maxWidth, Trimming trimming,
                                      int maxLines) {
    CTFontRef ctFont = static_cast<CTFontRef>(font(style));
    if (ctFont == nullptr) {
        return nullptr;
    }
    const std::wstring shaped = style.uppercase ? platform::toUpperInvariant(text) : std::wstring(text);
    const LineMetrics lm = lineMetrics(style);
    auto layout = std::make_shared<MacTextLayout>();
    // A single line never wraps; the style decides otherwise (as on Windows).
    const bool wrap = style.wrap && maxLines != 1;
    if (!layout->build(shaped, ctFont, style.letterSpacing, lm.lineHeight, lm.baseline, maxWidth, wrap,
                       trimming != Trimming::None, std::max(0, maxLines))) {
        HH_LOG_ERROR(kLog, L"CoreText layout failed for {} chars", shaped.size());
        return nullptr;
    }
    return layout;
}

TextLayoutRef TextCache::layout(std::wstring_view text, const TextStyle& style, float maxWidth, Trimming trimming,
                                int maxLines) {
    if (!initialised_) {
        init();
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

    TextLayoutRef created = createLayout(text, style, key.width, trimming, key.lines);
    if (!created) {
        return nullptr;
    }

    // Insert at the front of the LRU and evict from the back when over budget.
    lru_.push_front(key);
    LayoutEntry entry;
    entry.layout = created;
    entry.lru = lru_.begin();
    layouts_.emplace(std::move(key), std::move(entry));
    while (layouts_.size() > maxLayouts_ && !lru_.empty()) {
        layouts_.erase(lru_.back());
        lru_.pop_back();
    }
    return created;
}

Size TextCache::measure(std::wstring_view text, const TextStyle& style, float maxWidth, int maxLines) {
    TextLayoutRef l = layout(text, style, maxWidth, Trimming::End, maxLines);
    if (!l) {
        // No font: mirror the estimate the shared helpers use.
        const float size = std::max(1.0f, style.size);
        float width = static_cast<float>(text.size()) * size * 0.55f;
        if (maxWidth > 0.0f) {
            width = std::min(width, maxWidth);
        }
        return {width, 1.3f * size * static_cast<float>(std::max(1, maxLines))};
    }
    return layoutSize(l);
}

TextCache::LineMetrics TextCache::lineMetrics(const TextStyle& style) {
    const uint64_t key = style.key();
    const auto it = metrics_.find(key);
    if (it != metrics_.end()) {
        return it->second;
    }
    const float size = std::max(1.0f, style.size);
    LineMetrics lm;
    CTFontRef ctFont = static_cast<CTFontRef>(font(style));
    if (ctFont != nullptr) {
        lm.ascent = static_cast<float>(CTFontGetAscent(ctFont));
        lm.descent = static_cast<float>(CTFontGetDescent(ctFont));
        const float leading = static_cast<float>(CTFontGetLeading(ctFont));
        lm.lineHeight = std::max(1.3f * size, lm.ascent + lm.descent + leading);
    } else {
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
    // Keep a short file extension intact so "..._v3.mkv" still reads as an mkv.
    std::wstring ext;
    const size_t dot = full.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0 && full.size() - dot <= 8) {
        ext = full.substr(dot);
        full.erase(dot);
    }
    const std::wstring ellipsis = L"…";
    // UTF-32: every index is a whole code point, so no pair can be split.
    const auto candidate = [&](size_t keep) {
        keep = std::min(keep, full.size());
        const size_t headLen = keep / 2;
        const size_t tailStart = std::max(full.size() - (keep - headLen), headLen);
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

void TextCache::clearLayouts() {
    layouts_.clear();
    lru_.clear();
}

// ---- layout queries ------------------------------------------------------------

Size TextCache::layoutSize(const TextLayoutRef& layout) {
    if (!layout) {
        return {};
    }
    return {std::max(0.0f, layout->width()), std::max(0.0f, layout->height())};
}

float TextCache::caretX(const TextLayoutRef& layout, size_t pos) {
    return layout ? layout->caretX(pos) : 0.0f;
}

bool TextCache::hitTest(const TextLayoutRef& layout, float x, float y, size_t& position) {
    if (!layout) {
        return false;
    }
    position = layout->hitTest(x, y);
    return true;
}

bool TextCache::rangeRects(const TextLayoutRef& layout, size_t pos, size_t length, std::vector<Rect>& out) {
    out.clear();
    if (!layout || length == 0) {
        return false;
    }
    out = layout->rangeRects(pos, length);
    return !out.empty();
}

} // namespace hh::ui
