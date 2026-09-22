// ---------------------------------------------------------------------------
// MacWindowHost.mm - the main NSWindow (see MacWindowHost.h).
//
//   NSWindow (titled, full-size content view, transparent title bar)
//     NSVisualEffectView   under-window-background material (Mica's cousin)
//       HHCanvasView       the toolkit draws everything here
//
// Translucency follows the theme: a translucent theme clears the canvas so
// the material shows through; Reduce Transparency, High Contrast and the
// opaque themes paint the window background colour instead and the material
// is switched off.
//
// System state is observed rather than polled: the app's effective
// appearance (Light / Dark / Auto) through KVO, the accent colour through
// NSSystemColorsDidChangeNotification and the accessibility display options
// through NSWorkspace - the Win32 host's WM_SETTINGCHANGE counterparts.
// ---------------------------------------------------------------------------
#include "ui/mac/MacWindowHost.h"

#include "core/Logger.h"
#include "platform/Utf.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace hh::ui {
namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"MacWindowHost";

/// Prefix of our placement strings (other formats are ignored).
constexpr std::wstring_view kPlacementPrefix = L"mac:";

/// At least this much of a restored window must be on screen to be grabbable.
constexpr CGFloat kMinVisiblePoints = 64.0;

/// Gap between the zoom button and the first piece of content.
constexpr CGFloat kTrafficLightsTrailingGap = 12.0;

/// KVO context for NSApp.effectiveAppearance.
int g_appearanceContext = 0;

/// DipScale for a backing scale factor (1.0 = 96 "dpi", like Windows at 100 %).
DipScale scaleFor(CGFloat backing) {
    const float s = backing > 0.0 && std::isfinite(backing) ? static_cast<float>(backing) : 1.0f;
    return DipScale::fromDpi(96.0f * s);
}

/// Wide -> NSString (never nil).
NSString* ns(std::wstring_view text) {
    const std::string utf8 = platform::toUtf8(text);
    return [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding] ?: @"";
}

/// NSString -> wide.
std::wstring wide(NSString* s) {
    if (s == nil) {
        return {};
    }
    const char* utf8 = s.UTF8String;
    return utf8 != nullptr ? platform::toWide(utf8) : std::wstring();
}

} // namespace
} // namespace hh::ui

// ===========================================================================
// HHWindowDelegate - NSWindowDelegate + system-change observer
// ===========================================================================

@interface HHWindowDelegate : NSObject <NSWindowDelegate>
- (instancetype)initWithHost:(hh::ui::MacWindowHost*)host;
/// Stops observing and forgets the host.
- (void)detach;
@end

@implementation HHWindowDelegate {
    hh::ui::MacWindowHost* host_;
    BOOL observingAppearance_;
}

- (instancetype)initWithHost:(hh::ui::MacWindowHost*)host {
    self = [super init];
    if (self != nil) {
        host_ = host;
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        // Accent colour (System Settings > Appearance).
        [center addObserver:self selector:@selector(systemChanged:) name:NSSystemColorsDidChangeNotification object:nil];
        // Displays added / removed / rearranged: the work area and scale may change.
        [center addObserver:self
                   selector:@selector(screensChanged:)
                       name:NSApplicationDidChangeScreenParametersNotification
                     object:nil];
        // Reduce Motion / Reduce Transparency / Increase Contrast.
        [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:self
                                                               selector:@selector(systemChanged:)
                                                                   name:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
                                                                 object:nil];
        // Light / Dark / Auto.
        if (NSApp != nil) {
            [NSApp addObserver:self forKeyPath:@"effectiveAppearance" options:0 context:&hh::ui::g_appearanceContext];
            observingAppearance_ = YES;
        }
    }
    return self;
}

- (void)detach {
    host_ = nullptr;
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:self];
    if (observingAppearance_ && NSApp != nil) {
        [NSApp removeObserver:self forKeyPath:@"effectiveAppearance" context:&hh::ui::g_appearanceContext];
        observingAppearance_ = NO;
    }
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context {
    if (context != &hh::ui::g_appearanceContext) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }
    // KVO may fire mid-transaction: act on the next run-loop turn.
    __weak HHWindowDelegate* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        HHWindowDelegate* strong = weakSelf;
        if (strong != nil && strong->host_ != nullptr) {
            strong->host_->handleSystemSettingsChanged();
        }
    });
}

