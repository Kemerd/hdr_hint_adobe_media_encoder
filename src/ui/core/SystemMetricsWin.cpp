// ---------------------------------------------------------------------------
// SystemMetricsWin.cpp - SystemMetrics.h on Win32.
// ---------------------------------------------------------------------------
#include "ui/core/SystemMetrics.h"

#include "core/Logger.h"
#include "platform/Win.h"

namespace hh::ui::system {

namespace {

constexpr const wchar_t* kLog = L"SystemMetrics";

/// Windows' own default double-click time, used when the query fails.
constexpr int kDefaultDoubleClickMs = 500;

} // namespace

/**
 * @brief GetCaretBlinkTime: INFINITE = never blink, 0 = the query failed.
 */
int caretBlinkMs() {
    const UINT ms = ::GetCaretBlinkTime();
    if (ms == INFINITE) {
        return 0;
    }
    if (ms == 0) {
        HH_LOG_DEBUG(kLog, L"GetCaretBlinkTime failed ({})", ::GetLastError());
        return -1;
    }
    return static_cast<int>(ms);
}

/**
 * @brief GetDoubleClickTime (never 0 in practice; guarded anyway).
 */
int doubleClickMs() {
    const UINT ms = ::GetDoubleClickTime();
    return ms > 0 ? static_cast<int>(ms) : kDefaultDoubleClickMs;
}

} // namespace hh::ui::system
