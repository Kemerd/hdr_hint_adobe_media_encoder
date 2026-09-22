// ---------------------------------------------------------------------------
// SystemMetrics.h - the handful of OS user preferences widgets read directly.
//
// Everything else the toolkit needs from the system (theme, accent, motion,
// transparency) flows through ThemeManager. These are the leftovers that a
// single control asks for at a single moment, so they get plain functions.
//
//   Windows  SystemMetricsWin.cpp  (GetCaretBlinkTime, GetDoubleClickTime, ...)
//   macOS    ui/mac/SystemMetricsMac.mm (NSUserDefaults / NSEvent)
// ---------------------------------------------------------------------------
#pragma once

namespace hh::ui::system {

/**
 * @brief The caret's on/off period in milliseconds.
 * @return > 0 the period, 0 = the user turned blinking off, < 0 = unknown
 */
int caretBlinkMs();

/**
 * @brief Longest gap between two clicks that still makes a double click.
 * @return milliseconds (always > 0; a sane default when the query fails)
 */
int doubleClickMs();

} // namespace hh::ui::system