- (void)systemChanged:(NSNotification*)note {
    if (host_ != nullptr) {
        host_->handleSystemSettingsChanged();
    }
}

- (void)screensChanged:(NSNotification*)note {
    if (host_ != nullptr) {
        host_->handleResize();
    }
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    // The app decides between hiding to the menu bar and quitting.
    if (host_ != nullptr) {
        host_->handleCloseRequest();
    }
    return NO;
}

- (void)windowDidBecomeKey:(NSNotification*)note {
    if (host_ != nullptr) { host_->handleActivation(true); }
}

- (void)windowDidResignKey:(NSNotification*)note {
    if (host_ != nullptr) { host_->handleActivation(false); }
}

- (void)windowDidResize:(NSNotification*)note {
    if (host_ != nullptr) { host_->handleResize(); }
}

- (void)windowDidEndLiveResize:(NSNotification*)note {
    if (host_ != nullptr) { host_->handleResize(); }
}

- (void)windowDidChangeScreen:(NSNotification*)note {
    if (host_ != nullptr) { host_->handleResize(); }
}

- (void)windowDidMiniaturize:(NSNotification*)note {
    if (host_ != nullptr) { host_->handleMiniaturized(true); }
}

- (void)windowDidDeminiaturize:(NSNotification*)note {
    if (host_ != nullptr) { host_->handleMiniaturized(false); }
}

- (void)windowWillEnterFullScreen:(NSNotification*)note {
    if (host_ != nullptr) { host_->handleFullScreen(true); }
}

- (void)windowDidExitFullScreen:(NSNotification*)note {
    if (host_ != nullptr) { host_->handleFullScreen(false); }
}

@end

