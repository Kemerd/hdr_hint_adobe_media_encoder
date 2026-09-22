// ---------------------------------------------------------------------------
// MacPopupWindow.mm - the menu panel (see MacPopupWindow.h).
//
// Input model while open (NSMenu parity, and the Win32 popup's behaviour):
//   * a local event monitor sees every click in the app first: a click on
//     the card goes through, anything else (the shadow margin, the owner,
//     the opener itself) dismisses the popup and is swallowed;
//   * a global monitor dismisses on clicks in other apps;
//   * the app resigning active, the owner resigning key, moving, resizing
//     or miniaturising all dismiss;
//   * keys arrive from the owner (forwardKeyDown / forwardChar) because the
//     panel never becomes key.
//
// The panel is transparent and shadowless: PopupSurface paints the soft
// shadow, the opaque rounded card and its hairline itself, so menus look the
// same as on Windows and in the PNG captures.
// ---------------------------------------------------------------------------
#include "ui/mac/MacPopupWindow.h"

#include "core/Logger.h"
#include "ui/core/PopupSurface.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <optional>

/**
 * @brief Borderless panel that never takes key or main status.
 */
@interface HHPopupPanel : NSPanel
@end

@implementation HHPopupPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

namespace hh::ui {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"MacPopupWindow";

/// DipScale for a backing scale factor (1.0 = 96 "dpi", like Windows at 100 %).
DipScale scaleFor(CGFloat backing) {
    const float s = backing > 0.0 && std::isfinite(backing) ? static_cast<float>(backing) : 1.0f;
    return DipScale::fromDpi(96.0f * s);
}

} // namespace

/**
 * @brief The Cocoa side: panel, view and the dismissal observers.
 */
struct MacPopupWindow::Objc {
    HHPopupPanel* panel = nil;
    HHCanvasView* view = nil;
    __weak NSWindow* owner = nil;
    id localMonitor = nil;
    id globalMonitor = nil;
    NSMutableArray<id>* observers = [NSMutableArray array];
};

// ---------------------------------------------------------------------------
// Construction / lifetime
// ---------------------------------------------------------------------------

MacPopupWindow::MacPopupWindow(TextCache& text, ThemeManager& themes)
    : objc_(std::make_unique<Objc>()), text_(text), themes_(themes) {
    root_ = std::make_unique<RootView>(*this, themes_);
    root_->setContent(std::make_unique<PopupSurface>(shadowMargin_));
    scale_ = scaleFor(1.0);
    clock_ = std::make_unique<mac::FrameClock>([this] { pump(); });
}

MacPopupWindow::~MacPopupWindow() {
    destroy();
    *alive_ = false;
    clock_.reset();
}

bool MacPopupWindow::create(void* ownerWindow) {
    if (objc_->panel != nil) {
        return true;
    }
    @autoreleasepool {
        objc_->owner = (__bridge NSWindow*)ownerWindow;

        // Borderless + non-activating: clicks never pull the app forward on
        // their own and the owner keeps the keyboard.
        HHPopupPanel* panel = [[HHPopupPanel alloc] initWithContentRect:NSMakeRect(0, 0, 64, 64)
                                                              styleMask:NSWindowStyleMaskBorderless |
                                                                        NSWindowStyleMaskNonactivatingPanel
                                                                backing:NSBackingStoreBuffered
                                                                  defer:YES];
        if (panel == nil) {
            HH_LOG_ERROR(kLog, L"NSPanel creation failed");
            return false;
        }
        panel.opaque = NO;
        panel.backgroundColor = [NSColor clearColor];
        panel.hasShadow = NO;                         // PopupSurface draws its own
        panel.level = NSPopUpMenuWindowLevel;
        panel.hidesOnDeactivate = NO;                 // dismissed explicitly instead
        panel.releasedWhenClosed = NO;
        panel.becomesKeyOnlyIfNeeded = YES;
        panel.floatingPanel = YES;
        panel.animationBehavior = NSWindowAnimationBehaviorNone;
        panel.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenAuxiliary |
                                   NSWindowCollectionBehaviorIgnoresCycle;

        HHCanvasView* view = [[HHCanvasView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64) sink:this];
        panel.contentView = view;
        objc_->panel = panel;
        objc_->view = view;
        scale_ = scaleFor(panel.backingScaleFactor);
    }
    HH_LOG_DEBUG(kLog, L"popup panel created");
    return true;
}

