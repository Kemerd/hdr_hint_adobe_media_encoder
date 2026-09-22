// ---------------------------------------------------------------------------
// ThemeManagerWin.cpp - ThemeManager's OS readers on Windows.
//
// Dark mode and the accent colour come from the per-user personalisation
// registry values; motion and contrast from SystemParametersInfo. The rest
// of ThemeManager (ThemeManager.cpp) is platform-neutral policy.
// ---------------------------------------------------------------------------
#include "ui/theme/ThemeManager.h"

#include "core/Logger.h"

#include <optional>

namespace hh::ui {

namespace {

/// Log component tag.
constexpr const wchar_t* kLog = L"Theme";

/// Registry locations for the personalisation values we read.
constexpr const wchar_t* kPersonalizeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr const wchar_t* kAppsUseLightTheme = L"AppsUseLightTheme";
constexpr const wchar_t* kDwmKey = L"Software\\Microsoft\\Windows\\DWM";
constexpr const wchar_t* kAccentColor = L"AccentColor";

/**
 * @brief Reads a REG_DWORD from HKCU. Missing/wrong-type values yield nullopt.
 */
std::optional<DWORD> readHkcuDword(const wchar_t* subKey, const wchar_t* valueName)
{
    if (!subKey || !valueName) {
        return std::nullopt;
    }
    DWORD data = 0;
    DWORD bytes = sizeof(data);
    DWORD type = 0;
    // RRF_RT_REG_DWORD rejects anything that is not a 32-bit value.
    const LSTATUS status = ::RegGetValueW(HKEY_CURRENT_USER, subKey, valueName, RRF_RT_REG_DWORD, &type, &data, &bytes);
    if (status != ERROR_SUCCESS || bytes != sizeof(data)) {
        return std::nullopt;
    }
    return data;
}

} // namespace

/**
 * @brief AppsUseLightTheme == 0 (or missing) means dark.
 */
bool ThemeManager::readSystemDark()
{
    const std::optional<DWORD> value = readHkcuDword(kPersonalizeKey, kAppsUseLightTheme);
    if (!value) {
        // Older builds without the value default to the classic dark look.
        return true;
    }
    return *value == 0;
}

/**
 * @brief The user's accent colour from DWM\AccentColor (stored as ABGR).
 *
 * This is the real "Accent colour" from Settings, unlike
 * DwmGetColorizationColor which returns the (possibly desaturated) title-bar
 * colourisation.
 */
std::optional<Color> ThemeManager::readSystemAccent()
{
    const std::optional<DWORD> value = readHkcuDword(kDwmKey, kAccentColor);
    if (!value) {
        return std::nullopt;
    }
    // Layout is 0xAABBGGRR: red lives in the low byte.
    const DWORD abgr = *value;
    const float r = static_cast<float>(abgr & 0xFFu) / 255.0f;
    const float g = static_cast<float>((abgr >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>((abgr >> 16) & 0xFFu) / 255.0f;
    return Color{r, g, b, 1.0f};
}

/**
 * @brief True when the user switched off "Animation effects" (client-area animation).
 */
bool ThemeManager::readReducedMotion()
{
    BOOL animate = TRUE;
    if (!::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0)) {
        HH_LOG_WARN(kLog, L"SPI_GETCLIENTAREAANIMATION failed ({}); assuming animations on", ::GetLastError());
        return false;
    }
    return animate == FALSE;
}

/**
 * @brief True when a high-contrast theme is active.
 */
bool ThemeManager::readHighContrast()
{
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (!::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        HH_LOG_WARN(kLog, L"SPI_GETHIGHCONTRAST failed ({}); assuming off", ::GetLastError());
        return false;
    }
    return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

} // namespace hh::ui
