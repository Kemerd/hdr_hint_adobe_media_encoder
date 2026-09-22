// ---------------------------------------------------------------------------
// OverlayHost.cpp - the layer above the content: popups (in their own HWND),
// the tooltip and the toast stack.
//
// Popups live in a separate window so a small docked panel never clips a
// menu; this widget only remembers that one is open and hands the dismiss
// callback through. The tooltip and toasts are ordinary children painted
// inside the window.
// ---------------------------------------------------------------------------
#include "ui/core/OverlayHost.h"

#include "core/Logger.h"
#include "ui/anim/Spring.h"
#include "ui/controls/Toast.h"
#include "ui/core/RootView.h"
#include "ui/gfx/TextMeasure.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <memory>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"Overlay";

// Tooltip metrics (dips).
constexpr float kTipRadius = 6.0f;
constexpr float kTipPadX = 8.0f;
constexpr float kTipPadY = 6.0f;
constexpr float kTipMaxWidth = 320.0f;
constexpr int kTipMaxLines = 3;
constexpr float kTipGap = 8.0f;          ///< distance below the anchor point
constexpr float kTipEdgeInset = 4.0f;    ///< keep-out from the window edges

/// The caption style used for tooltip text (wrapping enabled).
TextStyle tooltipStyle() {
    TextStyle s = typography::caption();
    s.wrap = true;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// OverlayHost::TooltipView
// ---------------------------------------------------------------------------

/**
 * @brief A small rounded caption bubble that fades in below an anchor point.
 *
 * It is never interactive: the pointer passes straight through it to the
 * content underneath, exactly like native tooltips.
 */
class OverlayHost::TooltipView final : public Widget {
public:
    TooltipView() = default;

    /**
     * @brief Shows @p text anchored at @p anchorLocal (host-local dips) with a fade-in.
     */
    void show(const std::wstring& text, Point anchorLocal) {
        text_ = text;
        anchor_ = anchorLocal;
        showing_ = true;
        ++generation_;
        setVisible(true);
        invalidateLayout();
        // A fresh show starts from fully transparent unless it is already up.
        if (!wasVisible_) { opacity().set(0.0f); }
        wasVisible_ = true;
        opacity().animateTo(1.0f, springs::gentle);
    }

    /**
     * @brief Fades out and hides once the fade settled (a newer show() cancels the hide).
     */
    void hide() {
        if (!showing_ && !visible()) { return; }
        showing_ = false;
        const unsigned gen = ++generation_;
        opacity().animateTo(0.0f, springs::snappy, [this, gen]() {
            // Only the hide that started this fade may finish it.
            if (gen != generation_ || showing_) { return; }
            wasVisible_ = false;
            setVisible(false);
        });
    }

    /// True while the tooltip is (being) shown.
    [[nodiscard]] bool showing() const noexcept { return showing_; }

    /**
     * @brief Measures the bubble: caption text wrapped at the max width plus padding.
     */
    Size measure(const Constraints& c) override {
        const float maxOuter = std::min(kTipMaxWidth, std::max(0.0f, c.maxW));
        const float maxText = std::max(1.0f, maxOuter - 2.0f * kTipPadX);
        Size t = measureTextShared(text_, tooltipStyle(), maxText, kTipMaxLines);
        t.w = std::clamp(t.w, 0.0f, maxText);
        t.h = std::max(0.0f, t.h);
        return c.constrain({t.w + 2.0f * kTipPadX, t.h + 2.0f * kTipPadY});
    }

    /**
     * @brief Measures and positions the bubble 8 dip below the anchor, clamped to @p host.
     *
     * When there is no room below, the bubble flips above the anchor so it
     * never sits on top of the pointer.
     */
    void place(const Rect& host) {
        if (!visible()) { return; }
        const Size sz = measure(Constraints::loose({std::max(0.0f, host.w - 2.0f * kTipEdgeInset),
                                                    std::max(0.0f, host.h - 2.0f * kTipEdgeInset)}));
        float x = anchor_.x;
        float y = anchor_.y + kTipGap;
        if (y + sz.h > host.h - kTipEdgeInset) { y = anchor_.y - kTipGap - sz.h; }

        // Clamp inside the host with a small keep-out from the edges.
        const float maxX = std::max(kTipEdgeInset, host.w - sz.w - kTipEdgeInset);
        const float maxY = std::max(kTipEdgeInset, host.h - sz.h - kTipEdgeInset);
        x = std::clamp(x, kTipEdgeInset, maxX);
        y = std::clamp(y, kTipEdgeInset, maxY);
        layout({x, y, sz.w, sz.h});
    }

    /**
     * @brief Paints the opaque elevated bubble with a hairline outline and the caption.
     */
    void paintSelf(Canvas& c) override {
        const Theme& th = c.theme();
        const Rect r = bounds();
        if (r.isEmpty()) { return; }

        // Soft shadow first so the bubble reads as floating above the content.
        c.drawShadow(r, kTipRadius, 10.0f, th.shadow, 2.0f);
        c.fillRoundedRect(r, kTipRadius, th.elevatedOpaque);

        // Hairline outline centred on the edge (half a pixel in from each side).
        const float half = c.scale().hairline() * 0.5f;
        c.strokeRoundedRect(r.inset(half), kTipRadius - half, th.separatorStrong, 0.0f);

        // Caption text, vertically centred, at most three lines.
        const Rect textRect = r.inset(Insets::symmetric(kTipPadX, kTipPadY));
        if (!textRect.isEmpty()) {
            c.drawText(text_, tooltipStyle(), textRect, th.labelPrimary, HAlign::Left, VAlign::Center, Trimming::End, kTipMaxLines);
        }
    }

private:
    std::wstring text_;
    Point anchor_;
    bool showing_ = false;
    bool wasVisible_ = false;
    unsigned generation_ = 0;
};

// ---------------------------------------------------------------------------
// OverlayHost
// ---------------------------------------------------------------------------

/**
 * @brief Creates the toast stack (below) and the tooltip (on top).
 */
OverlayHost::OverlayHost() {
    toasts_ = add(std::make_unique<ToastHost>());
    tooltip_ = add(std::make_unique<TooltipView>());
    if (tooltip_) { tooltip_->setVisible(false); }
}

/**
 * @brief Opens @p content in the window's popup HWND anchored to a root-space rect.
 *
 * The dismiss wrapper handed to the window carries a weak token that lives
 * inside popupDismiss_: a callback arriving late (after this popup was
 * replaced, or after this host was destroyed) finds the token expired and
 * does nothing.
 */
void OverlayHost::showPopup(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                            std::function<void()> onDismiss) {
    if (!content) {
        HH_LOG_WARN(kLog, L"showPopup called without content");
        if (onDismiss) { onDismiss(); }
        return;
    }
    RootView* r = root();
    if (!r) {
        HH_LOG_WARN(kLog, L"showPopup called while detached from a root");
        if (onDismiss) { onDismiss(); }
        return;
    }

    // Only one popup at a time: closing the old one runs its dismiss callback.
    if (popupOpen_) { dismissPopup(); }
    hideTooltip();

    // Bound the content by the work area of the monitor that hosts the window
    // so a long menu can decide to scroll instead of running off the screen.
    Size maxSize{1e9f, 1e9f};
    const Size work = r->window().workAreaSizeDips();
    if (work.w > 0.0f && work.h > 0.0f) {
        maxSize = {std::max(0.0f, work.w - 16.0f), std::max(0.0f, work.h - 16.0f)};
    }
    Size contentSize = content->measure(Constraints::loose(maxSize));
    contentSize.w = std::max(0.0f, contentSize.w);
    contentSize.h = std::max(0.0f, contentSize.h);

    // The token lives in popupDismiss_; the window's wrapper only holds a weak reference.
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> weak = token;
    popupDismiss_ = [token, user = std::move(onDismiss)]() { if (user) { user(); } };
    popupOpen_ = true;

    r->window().showPopupWindow(std::move(content), anchorRoot, placement, contentSize, [this, weak]() {
        if (weak.expired()) { return; }   // belongs to an earlier popup (or a dead host)
        popupOpen_ = false;
        std::function<void()> fn = std::move(popupDismiss_);
        popupDismiss_ = {};
        if (fn) { fn(); }
    });
}

/**
 * @brief Closes the popup window (if any) and runs the pending dismiss callback once.
 */
void OverlayHost::dismissPopup() {
    if (!popupOpen_) { return; }
    if (RootView* r = root()) { r->window().dismissPopupWindow(); }

    // The window normally calls our wrapper synchronously; finish up if it did not.
    if (popupOpen_) {
        popupOpen_ = false;
        std::function<void()> fn = std::move(popupDismiss_);
        popupDismiss_ = {};
        if (fn) { fn(); }
    }
}

/**
 * @brief Shows the tooltip bubble near a root-space anchor point.
 */
void OverlayHost::showTooltip(const std::wstring& text, Point rootAnchor) {
    if (!tooltip_) { return; }
    if (text.empty()) { hideTooltip(); return; }
    tooltip_->show(text, fromRoot(rootAnchor));
    tooltip_->place(bounds());
    invalidate();
}

/**
 * @brief Fades the tooltip out.
 */
void OverlayHost::hideTooltip() {
    if (!tooltip_) { return; }
    tooltip_->hide();
}

/**
 * @brief True while the tooltip is shown (including its fade-in).
 */
bool OverlayHost::tooltipVisible() const noexcept {
    return tooltip_ && tooltip_->visible() && tooltip_->showing();
}

/**
 * @brief The overlay always fills whatever it is given.
 */
Size OverlayHost::measure(const Constraints& c) {
    const float w = c.hasBoundedWidth() ? c.maxW : 0.0f;
    const float h = c.hasBoundedHeight() ? c.maxH : 0.0f;
    return c.constrain({std::max(0.0f, w), std::max(0.0f, h)});
}

/**
 * @brief Z-stack layout: toasts get the full bounds, the tooltip its anchored spot.
 */
void OverlayHost::layout(const Rect& frame) {
    frame_ = frame;
    frame_.w = std::max(0.0f, frame_.w);
    frame_.h = std::max(0.0f, frame_.h);
    const Rect full = bounds();

    // The toast host positions its own stack against the bottom edge.
    if (toasts_) {
        toasts_->measure(Constraints::tight(full.size()));
        toasts_->layout(full);
    }
    if (tooltip_ && tooltip_->visible()) {
        tooltip_->place(full);
    }
    onLayout();
}

/**
 * @brief Only the toasts can take input; everything else falls through to the content.
 */
Widget* OverlayHost::hitTest(Point local) {
    if (!visible() || !enabled() || !bounds().contains(local)) { return nullptr; }

    // Topmost first; the tooltip is not interactive so it never claims the point.
    for (auto it = children().rbegin(); it != children().rend(); ++it) {
        Widget* child = it->get();
        if (!child || !child->visible()) { continue; }
        if (Widget* hit = child->hitTest(local - child->frame().origin())) { return hit; }
    }
    return nullptr;
}

} // namespace hh::ui
