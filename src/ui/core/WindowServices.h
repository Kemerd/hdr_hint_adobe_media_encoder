// ---------------------------------------------------------------------------
// WindowServices.h - what a RootView needs from the HWND that hosts it.
// Implemented by WindowHost (main window) and PopupWindow (menus).
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"
#include "ui/core/InputEvents.h"
#include "ui/gfx/Geometry.h"

#include <functional>
#include <memory>
#include <string>

namespace hh::ui {

class Widget;

/// Where a popup goes relative to its anchor (root-space rect of the opener).
enum class PopupPlacement { Below, Above, Auto, AtPoint };

class IWindowServices {
public:
    virtual ~IWindowServices() = default;

    virtual void requestFrame() = 0;
    virtual void setCursor(CursorKind cursor) = 0;
    virtual void captureMouse(bool capture) = 0;
    [[nodiscard]] virtual HWND hwnd() const = 0;
    [[nodiscard]] virtual DipScale dipScale() const = 0;
    /// Client size in dips.
    [[nodiscard]] virtual Size clientSizeDips() const = 0;
    /// Root-space dips -> screen pixels.
    [[nodiscard]] virtual POINT rootToScreenPx(Point rootPt) const = 0;
    /// Screen pixels -> root-space dips.
    [[nodiscard]] virtual Point screenPxToRoot(POINT pt) const = 0;
    [[nodiscard]] virtual bool isActiveWindow() const = 0;

    /**
     * @brief Opens a separate popup HWND with @p content (menus).
     * @param anchorRoot   opener rect in root dips of THIS window
     * @param contentSize  measured size of the content in dips
     * @param onDismiss    called when the popup closes for any reason
     */
    virtual void showPopupWindow(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                                 Size contentSize, std::function<void()> onDismiss) = 0;
    virtual void dismissPopupWindow() = 0;
    [[nodiscard]] virtual bool popupWindowVisible() const = 0;

    /// Positions the IME composition/candidate windows at a caret rect (root dips).
    virtual void setImeCaret(const Rect& caretRoot) = 0;
    virtual void setImeEnabled(bool enabled) = 0;

    virtual bool clipboardSetText(const std::wstring& text) = 0;
    virtual std::wstring clipboardGetText() = 0;
};

} // namespace hh::ui