void MacPopupWindow::destroy() {
    if (visible_) {
        dismiss();
    }
    // Retired menus can go now: none of their handlers is on the stack.
    if (root_) {
        if (auto* surface = static_cast<PopupSurface*>(root_->content())) {
            surface->retireMenu();
            surface->purgeRetired();
        }
    }
    removeMonitors();
    if (objc_->view != nil) {
        [objc_->view detachSink];
    }
    if (objc_->panel != nil) {
        [objc_->panel orderOut:nil];
        objc_->panel.contentView = nil;
    }
    objc_->view = nil;
    objc_->panel = nil;
}

// ---------------------------------------------------------------------------
// Show / dismiss
// ---------------------------------------------------------------------------

void MacPopupWindow::show(std::unique_ptr<Widget> content, CGRect anchorScreen, PopupPlacement placement, Size sizeDips,
                          std::function<void()> onDismiss) {
    if (objc_->panel == nil || !root_ || !content) {
        HH_LOG_WARN(kLog, L"show: {}; popup dropped", objc_->panel == nil ? L"no panel" : L"no content");
        if (onDismiss) {
            onDismiss();
        }
        return;
    }
    // A popup replacing another one closes the first properly first.
    if (visible_) {
        dismiss();
    }
    auto* surface = static_cast<PopupSurface*>(root_->content());
    if (surface == nullptr) {
        HH_LOG_ERROR(kLog, L"show: popup surface missing");
        if (onDismiss) {
            onDismiss();
        }
        return;
    }

    @autoreleasepool {
        // ---- geometry: Cocoa screen points, y up ------------------------------
        NSScreen* screen = nil;
        for (NSScreen* s in [NSScreen screens]) {
            if (NSIntersectsRect(s.frame, anchorScreen) || NSPointInRect(anchorScreen.origin, s.frame)) {
                screen = s;
                break;
            }
        }
        if (screen == nil) {
            screen = objc_->owner.screen ?: [NSScreen mainScreen];
        }
        const NSRect work = screen != nil ? screen.visibleFrame : NSMakeRect(0, 0, 1440, 900);
        scale_ = scaleFor(screen != nil ? screen.backingScaleFactor : 1.0);
        const CGFloat margin = surface->margin();

        // Content that was not pre-measured measures itself against the work area.
        const float maxW = std::max(1.0f, static_cast<float>(NSWidth(work) - 2.0 * margin));
        const float maxH = std::max(1.0f, static_cast<float>(NSHeight(work) - 2.0 * margin));
        if (sizeDips.isEmpty()) {
            sizeDips = content->measure(Constraints::loose({maxW, maxH}));
        }
        sizeDips.w = std::clamp(sizeDips.w, 1.0f, maxW);
        sizeDips.h = std::clamp(sizeDips.h, 1.0f, maxH);
        const CGFloat w = std::ceil(sizeDips.w);
        const CGFloat h = std::ceil(sizeDips.h);
        const CGFloat gap = kPopupAnchorGap;

        // "Below" means towards smaller y here.
        const CGFloat anchorTop = NSMaxY(anchorScreen);
        const CGFloat anchorBottom = NSMinY(anchorScreen);
        const bool fitsBelow = anchorBottom - gap - h >= NSMinY(work);
        const bool fitsAbove = anchorTop + gap + h <= NSMaxY(work);
        CGFloat cardX = NSMinX(anchorScreen);
        CGFloat cardY = 0.0;   // bottom edge of the card
        switch (placement) {
        case PopupPlacement::AtPoint:
            cardY = anchorTop - h;
            break;
        case PopupPlacement::Above:
            cardY = (fitsAbove || !fitsBelow) ? anchorTop + gap : anchorBottom - gap - h;
            break;
        case PopupPlacement::Below:
        case PopupPlacement::Auto:
        default:
            cardY = (fitsBelow || !fitsAbove) ? anchorBottom - gap - h : anchorTop + gap;
            break;
        }
        // Keep the card on screen; the shadow may hang over the edge.
        cardX = std::clamp(cardX, NSMinX(work), std::max(NSMinX(work), NSMaxX(work) - w));
        cardY = std::clamp(cardY, NSMinY(work), std::max(NSMinY(work), NSMaxY(work) - h));
        const NSRect frame = NSMakeRect(std::round(cardX - margin), std::round(cardY - margin), w + 2.0 * margin,
                                        h + 2.0 * margin);

        // ---- content ------------------------------------------------------------
        Widget* menu = surface->setMenu(std::move(content));
        root_->focus().setKeyboardMode(false);
        if (menu != nullptr) {
            root_->focus().focus(menu);
        }
        onDismiss_ = std::move(onDismiss);
        visible_ = true;

        // Size, paint, then reveal: the first visible frame is already right.
        [objc_->panel setFrame:frame display:NO];
        root_->invalidateLayout();
        pacer_.requestFrame();
        [objc_->view setNeedsDisplay:YES];
        [objc_->view displayIfNeeded];
        [objc_->panel orderFrontRegardless];
        installMonitors();
        clock_->wakeIn(0.0);
        HH_LOG_DEBUG(kLog, L"popup shown at {},{} size {}x{}", NSMinX(frame), NSMinY(frame), NSWidth(frame), NSHeight(frame));
    }
}

