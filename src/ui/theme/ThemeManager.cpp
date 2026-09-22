// ---------------------------------------------------------------------------
// ThemeManager.cpp - resolves settings + system state into one Theme.
//
// Inputs: the user's mode (System/Dark/Light), accent mode (Blue/System),
// docked state with the host panel colour, "reduce transparency", whether
// DWM actually renders a backdrop, and the OS (dark mode, accent colour,
// high contrast). rebuild() folds them into a Theme and only tells the
// listeners when something visible changed.
// ---------------------------------------------------------------------------
#include "ui/theme/ThemeManager.h"

#include "core/Logger.h"

#include <cmath>
#include <utility>

namespace hh::ui {

namespace {

/// Log component tag.
constexpr const wchar_t* kLog = L"Theme";

/**
 * @brief Field-by-field equality; Theme has no operator== of its own.
 */
bool sameTheme(const Theme& a, const Theme& b)
{
    return a.windowBackground == b.windowBackground
        && a.elevated == b.elevated
        && a.elevatedOpaque == b.elevatedOpaque
        && a.fillSecondary == b.fillSecondary
        && a.fillTertiary == b.fillTertiary
        && a.fillQuaternary == b.fillQuaternary
        && a.separator == b.separator
        && a.separatorStrong == b.separatorStrong
        && a.labelPrimary == b.labelPrimary
        && a.labelSecondary == b.labelSecondary
        && a.labelTertiary == b.labelTertiary
        && a.labelQuaternary == b.labelQuaternary
        && a.accent == b.accent
        && a.accentText == b.accentText
        && a.success == b.success
        && a.warning == b.warning
        && a.destructive == b.destructive
        && a.info == b.info
        && a.tonePQ == b.tonePQ
        && a.toneHLG == b.toneHLG
        && a.toneSDR == b.toneSDR
        && a.shadow == b.shadow
        && a.focusRing == b.focusRing
        && a.scrollThumb == b.scrollThumb
        && a.isDark == b.isDark
        && a.translucent == b.translucent;
}

/**
 * @brief Human-readable mode names for the log.
 */
const wchar_t* modeName(ThemeMode mode) noexcept
{
    switch (mode) {
    case ThemeMode::System: return L"system";
    case ThemeMode::Dark: return L"dark";
    case ThemeMode::Light: return L"light";
    }
    return L"?";
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/**
 * @brief Reads the OS state and builds the first theme (no listeners yet).
 */
ThemeManager::ThemeManager()
{
    // refreshFromSystem() reads the registry / SPI values and rebuilds.
    refreshFromSystem();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

/**
 * @brief Selects System / Dark / Light.
 */
void ThemeManager::setMode(ThemeMode mode)
{
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    rebuild();
}

/**
 * @brief Selects the fixed HIG blue or the user's Windows accent colour.
 */
void ThemeManager::setAccentMode(AccentMode mode)
{
    if (accentMode_ == mode) {
        return;
    }
    accentMode_ = mode;
    rebuild();
}

/**
 * @brief Enters/leaves docked mode with the host panel's background (optional).
 */
void ThemeManager::setDocked(bool docked, std::optional<Color> panelBackground)
{
    docked_ = docked;
    panelBackground_ = std::move(panelBackground);
    rebuild();
}

/**
 * @brief "Reduce transparency" preference: forces an opaque window.
 */
void ThemeManager::setReduceTransparency(bool reduce)
{
    if (reduceTransparency_ == reduce) {
        return;
    }
    reduceTransparency_ = reduce;
    rebuild();
}

/**
 * @brief Whether DWM is actually drawing a backdrop behind the window.
 */
void ThemeManager::setBackdropAvailable(bool available)
{
    if (backdropAvailable_ == available) {
        return;
    }
    backdropAvailable_ = available;
    rebuild();
}

// ---------------------------------------------------------------------------
// System state
// ---------------------------------------------------------------------------

/**
 * @brief Re-reads dark mode, accent colour and high contrast, then rebuilds.
 *
 * Called at construction and on WM_SETTINGCHANGE("ImmersiveColorSet") /
 * WM_DWMCOLORIZATIONCOLORCHANGED. Listeners only fire when the resulting
 * theme differs from the current one.
 */
void ThemeManager::refreshFromSystem()
{
    systemDark_ = readSystemDark();
    systemAccent_ = readSystemAccent();
    highContrast_ = readHighContrast();
    rebuild();
}

// ---------------------------------------------------------------------------
// Rebuild
// ---------------------------------------------------------------------------

/**
 * @brief Folds every input into a Theme and notifies listeners on change.
 *
 * Order matters: the accent is applied to the base palette first so
 * docked() can carry it across a polarity flip, and the translucency /
 * high-contrast overrides come last because they win over everything.
 */
void ThemeManager::rebuild()
{
    // 1. Base palette from the mode (System follows the OS).
    const bool wantDark = (mode_ == ThemeMode::System) ? systemDark_ : (mode_ == ThemeMode::Dark);
    Theme next = wantDark ? Theme::dark() : Theme::light();

    // 2. Accent: the user's Windows colour when requested and readable.
    if (accentMode_ == AccentMode::System && systemAccent_) {
        next.applyAccent(*systemAccent_);
    }

    // 3. Docked: opaque in the host panel's colour (palette default otherwise).
    if (docked_) {
        const Color panel = panelBackground_.value_or(next.windowBackground);
        next = Theme::docked(next, panel);
    }

    // 4. Translucency only when nothing forbids it and DWM really draws a backdrop.
    next.translucent = !docked_ && !reduceTransparency_ && backdropAvailable_ && !highContrast_;

    // 5. High contrast: every hairline becomes the strong separator.
    if (highContrast_) {
        next.separator = next.separatorStrong;
    }

    // Nothing visible changed: keep the listeners quiet.
    if (sameTheme(current_, next)) {
        return;
    }
    current_ = next;

    HH_LOG_DEBUG(kLog, L"theme rebuilt: mode={} dark={} systemAccent={} docked={} translucent={} highContrast={}",
                 modeName(mode_), current_.isDark, accentMode_ == AccentMode::System && systemAccent_.has_value(),
                 docked_, current_.translucent, highContrast_);

    // Iterate over a copy: a listener is allowed to add further listeners.
    const std::vector<std::function<void()>> listeners = listeners_;
    for (const auto& fn : listeners) {
        if (fn) {
            fn();
        }
    }
}

} // namespace hh::ui
