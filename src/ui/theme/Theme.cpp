// ---------------------------------------------------------------------------
// Theme.cpp - the two built-in palettes plus the docked / accent variants.
//
// Tokens follow Apple's semantic naming so a control never reasons about
// "grey 3"; it asks for fillSecondary or labelTertiary and gets the right
// tint for the current background. Neutral tokens are white tints on dark
// and black tints on light, which is what makes docked() able to flip
// polarity by swapping whole groups.
// ---------------------------------------------------------------------------
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace hh::ui {

namespace {

/// A panel darker than this relative luminance gets the dark neutral set.
constexpr float kDarkLuminanceCutoff = 0.5f;

/// Accents brighter than this get dark text on top (readability).
constexpr float kLightAccentCutoff = 0.55f;

/**
 * @brief Clamps every channel to 0..1 and forces full opacity.
 *
 * Accent colours arrive from the registry or from settings text, so a
 * component can be out of range or NaN; the palette must never carry that.
 */
Color sanitizeOpaque(const Color& c)
{
    auto clean = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; };
    return {clean(c.r), clean(c.g), clean(c.b), 1.0f};
}

} // namespace

// ---------------------------------------------------------------------------
// Built-in palettes
// ---------------------------------------------------------------------------

/**
 * @brief The dark palette (default; matches Windows dark mode + AME).
 */
Theme Theme::dark()
{
    Theme t;

    // Surfaces: near-black window, white tints for everything that sits on it.
    t.windowBackground = Color::fromHex(0x1E1E1E);
    t.elevated = Color::white(0.06f);
    t.elevatedOpaque = Color::fromHex(0x2C2C2E);
    t.fillSecondary = Color::white(0.10f);
    t.fillTertiary = Color::white(0.07f);
    t.fillQuaternary = Color::white(0.05f);
    t.separator = Color::white(0.10f);
    t.separatorStrong = Color::white(0.18f);

    // Text hierarchy.
    t.labelPrimary = Color::white(0.92f);
    t.labelSecondary = Color::white(0.60f);
    t.labelTertiary = Color::white(0.35f);
    t.labelQuaternary = Color::white(0.20f);

    // Semantic colours (HIG dark-mode system colours).
    t.accent = Color::fromHex(0x0A84FF);
    t.accentText = Color::white();
    t.success = Color::fromHex(0x30D158);
    t.warning = Color::fromHex(0xFFD60A);
    t.destructive = Color::fromHex(0xFF453A);
    t.info = Color::fromHex(0x64D2FF);

    // Transfer-function badges.
    t.tonePQ = Color::fromHex(0xBF5AF2);
    t.toneHLG = Color::fromHex(0x5AC8F5);
    t.toneSDR = Color::fromHex(0x98989D);

    // Misc.
    t.shadow = Color::black(0.30f);
    t.focusRing = t.accent.withAlpha(0.50f);
    t.scrollThumb = Color::white(0.35f);
    t.isDark = true;
    t.translucent = true;
    return t;
}

/**
 * @brief The light palette.
 */
Theme Theme::light()
{
    Theme t;

    // Surfaces: grouped-background grey, black tints on top.
    t.windowBackground = Color::fromHex(0xF2F2F7);
    t.elevated = Color::white(0.72f);
    t.elevatedOpaque = Color::fromHex(0xFFFFFF);
    t.fillSecondary = Color::black(0.08f);
    t.fillTertiary = Color::black(0.05f);
    t.fillQuaternary = Color::black(0.035f);
    t.separator = Color::black(0.10f);
    t.separatorStrong = Color::black(0.18f);

    // Text hierarchy.
    t.labelPrimary = Color::black(0.88f);
    t.labelSecondary = Color::black(0.55f);
    t.labelTertiary = Color::black(0.30f);
    t.labelQuaternary = Color::black(0.18f);

    // Semantic colours (HIG light-mode system colours).
    t.accent = Color::fromHex(0x007AFF);
    t.accentText = Color::white();
    t.success = Color::fromHex(0x34C759);
    t.warning = Color::fromHex(0xFF9F0A);
    t.destructive = Color::fromHex(0xFF3B30);
    t.info = Color::fromHex(0x32ADE6);

    // Transfer-function badges.
    t.tonePQ = Color::fromHex(0xAF52DE);
    t.toneHLG = Color::fromHex(0x32ADE6);
    t.toneSDR = Color::fromHex(0x8E8E93);

    // Misc.
    t.shadow = Color::black(0.12f);
    t.focusRing = t.accent.withAlpha(0.50f);
    t.scrollThumb = Color::black(0.35f);
    t.isDark = false;
    t.translucent = true;
    return t;
}

// ---------------------------------------------------------------------------
// Variants
// ---------------------------------------------------------------------------

/**
 * @brief Opaque variant for a window docked inside AME.
 *
 * The window paints in the host panel's colour, the polarity follows that
 * colour's luminance (an AME "lightest" skin gets dark text even when the
 * OS is in dark mode), and the accent trio is carried over from @p base so
 * a system accent survives docking.
 */
Theme Theme::docked(const Theme& base, const Color& panelBackground)
{
    // Docked windows are always opaque: the panel colour is the backdrop.
    const Color panel = sanitizeOpaque(panelBackground);
    const bool isDark = panel.luminance() < kDarkLuminanceCutoff;

    // When the panel's polarity disagrees with the base palette, every
    // neutral token (labels, fills, separators, shadow, scroll thumb, opaque
    // elevated) would be invisible; take them from the matching palette
    // instead and re-apply the accent trio from the base afterwards.
    Theme t = (isDark == base.isDark) ? base : (isDark ? dark() : light());
    if (isDark != base.isDark) {
        t.accent = base.accent;
        t.accentText = base.accentText;
        t.focusRing = base.focusRing;
    }

    // Docked overrides.
    t.windowBackground = panel;
    t.translucent = false;
    t.isDark = isDark;
    t.elevated = isDark ? Color::white(0.06f) : Color::black(0.04f);
    return t;
}

/**
 * @brief Replaces the accent and derives a readable text colour + focus ring.
 */
void Theme::applyAccent(const Color& colour)
{
    const Color c = sanitizeOpaque(colour);
    accent = c;
    // Bright accents (yellow, mint) need dark text; everything else stays white.
    accentText = c.luminance() > kLightAccentCutoff ? Color::black(0.88f) : Color::white();
    focusRing = c.withAlpha(0.50f);
}

} // namespace hh::ui