void MacPopupWindow::dismiss() {
    if (!visible_) {
        return;
    }
    // Flag first: everything below may re-enter.
    visible_ = false;
    removeMonitors();
    if (objc_->panel != nil) {
        [objc_->panel orderOut:nil];
    }

    // Clear hover / press / focus, then detach the menu. It is destroyed on
    // a later run-loop turn because dismissal usually starts in its own handler.
    if (root_) {
        root_->cancelInteraction();
        root_->focus().focus(nullptr);
        if (auto* surface = static_cast<PopupSurface*>(root_->content())) {
            surface->retireMenu();
        }
    }
    std::weak_ptr<bool> alive = alive_;
    dispatch_async(dispatch_get_main_queue(), ^{
        const auto token = alive.lock();
        if (!token || !*token || !root_) {
            return;
        }
        if (auto* surface = static_cast<PopupSurface*>(root_->content())) {
            surface->purgeRetired();
        }
    });

    // Move the callback out first so a re-entrant show() cannot clobber it.
    std::function<void()> cb = std::move(onDismiss_);
    onDismiss_ = {};
    if (cb) {
        cb();
    }
    HH_LOG_DEBUG(kLog, L"popup dismissed");
}

// ---------------------------------------------------------------------------
// Dismissal observers
// ---------------------------------------------------------------------------

