// ---------------------------------------------------------------------------
// ThemeManager.h - picks the active Theme from settings + system state.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"
#include "ui/theme/Theme.h"

#include <functional>
#include <optional>
#include <vector>

namespace hh::ui {

enum class ThemeMode { System, Dark, Light };
enum class AccentMode { Blue, System };

class ThemeManager {
public:
    ThemeManager();

    [[nodiscard]] const Theme& current() const noexcept { return current_; }

    void setMode(ThemeMode mode);
    void setAccentMode(AccentMode mode);
    /// Docked windows paint opaque in the host panel's colour.
    void setDocked(bool docked, std::optional<Color> panelBackground);
    /// "Reduce transparency": forces an opaque window background.
    void setReduceTransparency(bool reduce);
    /// True when the backdrop (Mica) is actually rendered by DWM; else paint opaque.
    void setBackdropAvailable(bool available);

    [[nodiscard]] ThemeMode mode() const noexcept { return mode_; }
    [[nodiscard]] AccentMode accentMode() const noexcept { return accentMode_; }
    [[nodiscard]] bool systemIsDark() const noexcept { return systemDark_; }

    /// WM_SETTINGCHANGE(lParam "ImmersiveColorSet") / WM_DWMCOLORIZATIONCOLORCHANGED -> re-read system state.
    void refreshFromSystem();

    /// Listeners are called whenever current() changes.
    void addListener(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

    /// Reads AppsUseLightTheme from the registry (true = dark).
    static bool readSystemDark();
    /// Reads the user's accent colour (DWM\AccentColor), if available.
    static std::optional<Color> readSystemAccent();
    /// SPI_GETCLIENTAREAANIMATION == FALSE -> reduced motion.
    static bool readReducedMotion();
    /// High-contrast mode active.
    static bool readHighContrast();

private:
    void rebuild();

    Theme current_;
    ThemeMode mode_ = ThemeMode::System;
    AccentMode accentMode_ = AccentMode::Blue;
    bool systemDark_ = true;
    bool docked_ = false;
    std::optional<Color> panelBackground_;
    bool reduceTransparency_ = false;
    bool backdropAvailable_ = true;
    bool highContrast_ = false;
    std::optional<Color> systemAccent_;
    std::vector<std::function<void()>> listeners_;
};

} // namespace hh::ui
