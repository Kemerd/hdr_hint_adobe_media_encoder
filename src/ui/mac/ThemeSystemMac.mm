// ---------------------------------------------------------------------------
// ThemeSystemMac.mm - ThemeManager's OS readers on macOS.
//
// Dark mode comes from the app's effective appearance (which follows System
// Settings > Appearance, including "Auto"), the accent colour from
// +[NSColor controlAccentColor], and motion / contrast from the Accessibility
// display options NSWorkspace exposes.
// ---------------------------------------------------------------------------
#include "ui/theme/ThemeManager.h"

#include "core/Logger.h"

#import <AppKit/AppKit.h>

#include <optional>

namespace hh::ui {

namespace {

/// Log component tag.
constexpr const wchar_t* kLog = L"Theme";

} // namespace

/**
 * @brief True when the app currently renders with a dark appearance.
 */
bool ThemeManager::readSystemDark()
{
    @autoreleasepool {
        NSAppearance* appearance = nil;
        if (NSApp != nil) {
            appearance = NSApp.effectiveAppearance;
        }
        if (appearance == nil) {
            // Before NSApplication exists: the user's global preference.
            NSString* style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
            return style != nil && [style caseInsensitiveCompare:@"Dark"] == NSOrderedSame;
        }
        NSAppearanceName best =
            [appearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
        return [best isEqualToString:NSAppearanceNameDarkAqua];
    }
}

/**
 * @brief The user's accent colour (System Settings > Appearance > Accent colour), as sRGB.
 */
std::optional<Color> ThemeManager::readSystemAccent()
{
    @autoreleasepool {
        NSColor* accent = [NSColor controlAccentColor];
        NSColor* srgb = [accent colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        if (srgb == nil) {
            HH_LOG_DEBUG(kLog, L"accent colour not convertible to sRGB");
            return std::nullopt;
        }
        return Color{static_cast<float>(srgb.redComponent), static_cast<float>(srgb.greenComponent),
                     static_cast<float>(srgb.blueComponent), 1.0f};
    }
}

/**
 * @brief Accessibility > Display > Reduce motion.
 */
bool ThemeManager::readReducedMotion()
{
    @autoreleasepool {
        return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
    }
}

/**
 * @brief Accessibility > Display > Increase contrast.
 */
bool ThemeManager::readHighContrast()
{
    @autoreleasepool {
        return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldIncreaseContrast];
    }
}

} // namespace hh::ui