void MacPopupWindow::installMonitors() {
    removeMonitors();
    std::weak_ptr<bool> alive = alive_;
    MacPopupWindow* popup = this;
    const NSEventMask clicks = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown;

    // Clicks inside the app: on the card they go through, anywhere else they
    // close the popup and are swallowed (NSMenu behaviour).
    objc_->localMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:clicks
                                                                handler:^NSEvent*(NSEvent* event) {
                                                                    const auto token = alive.lock();
                                                                    if (!token || !*token || !popup->visible_) {
                                                                        return event;
                                                                    }
                                                                    if (event.window == popup->objc_->panel) {
                                                                        const NSPoint p = [popup->objc_->view
                                                                            convertPoint:event.locationInWindow
                                                                                fromView:nil];
                                                                        const Point pt{static_cast<float>(p.x),
                                                                                       static_cast<float>(p.y)};
                                                                        if (popup->insideCard(pt)) {
                                                                            return event;
                                                                        }
                                                                    }
                                                                    popup->dismiss();
                                                                    return nil;
                                                                }];
    // Clicks in other apps.
    objc_->globalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:clicks
                                                                  handler:^(NSEvent*) {
                                                                      const auto token = alive.lock();
                                                                      if (token && *token) {
                                                                          popup->dismiss();
                                                                      }
                                                                  }];

    // Focus leaving the app or the owner moving away.
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    void (^close)(NSNotification*) = ^(NSNotification*) {
        const auto token = alive.lock();
        if (token && *token) {
            popup->dismiss();
        }
    };
    [objc_->observers addObject:[center addObserverForName:NSApplicationDidResignActiveNotification
                                                    object:nil
                                                     queue:nil
                                                usingBlock:close]];
    NSWindow* owner = objc_->owner;
    if (owner != nil) {
        for (NSNotificationName name in @[ NSWindowDidResignKeyNotification, NSWindowWillMoveNotification,
                                           NSWindowDidResizeNotification, NSWindowWillMiniaturizeNotification,
                                           NSWindowWillCloseNotification ]) {
            [objc_->observers addObject:[center addObserverForName:name object:owner queue:nil usingBlock:close]];
        }
    }
}

void MacPopupWindow::removeMonitors() {
    if (objc_->localMonitor != nil) {
        [NSEvent removeMonitor:objc_->localMonitor];
        objc_->localMonitor = nil;
    }
    if (objc_->globalMonitor != nil) {
        [NSEvent removeMonitor:objc_->globalMonitor];
        objc_->globalMonitor = nil;
    }
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    for (id observer in objc_->observers) {
        [center removeObserver:observer];
    }
    [objc_->observers removeAllObjects];
}

bool MacPopupWindow::insideCard(Point rootPt) const {
    const Size size = clientSizeDips();
    const float m = shadowMargin_;
    return rootPt.x >= m && rootPt.y >= m && rootPt.x < size.w - m && rootPt.y < size.h - m;
}

// ---------------------------------------------------------------------------
// Keyboard forwarding
// ---------------------------------------------------------------------------

bool MacPopupWindow::forwardKeyDown(UINT vk, Modifiers mods, bool repeat) {
    if (!visible_ || !root_) {
        return false;
    }
    if (root_->dispatchKeyDown(vk, mods, repeat)) {
        return true;
    }
    if (vk == VK_ESCAPE) {
        dismiss();
        return true;
    }
    return false;
}

