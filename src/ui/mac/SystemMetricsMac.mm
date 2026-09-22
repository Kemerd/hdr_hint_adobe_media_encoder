// ---------------------------------------------------------------------------
// SystemMetricsMac.mm - SystemMetrics.h on macOS.
//
// Caret blink: AppKit's own text views read NSTextInsertionPointBlinkPeriodOn
// / Off from the user defaults (both 560 ms unless the user or an
// accessibility setting changed them); a zero "off" period means "do not
// blink". Double-click: +[NSEvent doubleClickInterval].
// ---------------------------------------------------------------------------
#include "ui/core/SystemMetrics.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

namespace hh::ui::system {

namespace {

/// macOS' built-in insertion-point blink period.
constexpr int kDefaultBlinkMs = 560;

} // namespace

int caretBlinkMs() {
    @autoreleasepool {
        NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
        // Accessibility "Prefer non-blinking cursor" (macOS 14+) stops the blink.
        if ([defaults objectForKey:@"NSTextInsertionPointBlinkPeriodOff"] != nil &&
            [defaults integerForKey:@"NSTextInsertionPointBlinkPeriodOff"] <= 0) {
            return 0;
        }
        const NSInteger on = [defaults integerForKey:@"NSTextInsertionPointBlinkPeriodOn"];
        return on > 0 ? static_cast<int>(std::min<NSInteger>(on, 5000)) : kDefaultBlinkMs;
    }
}

int doubleClickMs() {
    const double seconds = [NSEvent doubleClickInterval];
    if (!(seconds > 0.0) || !std::isfinite(seconds)) {
        return 500;
    }
    return static_cast<int>(std::lround(seconds * 1000.0));
}

} // namespace hh::ui::system
