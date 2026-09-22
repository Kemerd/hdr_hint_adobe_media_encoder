// ---------------------------------------------------------------------------
// PopupSurface.cpp - shadow + rounded card host for popup menus.
//
// Moved out of the Win32 PopupWindow unchanged so the macOS popup panel can
// host menus with exactly the same look.
// ---------------------------------------------------------------------------
#include "ui/core/PopupSurface.h"

#include "ui/gfx/Canvas.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <utility>

namespace hh::ui {

PopupSurface::PopupSurface(float shadowMargin) : margin_(std::max(0.0f, shadowMargin)) {}

Widget* PopupSurface::setMenu(std::unique_ptr<Widget> menu) {
    retireMenu();
    if (!menu) {
        return nullptr;
    }
    menu_ = addChild(std::move(menu));
    invalidateLayout();
    return menu_;
}

void PopupSurface::retireMenu() {
    if (menu_ == nullptr) {
        return;
    }
    std::unique_ptr<Widget> old = removeChild(menu_);
    menu_ = nullptr;
    if (old) {
        retired_.push_back(std::move(old));
    }
    invalidateLayout();
}

Size PopupSurface::measure(const Constraints& c) {
    Size inner{0.0f, 0.0f};
    if (menu_ != nullptr && menu_->visible()) {
        // Offer the menu everything minus the margins (never negative).
        Constraints loose = c;
        loose.minW = 0.0f;
        loose.minH = 0.0f;
        loose.maxW = std::max(0.0f, c.maxW - 2.0f * margin_);
        loose.maxH = std::max(0.0f, c.maxH - 2.0f * margin_);
        inner = menu_->measure(loose);
    }
    return c.constrain({inner.w + 2.0f * margin_, inner.h + 2.0f * margin_});
}

void PopupSurface::onLayout() {
    if (menu_ == nullptr) {
        return;
    }
    Rect card = cardRect();
    card.w = std::max(0.0f, card.w);
    card.h = std::max(0.0f, card.h);
    menu_->measure(Constraints::tight(card.size()));
    menu_->layout(card);
}

void PopupSurface::paintSelf(Canvas& c) {
    const Theme* t = theme();
    const Theme fallback = Theme::dark();
    if (t == nullptr) {
        t = &fallback;
    }
    const Rect card = c.scale().snap(cardRect());
    if (card.isEmpty()) {
        return;
    }
    // Soft shadow first, then the opaque card on top of it.
    c.drawShadow(card, kPopupCardRadius, kPopupShadowBlur, t->shadow, 6.0f);
    c.fillRoundedRect(card, kPopupCardRadius, t->elevatedOpaque);
    // A hairline outline separates the card from busy backgrounds.
    const float hair = c.scale().hairline();
    c.strokeRoundedRect(card.inset(hair * 0.5f), kPopupCardRadius, t->separatorStrong, hair);
}

} // namespace hh::ui