bool MacPopupWindow::forwardChar(char32_t ch) {
    if (!visible_ || !root_) {
        return false;
    }
    return root_->dispatchChar(ch);
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

void MacPopupWindow::requestFrame() {
    pacer_.requestFrame();
    if (clock_) {
        clock_->wakeIn(0.0);
    }
}

void MacPopupWindow::pump() {
    if (!root_ || !visible_) {
        return;
    }
    if (root_->tickAnimations()) {
        pacer_.requestFrame();
    }
    if (pacer_.consume() && objc_->view != nil) {
        [objc_->view setNeedsDisplay:YES];
    }
    // Next wake-up: the frame cadence while something moves, else the next
    // timeline timer.
    Timeline& tl = root_->timeline();
    if (tl.hasActive()) {
        clock_->wakeIn(mac::FrameClock::kFrameIntervalSec);
    } else if (const std::optional<double> next = tl.nextDeadline()) {
        clock_->wakeIn(std::max(0.0, *next - tl.now()));
    }
}

void MacPopupWindow::viewDraw(CGContextRef ctx, Size sizeDips) {
    pacer_.consume();
    if (ctx == nullptr || !root_) {
        return;
    }
    root_->layoutIfNeeded(sizeDips);
    // Transparent clear: the shadow margin and the card corners show what is behind.
    CGContextClearRect(ctx, CGRectMake(0, 0, sizeDips.w, sizeDips.h));
    canvas_.begin(ctx, scale_, themes_.current(), text_);
    root_->paintAll(canvas_);
    canvas_.end();
}

// ---------------------------------------------------------------------------
// IWindowServices
// ---------------------------------------------------------------------------

void MacPopupWindow::setCursor(CursorKind cursor) {
    static_cast<void>(cursor);
    if (objc_->view != nil) {
        [objc_->view refreshCursor];
    }
}

Size MacPopupWindow::clientSizeDips() const {
    if (objc_->view == nil) {
        return {0.0f, 0.0f};
    }
    const NSSize s = objc_->view.bounds.size;
    return {static_cast<float>(s.width), static_cast<float>(s.height)};
}

Size MacPopupWindow::workAreaSizeDips() const {
    NSScreen* screen = objc_->panel.screen ?: [NSScreen mainScreen];
    if (screen == nil) {
        return {0.0f, 0.0f};
    }
    const NSRect work = screen.visibleFrame;
    return {static_cast<float>(NSWidth(work)), static_cast<float>(NSHeight(work))};
}

void MacPopupWindow::showPopupWindow(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                                     Size contentSize, std::function<void()> onDismiss) {
    // Nested popups are not supported: the request replaces this one.
    CGRect anchor = CGRectZero;
    if (objc_->view != nil && objc_->panel != nil) {
        const NSRect inWindow = [objc_->view convertRect:NSMakeRect(anchorRoot.x, anchorRoot.y, anchorRoot.w, anchorRoot.h)
                                                  toView:nil];
        anchor = [objc_->panel convertRectToScreen:inWindow];
    }
    show(std::move(content), anchor, placement, contentSize, std::move(onDismiss));
}

bool MacPopupWindow::clipboardSetText(const std::wstring& text) {
    return mac::setClipboardText(text);
}

std::wstring MacPopupWindow::clipboardGetText() {
    return mac::clipboardText();
}

// ---------------------------------------------------------------------------
// mac::IViewSink
// ---------------------------------------------------------------------------

void MacPopupWindow::viewMouseMove(Point pt, Modifiers mods) {
    if (root_) {
        root_->dispatchMouseMove(pt, mods);
    }
}

bool MacPopupWindow::viewMouseDown(Point pt, MouseButton button, Modifiers mods, int clickCount) {
    // The local monitor already dismissed outside clicks; stay robust anyway.
    if (!insideCard(pt)) {
        dismiss();
        return true;
    }
    if (root_) {
        root_->dispatchMouseDown(pt, button, mods, clickCount);
    }
    return true;
}

void MacPopupWindow::viewMouseUp(Point pt, MouseButton button, Modifiers mods) {
    if (root_) {
        root_->dispatchMouseUp(pt, button, mods);
    }
}

void MacPopupWindow::viewMouseExited() {
    if (root_) {
        root_->dispatchMouseLeave();
    }
}

void MacPopupWindow::viewWheel(Point pt, float delta, float deltaX, bool precise, Modifiers mods) {
    if (root_) {
        root_->dispatchWheel(pt, delta, deltaX, precise, 3, mods);
    }
}

bool MacPopupWindow::viewKeyDown(UINT vk, Modifiers mods, bool repeat) {
    // The panel never becomes key, but be complete.
    return forwardKeyDown(vk, mods, repeat);
}

bool MacPopupWindow::viewKeyUp(UINT vk, Modifiers mods) {
    return root_ && root_->dispatchKeyUp(vk, mods);
}

void MacPopupWindow::viewChar(char32_t ch) {
    forwardChar(ch);
}

CursorKind MacPopupWindow::viewCursor() const {
    return root_ ? root_->currentCursor() : CursorKind::Arrow;
}

void MacPopupWindow::viewBackingChanged(float backingScale) {
    scale_ = scaleFor(backingScale);
    if (root_) {
        root_->dpiChanged();
        root_->invalidateLayout();
    }
    requestFrame();
}

} // namespace hh::ui