namespace hh::ui {

// ===========================================================================
// MacWindowHost
// ===========================================================================

/**
 * @brief The Cocoa objects behind the host.
 */
struct MacWindowHost::Objc {
    NSWindow* window = nil;
    NSVisualEffectView* backdrop = nil;
    HHCanvasView* view = nil;
    HHWindowDelegate* delegate = nil;
    NSString* lastPlacement = nil;    ///< survives destroy() for placementString()
};

MacWindowHost::MacWindowHost(TextCache& text, ThemeManager& themes)
    : objc_(std::make_unique<Objc>()), text_(text), themes_(themes) {
    root_ = std::make_unique<RootView>(*this, themes_);
    popup_ = std::make_unique<MacPopupWindow>(text_, themes_);
    scale_ = scaleFor(1.0);
    clock_ = std::make_unique<mac::FrameClock>([this] { pump(); });
}

MacWindowHost::~MacWindowHost() {
    destroy();
    clock_.reset();
    popup_.reset();
    root_.reset();
}

void* MacWindowHost::nsWindow() const noexcept {
    return (__bridge void*)objc_->window;
}

// ---------------------------------------------------------------------------
// Creation / destruction
// ---------------------------------------------------------------------------

bool MacWindowHost::create(const MacWindowSpec& spec) {
    if (objc_->window != nil) {
        return true;
    }
    spec_ = spec;
    @autoreleasepool {
        const NSRect content = NSMakeRect(0, 0, std::max(spec.initialDips.w, spec.minSizeDips.w),
                                          std::max(spec.initialDips.h, spec.minSizeDips.h));
        // A real titled window (Mission Control, Stage Manager, tiling, the
        // Window menu and VoiceOver all see a normal window), drawn edge to edge.
        const NSWindowStyleMask mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                                       NSWindowStyleMaskFullSizeContentView;
        NSWindow* window = [[NSWindow alloc] initWithContentRect:content
                                                       styleMask:mask
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        if (window == nil) {
            HH_LOG_ERROR(kLog, L"NSWindow creation failed");
            return false;
        }
        window.title = ns(spec.title);
        window.titleVisibility = NSWindowTitleHidden;
        window.titlebarAppearsTransparent = YES;
        window.releasedWhenClosed = NO;
        window.contentMinSize = NSMakeSize(spec.minSizeDips.w, spec.minSizeDips.h);
        window.tabbingMode = NSWindowTabbingModeDisallowed;
        window.collectionBehavior = NSWindowCollectionBehaviorManaged | NSWindowCollectionBehaviorFullScreenPrimary;
        window.animationBehavior = NSWindowAnimationBehaviorDocumentWindow;

        // Backdrop material behind the canvas.
        NSVisualEffectView* backdrop = [[NSVisualEffectView alloc] initWithFrame:content];
        backdrop.material = NSVisualEffectMaterialUnderWindowBackground;
        backdrop.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        backdrop.state = NSVisualEffectStateFollowsWindowActiveState;
        backdrop.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        HHCanvasView* view = [[HHCanvasView alloc] initWithFrame:backdrop.bounds sink:this];
        view.dragsWindowOnCaption = YES;
        [backdrop addSubview:view];
        window.contentView = backdrop;
        window.initialFirstResponder = view;
        [window makeFirstResponder:view];

        HHWindowDelegate* delegate = [[HHWindowDelegate alloc] initWithHost:this];
        window.delegate = delegate;

        objc_->window = window;
        objc_->backdrop = backdrop;
        objc_->view = view;
        objc_->delegate = delegate;
        scale_ = scaleFor(window.backingScaleFactor);
    }

    setAlwaysOnTop(spec.alwaysOnTop);
    centreOnMainScreen();
    if (!popup_->create(nsWindow())) {
        HH_LOG_WARN(kLog, L"popup panel unavailable; menus will not open");
    }
    root_->timeline().setReducedMotion(ThemeManager::readReducedMotion());
    refreshBackdrop();
    layoutTrafficLights();
    HH_LOG_INFO(kLog, L"main window created ({}x{} pt, scale {})", spec.initialDips.w, spec.initialDips.h, scale_.scale);
    return true;
}

void MacWindowHost::destroy() {
    tickTimer_.stop();
    if (clock_) {
        clock_->cancel();
    }
    if (popup_) {
        popup_->destroy();
    }
    if (objc_->window == nil) {
        return;
    }
    @autoreleasepool {
        objc_->lastPlacement = [objc_->window stringWithSavedFrame];
        [objc_->delegate detach];
        [objc_->view detachSink];
        objc_->window.delegate = nil;
        [objc_->window orderOut:nil];
        [objc_->window close];
        objc_->window = nil;
        objc_->backdrop = nil;
        objc_->view = nil;
        objc_->delegate = nil;
    }
}

// ---------------------------------------------------------------------------
// Window state
// ---------------------------------------------------------------------------

void MacWindowHost::show(bool activate) {
    NSWindow* window = objc_->window;
    if (window == nil) {
        return;
    }
    if (window.isMiniaturized) {
        [window deminiaturize:nil];
    }
    if (root_->timeline().paused()) {
        root_->timeline().resume();
    }
    // Paint before the reveal so the first visible frame is current.
    pacer_.requestFrame();
    [objc_->view setNeedsDisplay:YES];
    [objc_->view displayIfNeeded];
    if (activate) {
        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyAndOrderFront:nil];
    } else {
        [window orderFrontRegardless];
    }
    layoutTrafficLights();
    requestFrame();
}

void MacWindowHost::hide() {
    if (objc_->window == nil) {
        return;
    }
    dismissPopupWindow();
    root_->cancelInteraction();
    [objc_->window orderOut:nil];
    // Hidden: freeze the clock (WM_SHOWWINDOW parity).
    if (!root_->timeline().paused()) {
        root_->timeline().pause();
    }
}

void MacWindowHost::minimize() {
    if (objc_->window != nil) {
        [objc_->window miniaturize:nil];
    }
}

void MacWindowHost::toggleZoom() {
    if (objc_->window != nil) {
        [objc_->window zoom:nil];
    }
}

bool MacWindowHost::visible() const {
    NSWindow* window = objc_->window;
    return window != nil && (window.isVisible || window.isMiniaturized);
}

bool MacWindowHost::minimized() const {
    return objc_->window != nil && objc_->window.isMiniaturized;
}

bool MacWindowHost::zoomed() const {
    return objc_->window != nil && objc_->window.isZoomed;
}

void MacWindowHost::bringToFront() {
    if (objc_->window == nil) {
        return;
    }
    [NSApp activateIgnoringOtherApps:YES];
    [objc_->window makeKeyAndOrderFront:nil];
}

void MacWindowHost::setAlwaysOnTop(bool on) {
    alwaysOnTop_ = on;
    if (objc_->window != nil) {
        objc_->window.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel;
    }
}

void MacWindowHost::refreshBackdrop() {
    // Reduce Transparency switches the material off, like "Transparency
    // effects" does for Mica.
    const bool reduce = [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceTransparency];
    themes_.setBackdropAvailable(!reduce);

    const Theme& theme = themes_.current();
    if (objc_->window != nil) {
        // The window's appearance follows the theme (traffic lights, material,
        // open panels), including a forced Light / Dark setting.
        objc_->window.appearance = [NSAppearance appearanceNamed:theme.isDark ? NSAppearanceNameDarkAqua
                                                                              : NSAppearanceNameAqua];
    }
    if (objc_->backdrop != nil) {
        objc_->backdrop.state = theme.translucent ? NSVisualEffectStateFollowsWindowActiveState
                                                  : NSVisualEffectStateInactive;
        objc_->backdrop.hidden = !theme.translucent;
    }
    requestFrame();
}

// ---------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------

std::wstring MacWindowHost::placementString() const {
    NSString* saved = objc_->window != nil ? [objc_->window stringWithSavedFrame] : objc_->lastPlacement;
    if (saved.length == 0) {
        return {};
    }
    return std::wstring(kPlacementPrefix) + wide(saved);
}

void MacWindowHost::applyPlacement(const std::wstring& placement) {
    NSWindow* window = objc_->window;
    if (window == nil) {
        return;
    }
    // Other platforms' strings (or junk) centre the window.
    if (placement.size() <= kPlacementPrefix.size() || placement.compare(0, kPlacementPrefix.size(), kPlacementPrefix) != 0) {
        if (!placement.empty()) {
            HH_LOG_INFO(kLog, L"saved placement is not a macOS placement; centring");
        }
        centreOnMainScreen();
        return;
    }
    [window setFrameFromString:ns(std::wstring_view(placement).substr(kPlacementPrefix.size()))];

    // Enforce the minimum size, then make sure enough of it is on a screen.
    NSRect frame = window.frame;
    const NSRect minFrame = [window frameRectForContentRect:NSMakeRect(0, 0, spec_.minSizeDips.w, spec_.minSizeDips.h)];
    frame.size.width = std::max(frame.size.width, minFrame.size.width);
    frame.size.height = std::max(frame.size.height, minFrame.size.height);
    bool onScreen = false;
    for (NSScreen* screen in [NSScreen screens]) {
        const NSRect overlap = NSIntersectionRect(frame, screen.visibleFrame);
        if (NSWidth(overlap) >= kMinVisiblePoints && NSHeight(overlap) >= kMinVisiblePoints) {
            onScreen = true;
            break;
        }
    }
    if (!onScreen) {
        HH_LOG_INFO(kLog, L"saved placement is off screen; centring");
        centreOnMainScreen();
        return;
    }
    [window setFrame:frame display:NO];
}

void MacWindowHost::centreOnMainScreen() {
    NSWindow* window = objc_->window;
    if (window == nil) {
        return;
    }
    NSScreen* screen = [NSScreen mainScreen] ?: [NSScreen screens].firstObject;
    const NSSize content = NSMakeSize(std::max(spec_.initialDips.w, spec_.minSizeDips.w),
                                      std::max(spec_.initialDips.h, spec_.minSizeDips.h));
    NSRect frame = [window frameRectForContentRect:NSMakeRect(0, 0, content.width, content.height)];
    if (screen != nil) {
        const NSRect work = screen.visibleFrame;
        frame.size.width = std::min(frame.size.width, NSWidth(work));
        frame.size.height = std::min(frame.size.height, NSHeight(work));
        frame.origin.x = std::round(NSMinX(work) + (NSWidth(work) - NSWidth(frame)) / 2.0);
        frame.origin.y = std::round(NSMinY(work) + (NSHeight(work) - NSHeight(frame)) / 2.0);
    }
    [window setFrame:frame display:NO];
}

// ---------------------------------------------------------------------------
// Traffic lights
// ---------------------------------------------------------------------------

void MacWindowHost::layoutTrafficLights() {
    NSWindow* window = objc_->window;
    if (window == nil) {
        return;
    }
    // Full screen: the buttons live in the hidden toolbar window.
    if (fullScreen_ || (window.styleMask & NSWindowStyleMaskFullScreen) != 0) {
        if (trafficLightsInset_ != 0.0f) {
            trafficLightsInset_ = 0.0f;
            if (onTrafficLightsChanged) {
                onTrafficLightsChanged(0.0f);
            }
        }
        return;
    }
    NSButton* close = [window standardWindowButton:NSWindowCloseButton];
    NSButton* mini = [window standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoom = [window standardWindowButton:NSWindowZoomButton];
    NSView* titleBar = close.superview;
    NSView* container = titleBar.superview;
    if (close == nil || mini == nil || zoom == nil || titleBar == nil || container == nil) {
        return;
    }

    // The title-bar container spans the whole top bar...
    const CGFloat barH = std::max<CGFloat>(spec_.topBarHeight, NSHeight(close.frame));
    NSRect containerFrame = container.frame;
    containerFrame.size.height = barH;
    containerFrame.origin.y = NSHeight(window.frame) - barH;
    if (!NSEqualRects(containerFrame, container.frame)) {
        container.frame = containerFrame;
    }
    if (!NSEqualRects(titleBar.frame, container.bounds)) {
        titleBar.frame = container.bounds;
    }

    // ...and the buttons sit vertically centred in it, with the same
    // padding on the left as above and below.
    const CGFloat bw = NSWidth(close.frame);
    const CGFloat bh = NSHeight(close.frame);
    CGFloat pitch = NSMinX(mini.frame) - NSMinX(close.frame);
    if (!(pitch > bw)) {
        pitch = bw + 6.0;
    }
    const CGFloat pad = std::round((barH - bh) / 2.0);
    const CGFloat y = std::round((NSHeight(titleBar.bounds) - bh) / 2.0);
    NSArray<NSButton*>* buttons = @[ close, mini, zoom ];
    for (NSUInteger i = 0; i < buttons.count; ++i) {
        const NSPoint origin = NSMakePoint(pad + 1.0 + static_cast<CGFloat>(i) * pitch, y);
        if (!NSEqualPoints(buttons[i].frame.origin, origin)) {
            [buttons[i] setFrameOrigin:origin];
        }
    }

    // Tell the content how much room to leave.
    const float inset = static_cast<float>(pad + 1.0 + 2.0 * pitch + bw + kTrafficLightsTrailingGap);
    if (std::abs(inset - trafficLightsInset_) > 0.5f) {
        trafficLightsInset_ = inset;
        if (onTrafficLightsChanged) {
            onTrafficLightsChanged(inset);
        }
    }
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

void MacWindowHost::requestFrame() {
    pacer_.requestFrame();
    if (clock_) {
        clock_->wakeIn(0.0);
    }
}

void MacWindowHost::pump() {
    if (!root_) {
        return;
    }
    NSWindow* window = objc_->window;
    const bool shown = window != nil && window.isVisible && !window.isMiniaturized;
    if (root_->tickAnimations()) {
        pacer_.requestFrame();
    }
    if (shown && pacer_.consume() && objc_->view != nil) {
        [objc_->view setNeedsDisplay:YES];
    }

    // Next wake-up: the frame cadence while something moves, else the next
    // timeline timer. A paused (hidden) clock waits for show().
    Timeline& tl = root_->timeline();
    if (tl.paused()) {
        return;
    }
    if (shown && tl.hasActive()) {
        clock_->wakeIn(mac::FrameClock::kFrameIntervalSec);
    } else if (const std::optional<double> next = tl.nextDeadline()) {
        clock_->wakeIn(std::max(0.0, *next - tl.now()));
    }
}

void MacWindowHost::startTicking() {
    tickTimer_.start(1.0, [this] {
        if (onTick) {
            onTick();
        }
    });
}

void MacWindowHost::paint(CGContextRef ctx, Size sizeDips) {
    if (ctx == nullptr || !root_) {
        return;
    }
    root_->layoutIfNeeded(sizeDips);
    const Theme& theme = themes_.current();

    // Translucent themes clear so the material shows through; the others
    // paint the opaque window colour.
    CGContextClearRect(ctx, CGRectMake(0, 0, sizeDips.w, sizeDips.h));
    canvas_.begin(ctx, scale_, theme, text_);
    if (!theme.translucent) {
        canvas_.fillRect(Rect{0.0f, 0.0f, sizeDips.w, sizeDips.h}, theme.windowBackground);
    }
    root_->paintAll(canvas_);
    canvas_.end();
}

void MacWindowHost::viewDraw(CGContextRef ctx, Size sizeDips) {
    pacer_.consume();
    paint(ctx, sizeDips);
}

bool MacWindowHost::captureFrame(std::vector<uint8_t>& bgra, UINT& w, UINT& h, float backingScale) {
    Size size = clientSizeDips();
    if (size.isEmpty()) {
        size = spec_.initialDips;
    }
    const float s = backingScale > 0.0f ? backingScale : scale_.scale;
    w = static_cast<UINT>(std::max(1L, std::lround(size.w * s)));
    h = static_cast<UINT>(std::max(1L, std::lround(size.h * s)));
    bgra.assign(static_cast<size_t>(w) * h * 4, 0);

    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef bmp = space != nullptr
                           ? CGBitmapContextCreate(bgra.data(), w, h, 8, static_cast<size_t>(w) * 4, space,
                                                   kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little)
                           : nullptr;
    if (space != nullptr) {
        CGColorSpaceRelease(space);
    }
    if (bmp == nullptr) {
        HH_LOG_ERROR(kLog, L"captureFrame: bitmap context creation failed ({}x{})", w, h);
        return false;
    }
    // Same space as the view: y down, one unit per dip.
    CGContextTranslateCTM(bmp, 0, static_cast<CGFloat>(h));
    CGContextScaleCTM(bmp, s, -s);

    // Pixel snapping follows the capture's own scale for this frame.
    const DipScale saved = scale_;
    scale_ = scaleFor(s);
    paint(bmp, size);
    scale_ = saved;
    CGContextRelease(bmp);
    return true;
}

// ---------------------------------------------------------------------------
// Delegate callbacks
// ---------------------------------------------------------------------------

void MacWindowHost::handleCloseRequest() {
    if (onCloseRequested) {
        onCloseRequested();
    } else {
        hide();
    }
}

void MacWindowHost::handleActivation(bool active) {
    if (root_) {
        root_->setWindowActive(active);
        if (!active) {
            root_->cancelInteraction();
        }
    }
    layoutTrafficLights();
    if (onActivate) {
        onActivate(active);
    }
    requestFrame();
}

void MacWindowHost::handleResize() {
    layoutTrafficLights();
    requestFrame();
}

void MacWindowHost::handleMiniaturized(bool miniaturized) {
    if (!root_) {
        return;
    }
    if (miniaturized) {
        if (!root_->timeline().paused()) {
            root_->timeline().pause();
        }
        return;
    }
    if (root_->timeline().paused()) {
        root_->timeline().resume();
    }
    requestFrame();
}

void MacWindowHost::handleFullScreen(bool fullScreen) {
    fullScreen_ = fullScreen;
    if (!fullScreen) {
        // Force a report: the inset went to 0 on the way in.
        trafficLightsInset_ = -1.0f;
    }
    layoutTrafficLights();
    requestFrame();
}

void MacWindowHost::handleSystemSettingsChanged() {
    if (root_) {
        root_->timeline().setReducedMotion(ThemeManager::readReducedMotion());
    }
    if (onSystemSettingsChanged) {
        onSystemSettingsChanged();
    }
    refreshBackdrop();
    requestFrame();
}

void MacWindowHost::titleBarDoubleClick() {
    NSWindow* window = objc_->window;
    if (window == nil) {
        return;
    }
    // System Settings > Desktop & Dock > "Double-click a window's title bar to".
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSString* action = [defaults stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"None"]) {
        return;
    }
    if ([action isEqualToString:@"Minimize"] ||
        (action == nil && [defaults boolForKey:@"AppleMiniaturizeOnDoubleClick"])) {
        [window performMiniaturize:nil];
        return;
    }
    [window performZoom:nil];
}

// ---------------------------------------------------------------------------
// IWindowServices
// ---------------------------------------------------------------------------

void MacWindowHost::setCursor(CursorKind cursor) {
    static_cast<void>(cursor);
    if (objc_->view != nil) {
        [objc_->view refreshCursor];
    }
}

Size MacWindowHost::clientSizeDips() const {
    if (objc_->view == nil) {
        return {0.0f, 0.0f};
    }
    const NSSize s = objc_->view.bounds.size;
    return {static_cast<float>(s.width), static_cast<float>(s.height)};
}

Size MacWindowHost::workAreaSizeDips() const {
    NSScreen* screen = objc_->window.screen ?: [NSScreen mainScreen];
    if (screen == nil) {
        return {0.0f, 0.0f};
    }
    const NSRect work = screen.visibleFrame;
    return {static_cast<float>(NSWidth(work)), static_cast<float>(NSHeight(work))};
}

bool MacWindowHost::isActiveWindow() const {
    return objc_->window != nil && objc_->window.isKeyWindow;
}

void MacWindowHost::showPopupWindow(std::unique_ptr<Widget> content, const Rect& anchorRoot, PopupPlacement placement,
                                    Size contentSize, std::function<void()> onDismiss) {
    if (!popup_ || objc_->window == nil || objc_->view == nil) {
        // No popup panel: tell the caller it closed so its state resets.
        HH_LOG_WARN(kLog, L"showPopupWindow: popup panel unavailable");
        if (onDismiss) {
            onDismiss();
        }
        return;
    }
    const NSRect inWindow = [objc_->view convertRect:NSMakeRect(anchorRoot.x, anchorRoot.y, anchorRoot.w, anchorRoot.h)
                                              toView:nil];
    const NSRect onScreen = [objc_->window convertRectToScreen:inWindow];
    popup_->show(std::move(content), onScreen, placement, contentSize, std::move(onDismiss));
}

void MacWindowHost::dismissPopupWindow() {
    if (popup_ && popup_->visible()) {
        popup_->dismiss();
    }
}

bool MacWindowHost::popupWindowVisible() const {
    return popup_ && popup_->visible();
}

void MacWindowHost::setImeCaret(const Rect& caretRoot) {
    imeCaret_ = caretRoot;
    if (objc_->view != nil) {
        [[objc_->view inputContext] invalidateCharacterCoordinates];
    }
}

void MacWindowHost::setImeEnabled(bool enabled) {
    if (objc_->view != nil) {
        objc_->view.imeEnabled = enabled ? YES : NO;
    }
}

bool MacWindowHost::clipboardSetText(const std::wstring& text) {
    return mac::setClipboardText(text);
}

std::wstring MacWindowHost::clipboardGetText() {
    return mac::clipboardText();
}

// ---------------------------------------------------------------------------
// mac::IViewSink
// ---------------------------------------------------------------------------

void MacWindowHost::viewMouseMove(Point pt, Modifiers mods) {
    if (root_) {
        root_->dispatchMouseMove(pt, mods);
    }
}

bool MacWindowHost::viewMouseDown(Point pt, MouseButton button, Modifiers mods, int clickCount) {
    if (!root_) {
        return true;
    }
    // The top bar's empty space is the title bar (HTCAPTION parity).
    if (button == MouseButton::Left && root_->chromeHitTest(pt) == ChromeHit::Caption) {
        if (clickCount == 2) {
            titleBarDoubleClick();
            return true;
        }
        return false;   // the view starts a native window drag
    }
    root_->dispatchMouseDown(pt, button, mods, clickCount);
    return true;
}

void MacWindowHost::viewMouseUp(Point pt, MouseButton button, Modifiers mods) {
    if (root_) {
        root_->dispatchMouseUp(pt, button, mods);
    }
}

void MacWindowHost::viewMouseExited() {
    if (root_) {
        root_->dispatchMouseLeave();
    }
}

void MacWindowHost::viewWheel(Point pt, float delta, float deltaX, bool precise, Modifiers mods) {
    if (root_) {
        root_->dispatchWheel(pt, delta, deltaX, precise, 3, mods);
    }
}

bool MacWindowHost::viewKeyDown(UINT vk, Modifiers mods, bool repeat) {
    // An open popup gets every key; the window underneath none.
    if (popup_ && popup_->visible()) {
        return popup_->forwardKeyDown(vk, mods, repeat);
    }
    return root_ && root_->dispatchKeyDown(vk, mods, repeat);
}

bool MacWindowHost::viewKeyUp(UINT vk, Modifiers mods) {
    return root_ && root_->dispatchKeyUp(vk, mods);
}

void MacWindowHost::viewChar(char32_t ch) {
    if (popup_ && popup_->visible()) {
        popup_->forwardChar(ch);
        return;
    }
    if (root_) {
        root_->dispatchChar(ch);
    }
}

CursorKind MacWindowHost::viewCursor() const {
    return root_ ? root_->currentCursor() : CursorKind::Arrow;
}

void MacWindowHost::viewFilesDropped(const std::vector<std::wstring>& paths) {
    if (onFilesDropped && !paths.empty()) {
        onFilesDropped(paths);
    }
}

void MacWindowHost::viewBackingChanged(float backingScale) {
    scale_ = scaleFor(backingScale);
    if (root_) {
        root_->dpiChanged();
        root_->invalidateLayout();
    }
    if (onDpiChanged) {
        onDpiChanged();
    }
    requestFrame();
}

void MacWindowHost::viewResized(Size sizeDips) {
    static_cast<void>(sizeDips);
    requestFrame();
}

} // namespace hh::ui
