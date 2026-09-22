// ---------------------------------------------------------------------------
// MacView.mm - HHCanvasView, FrameClock and RepeatingTimer (see MacView.h).
//
// Keyboard model (mirrors the Win32 host):
//   * every key press is first offered to the toolkit as a virtual key
//     (WM_KEYDOWN parity), then - unless Command or Control is held - it
//     runs through -interpretKeyEvents: so the keyboard layout, dead keys
//     and input methods produce text (WM_CHAR parity);
//   * while an input method composes (marked text), it owns every key;
//   * Command is the shortcut key (Modifiers::ctrl), and the Mac text
//     idioms are translated at this boundary: Option+Left/Right/Backspace
//     act on words (Ctrl on Windows), Command+Left/Right jump to the line
//     ends (Home/End), and Command+Backspace deletes the selected job when
//     no text field wanted it (Delete on Windows);
//   * Edit-menu actions (copy:, paste:, ...) arrive through the responder
//     chain because the menu claims their key equivalents first; they are
//     replayed as the matching Ctrl chords.
//
// Pointer model: Control-click is a right click (Mac convention), clicks
// count 1/2 the way WM_xBUTTONDOWN / WM_xBUTTONDBLCLK do, and the touchpad
// momentum phase is dropped because the toolkit's ScrollView runs its own
// inertia from the gesture's velocity.
// ---------------------------------------------------------------------------
#include "ui/mac/MacView.h"

#include "core/Logger.h"
#include "platform/Utf.h"

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace hh::ui::mac {

namespace {

/// Component tag for the log.
constexpr const wchar_t* kLog = L"MacView";

/// Touchpad points -> WHEEL_DELTA units: ScrollView moves 48 dips per 120
/// units, so 2.5 units per point makes one point of finger travel one dip.
constexpr float kPreciseUnitsPerPoint = static_cast<float>(WHEEL_DELTA) / 48.0f;

/// "Never" for the frame clock (seconds from now; CF has no infinity).
constexpr double kFarFutureSec = 1.0e9;

// ---- Win32 virtual keys outside the portable subset in InputEvents.h --------
constexpr UINT kVkOemPlus = 0xBB;
constexpr UINT kVkOemMinus = 0xBD;
constexpr UINT kVkOemPeriod = 0xBE;
constexpr UINT kVkOem1 = 0xBA;   // ;:
constexpr UINT kVkOem2 = 0xBF;   // /?
constexpr UINT kVkOem3 = 0xC0;   // `~
constexpr UINT kVkOem4 = 0xDB;   // [{
constexpr UINT kVkOem5 = 0xDC;   // \|
constexpr UINT kVkOem6 = 0xDD;   // ]}
constexpr UINT kVkOem7 = 0xDE;   // '"

// ---- macOS virtual key codes (HIToolbox/Events.h, kVK_*) --------------------
enum : unsigned short {
    kKeyReturn = 0x24,
    kKeyTab = 0x30,
    kKeySpace = 0x31,
    kKeyBackspace = 0x33,        // "delete" on Mac keyboards
    kKeyEscape = 0x35,
    kKeyKeypadEnter = 0x4C,
    kKeyF5 = 0x60, kKeyF6 = 0x61, kKeyF7 = 0x62, kKeyF3 = 0x63, kKeyF8 = 0x64, kKeyF9 = 0x65,
    kKeyF11 = 0x67, kKeyF10 = 0x6D, kKeyF12 = 0x6F,
    kKeyHome = 0x73,
    kKeyPageUp = 0x74,
    kKeyForwardDelete = 0x75,
    kKeyF4 = 0x76,
    kKeyEnd = 0x77,
    kKeyF2 = 0x78,
    kKeyPageDown = 0x79,
    kKeyF1 = 0x7A,
    kKeyLeft = 0x7B,
    kKeyRight = 0x7C,
    kKeyDown = 0x7D,
    kKeyUp = 0x7E,
};

/**
 * @brief ANSI-position fallback for letters and digits (used when the
 *        layout's character is not a plain letter/digit, e.g. Shift+1).
 */
UINT vkFromAnsiKeyCode(unsigned short code) {
    // Index = kVK_ANSI_* code, value = the key's US letter/digit.
    static constexpr char kAnsi[] = {
        'A', 'S', 'D', 'F', 'H', 'G', 'Z', 'X', 'C', 'V', 0,   'B', 'Q', 'W', 'E', 'R',   // 0x00-0x0F
        'Y', 'T', '1', '2', '3', '4', '6', '5', 0,   '9', '7', 0,   '8', '0', 0,   'O',   // 0x10-0x1F
        'U', 0,   'I', 'P', 0,   'L', 'J', 0,   'K', 0,   0,   0,   0,   'N', 'M', 0,     // 0x20-0x2F
    };
    if (code < sizeof(kAnsi)) {
        return static_cast<UINT>(static_cast<unsigned char>(kAnsi[code]));
    }
    return 0;
}

/**
 * @brief Printable character -> Win32 virtual key (letters upper-cased).
 */
UINT vkFromCharacter(unichar ch) {
    if (ch >= 'a' && ch <= 'z') { return static_cast<UINT>(ch - 'a' + 'A'); }
    if (ch >= 'A' && ch <= 'Z') { return static_cast<UINT>(ch); }
    if (ch >= '0' && ch <= '9') { return static_cast<UINT>(ch); }
    switch (ch) {
    case ',': return VK_OEM_COMMA;
    case '.': return kVkOemPeriod;
    case '-': return kVkOemMinus;
    case '=': return kVkOemPlus;
    case ';': return kVkOem1;
    case '/': return kVkOem2;
    case '`': return kVkOem3;
    case '[': return kVkOem4;
    case '\\': return kVkOem5;
    case ']': return kVkOem6;
    case '\'': return kVkOem7;
    default: return 0;
    }
}

/**
 * @brief Command / Shift / Option from an event's modifier flags.
 */
Modifiers modifiersFrom(NSEventModifierFlags flags) {
    Modifiers m;
    m.ctrl = (flags & NSEventModifierFlagCommand) != 0;
    m.shift = (flags & NSEventModifierFlagShift) != 0;
    m.alt = (flags & NSEventModifierFlagOption) != 0;
    return m;
}

/**
 * @brief Translates a key event to the toolkit's virtual key + modifiers,
 *        applying the Mac text-navigation idioms.
 * @return false when the key has no virtual-key meaning (text only)
 */
bool translateKey(NSEvent* event, UINT& vk, Modifiers& mods) {
    if (event == nil) {
        return false;
    }
    mods = modifiersFrom(event.modifierFlags);
    vk = 0;

    // Keys that are the same on every layout.
    switch (event.keyCode) {
    case kKeyReturn:
    case kKeyKeypadEnter: vk = VK_RETURN; break;
    case kKeyTab: vk = VK_TAB; break;
    case kKeySpace: vk = VK_SPACE; break;
    case kKeyBackspace: vk = VK_BACK; break;
    case kKeyForwardDelete: vk = VK_DELETE; break;
    case kKeyEscape: vk = VK_ESCAPE; break;
    case kKeyLeft: vk = VK_LEFT; break;
    case kKeyRight: vk = VK_RIGHT; break;
    case kKeyUp: vk = VK_UP; break;
    case kKeyDown: vk = VK_DOWN; break;
    case kKeyHome: vk = VK_HOME; break;
    case kKeyEnd: vk = VK_END; break;
    case kKeyPageUp: vk = VK_PRIOR; break;
    case kKeyPageDown: vk = VK_NEXT; break;
    case kKeyF1: vk = VK_F1; break;
    case kKeyF2: vk = VK_F1 + 1; break;
    case kKeyF3: vk = VK_F1 + 2; break;
    case kKeyF4: vk = VK_F1 + 3; break;
    case kKeyF5: vk = VK_F1 + 4; break;
    case kKeyF6: vk = VK_F1 + 5; break;
    case kKeyF7: vk = VK_F1 + 6; break;
    case kKeyF8: vk = VK_F1 + 7; break;
    case kKeyF9: vk = VK_F1 + 8; break;
    case kKeyF10: vk = VK_F1 + 9; break;
    case kKeyF11: vk = VK_F1 + 10; break;
    case kKeyF12: vk = VK_F1 + 11; break;
    default: break;
    }

    // Everything else: the layout's character first (AZERTY's A is 'A'),
    // then the key's ANSI position.
    if (vk == 0) {
        NSString* chars = event.charactersIgnoringModifiers;
        if (chars.length > 0) {
            vk = vkFromCharacter([chars characterAtIndex:0]);
        }
        if (vk == 0) {
            vk = vkFromAnsiKeyCode(event.keyCode);
        }
    }
    if (vk == 0) {
        return false;
    }

    // Mac text idioms, expressed in the Windows vocabulary the widgets know.
    const bool arrowOrErase = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_BACK || vk == VK_DELETE;
    if (arrowOrErase && mods.alt && !mods.ctrl) {
        // Option = word-wise (Ctrl on Windows).
        mods.alt = false;
        mods.ctrl = true;
    } else if ((vk == VK_LEFT || vk == VK_RIGHT) && mods.ctrl && !mods.alt) {
        // Command+Left/Right = start/end of line.
        vk = vk == VK_LEFT ? VK_HOME : VK_END;
        mods.ctrl = false;
    }
    return true;
}

/**
 * @brief The Unicode scalar values of an NSString (surrogates joined,
 *        unpaired halves dropped).
 */
std::vector<char32_t> codePointsOf(NSString* text) {
    std::vector<char32_t> out;
    if (text == nil) {
        return out;
    }
    const NSUInteger n = text.length;
    out.reserve(n);
    for (NSUInteger i = 0; i < n; ++i) {
        const unichar u = [text characterAtIndex:i];
        char32_t cp = u;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            const unichar lo = [text characterAtIndex:i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((static_cast<char32_t>(u) - 0xD800) << 10) + (static_cast<char32_t>(lo) - 0xDC00);
                ++i;
            } else {
                continue;   // unpaired high surrogate: drop
            }
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            continue;       // unpaired surrogate: drop
        }
        out.push_back(cp);
    }
    return out;
}

/// NSString -> wide (NFC, the engine's path form).
std::wstring wideFrom(NSString* s) {
    if (s == nil) {
        return {};
    }
    const char* utf8 = s.UTF8String;
    return utf8 != nullptr ? platform::normalizeNfc(platform::toWide(utf8)) : std::wstring();
}

} // namespace

// ===========================================================================
// Cursors
// ===========================================================================

void* cursorFor(CursorKind kind) {
    NSCursor* cursor = nil;
    switch (kind) {
    case CursorKind::Hand: cursor = [NSCursor pointingHandCursor]; break;
    case CursorKind::IBeam: cursor = [NSCursor IBeamCursor]; break;
    case CursorKind::SizeNS: cursor = [NSCursor resizeUpDownCursor]; break;
    case CursorKind::SizeWE: cursor = [NSCursor resizeLeftRightCursor]; break;
    case CursorKind::SizeNWSE:
    case CursorKind::SizeNESW:
        // Diagonal resize cursors are public from macOS 15 on.
        if (@available(macOS 15.0, *)) {
            const NSCursorFrameResizePosition pos = kind == CursorKind::SizeNWSE ? NSCursorFrameResizePositionTopLeft
                                                                                 : NSCursorFrameResizePositionTopRight;
            cursor = [NSCursor frameResizeCursorFromPosition:pos inDirections:NSCursorFrameResizeDirectionsAll];
        } else {
            cursor = [NSCursor crosshairCursor];
        }
        break;
    case CursorKind::SizeAll: cursor = [NSCursor openHandCursor]; break;
    case CursorKind::No: cursor = [NSCursor operationNotAllowedCursor]; break;
    case CursorKind::Arrow:
    default: cursor = [NSCursor arrowCursor]; break;
    }
    if (cursor == nil) {
        cursor = [NSCursor arrowCursor];
    }
    return (__bridge void*)cursor;
}

// ===========================================================================
// Pasteboard / PNG
// ===========================================================================

std::wstring clipboardText() {
    @autoreleasepool {
        NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        return wideFrom(text);
    }
}

bool setClipboardText(std::wstring_view text) {
    @autoreleasepool {
        const std::string utf8 = platform::toUtf8(text);
        NSString* s = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
        if (s == nil) {
            HH_LOG_WARN(kLog, L"clipboard: text is not valid UTF-8");
            return false;
        }
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        [board clearContents];
        return [board setString:s forType:NSPasteboardTypeString] == YES;
    }
}

bool writePng(const std::wstring& path, const uint8_t* bgra, UINT width, UINT height) {
    if (bgra == nullptr || width == 0 || height == 0 || path.empty()) {
        return false;
    }
    @autoreleasepool {
        const size_t stride = static_cast<size_t>(width) * 4;
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, bgra, stride * height, nullptr);
        CGImageRef image = nullptr;
        if (space != nullptr && provider != nullptr) {
            // BGRA in memory = 32-bit little-endian ARGB, alpha premultiplied.
            image = CGImageCreate(width, height, 8, 32, stride, space,
                                  kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, provider, nullptr,
                                  false, kCGRenderingIntentDefault);
        }
        bool ok = false;
        if (image != nullptr) {
            const std::string utf8 = platform::toUtf8(path);
            CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                                   reinterpret_cast<const UInt8*>(utf8.c_str()),
                                                                   static_cast<CFIndex>(utf8.size()), false);
            if (url != nullptr) {
                CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
                if (dest != nullptr) {
                    CGImageDestinationAddImage(dest, image, nullptr);
                    ok = CGImageDestinationFinalize(dest);
                    CFRelease(dest);
                }
                CFRelease(url);
            }
            CGImageRelease(image);
        }
        if (provider != nullptr) { CGDataProviderRelease(provider); }
        if (space != nullptr) { CGColorSpaceRelease(space); }
        if (!ok) {
            HH_LOG_WARN(kLog, L"PNG write failed: {}", path);
        }
        return ok;
    }
}

// ===========================================================================
// FrameClock
// ===========================================================================

struct FrameClock::Impl {
    CFRunLoopTimerRef timer = nullptr;
    std::function<void()> fire;
    double pendingAt = 0.0;     ///< absolute fire time, or 0 when nothing is pending
    double lastFireAt = 0.0;    ///< absolute time of the previous fire
};

FrameClock::FrameClock(std::function<void()> onFire) : impl_(std::make_unique<Impl>()) {
    impl_->fire = std::move(onFire);
    Impl* impl = impl_.get();

    // One timer for the clock's lifetime; "one-shot" by pushing the next
    // fire date far into the future after every fire.
    impl->timer = CFRunLoopTimerCreateWithHandler(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + kFarFutureSec,
                                                  kFarFutureSec, 0, 0, ^(CFRunLoopTimerRef) {
                                                      impl->pendingAt = 0.0;
                                                      impl->lastFireAt = CFAbsoluteTimeGetCurrent();
                                                      if (impl->fire) {
                                                          impl->fire();
                                                      }
                                                  });
    if (impl->timer == nullptr) {
        HH_LOG_ERROR(kLog, L"CFRunLoopTimerCreate failed; frames will not be paced");
        return;
    }
    // Common modes: keeps running during live resize and menu tracking.
    CFRunLoopAddTimer(CFRunLoopGetMain(), impl->timer, kCFRunLoopCommonModes);
}

FrameClock::~FrameClock() {
    if (impl_ && impl_->timer != nullptr) {
        CFRunLoopTimerInvalidate(impl_->timer);
        CFRelease(impl_->timer);
        impl_->timer = nullptr;
    }
}

void FrameClock::wakeIn(double seconds) {
    if (!impl_ || impl_->timer == nullptr) {
        return;
    }
    const double now = CFAbsoluteTimeGetCurrent();
    const double wanted = now + std::max(0.0, std::isfinite(seconds) ? seconds : 0.0);
    // Never faster than the frame-rate cap.
    const double at = std::max(wanted, impl_->lastFireAt + kMinIntervalSec);
    // Only ever move the pending fire earlier.
    if (impl_->pendingAt == 0.0 || at + 1.0e-4 < impl_->pendingAt) {
        impl_->pendingAt = at;
        CFRunLoopTimerSetNextFireDate(impl_->timer, at);
    }
}

void FrameClock::cancel() {
    if (!impl_ || impl_->timer == nullptr) {
        return;
    }
    impl_->pendingAt = 0.0;
    CFRunLoopTimerSetNextFireDate(impl_->timer, CFAbsoluteTimeGetCurrent() + kFarFutureSec);
}

// ===========================================================================
// RepeatingTimer
// ===========================================================================

RepeatingTimer::~RepeatingTimer() {
    stop();
}

void RepeatingTimer::start(double intervalSec, std::function<void()> fn) {
    stop();
    if (!fn || !(intervalSec > 0.0)) {
        return;
    }
    fn_ = std::make_shared<std::function<void()>>(std::move(fn));
    std::shared_ptr<std::function<void()>> call = fn_;
    CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + intervalSec,
                                                              intervalSec, 0, 0, ^(CFRunLoopTimerRef) {
                                                                  if (call && *call) {
                                                                      (*call)();
                                                                  }
                                                              });
    if (timer == nullptr) {
        HH_LOG_ERROR(kLog, L"CFRunLoopTimerCreate failed (repeating timer)");
        fn_.reset();
        return;
    }
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    timer_ = timer;
}

void RepeatingTimer::stop() {
    if (timer_ != nullptr) {
        auto timer = static_cast<CFRunLoopTimerRef>(timer_);
        CFRunLoopTimerInvalidate(timer);
        CFRelease(timer);
        timer_ = nullptr;
    }
    fn_.reset();
}

} // namespace hh::ui::mac

// ===========================================================================
// HHCanvasView
// ===========================================================================

using hh::ui::CursorKind;
using hh::ui::Modifiers;
using hh::ui::MouseButton;
using hh::ui::Point;
using hh::ui::Rect;
using hh::ui::Size;

@implementation HHCanvasView {
    hh::ui::mac::IViewSink* sink_;           ///< not owned; nil after detachSink
    NSTrackingArea* tracking_;
    NSMutableAttributedString* marked_;      ///< input-method composition (not drawn inline)
    NSRange markedSelection_;
    BOOL rightFromControlClick_;             ///< the current left press is a Control-click
}

- (instancetype)initWithFrame:(NSRect)frame sink:(hh::ui::mac::IViewSink*)sink {
    self = [super initWithFrame:frame];
    if (self != nil) {
        sink_ = sink;
        marked_ = [[NSMutableAttributedString alloc] init];
        markedSelection_ = NSMakeRange(0, 0);
        _imeEnabled = NO;
        _dragsWindowOnCaption = NO;

        // Layer-backed so the frame is composited by Core Animation; the
        // layer is redrawn (not stretched) while the window resizes.
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    return self;
}

- (void)detachSink {
    sink_ = nullptr;
    [self unregisterDraggedTypes];
}

// ---- view configuration -----------------------------------------------------

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)wantsUpdateLayer { return NO; }

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (sink_ != nullptr && sink_->viewAcceptsFileDrops()) {
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    }
}

- (void)updateTrackingAreas {
    if (tracking_ != nil) {
        [self removeTrackingArea:tracking_];
    }
    // ActiveAlways: hover feedback even while another app is frontmost,
    // like the Win32 window (and required for a non-activating panel).
    const NSTrackingAreaOptions options = NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                          NSTrackingCursorUpdate | NSTrackingActiveAlways | NSTrackingInVisibleRect;
    tracking_ = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:options owner:self userInfo:nil];
    [self addTrackingArea:tracking_];
    [super updateTrackingAreas];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (sink_ != nullptr && self.window != nil) {
        sink_->viewBackingChanged(static_cast<float>(self.window.backingScaleFactor));
    }
    [self setNeedsDisplay:YES];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (sink_ != nullptr) {
        sink_->viewResized(Size{static_cast<float>(size.width), static_cast<float>(size.height)});
    }
}

// ---- drawing -------------------------------------------------------------------

- (void)viewWillDraw {
    if (sink_ != nullptr) {
        sink_->viewWillDraw();
    }
    [super viewWillDraw];
}

- (void)drawRect:(NSRect)dirty {
    if (sink_ == nullptr) {
        return;
    }
    NSGraphicsContext* gc = [NSGraphicsContext currentContext];
    CGContextRef ctx = gc != nil ? gc.CGContext : nullptr;
    if (ctx == nullptr) {
        return;
    }
    const NSSize size = self.bounds.size;
    sink_->viewDraw(ctx, Size{static_cast<float>(size.width), static_cast<float>(size.height)});
}

// ---- cursor -------------------------------------------------------------------------

- (void)refreshCursor {
    if (sink_ == nullptr || self.window == nil) {
        return;
    }
    // Only while the pointer is over us: never fight another view's cursor.
    const NSPoint inWindow = [self.window mouseLocationOutsideOfEventStream];
    const NSPoint local = [self convertPoint:inWindow fromView:nil];
    if (NSPointInRect(local, self.bounds)) {
        [(__bridge NSCursor*)hh::ui::mac::cursorFor(sink_->viewCursor()) set];
    }
}

- (void)cursorUpdate:(NSEvent*)event {
    if (sink_ == nullptr) {
        [super cursorUpdate:event];
        return;
    }
    [(__bridge NSCursor*)hh::ui::mac::cursorFor(sink_->viewCursor()) set];
}

// ---- pointer --------------------------------------------------------------------------

/// Event location in view (= root dip) coordinates.
- (Point)pointFor:(NSEvent*)event {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    return Point{static_cast<float>(p.x), static_cast<float>(p.y)};
}

/// WM_xBUTTONDOWN (1) vs WM_xBUTTONDBLCLK (2): every second click is a double.
- (int)clickCountFor:(NSEvent*)event {
    const NSInteger n = event.clickCount;
    return (n >= 2 && (n % 2) == 0) ? 2 : 1;
}

- (void)pressWith:(NSEvent*)event button:(MouseButton)button {
    if (sink_ == nullptr) {
        return;
    }
    const bool handled = sink_->viewMouseDown([self pointFor:event], button, hh::ui::mac::modifiersFrom(event.modifierFlags),
                                              [self clickCountFor:event]);
    // Caption area: hand the drag to the window server (smooth, snaps to
    // Stage Manager / tiling like any native title bar).
    if (!handled && _dragsWindowOnCaption && button == MouseButton::Left && self.window != nil) {
        [self.window performWindowDragWithEvent:event];
    }
    [self refreshCursor];
}

- (void)mouseDown:(NSEvent*)event {
    if (self.window != nil && self.window.firstResponder != self && self.window.canBecomeKeyWindow) {
        [self.window makeFirstResponder:self];
    }
    // Control-click is the Mac right click.
    rightFromControlClick_ = (event.modifierFlags & NSEventModifierFlagControl) != 0;
    [self pressWith:event button:rightFromControlClick_ ? MouseButton::Right : MouseButton::Left];
}

- (void)rightMouseDown:(NSEvent*)event {
    [self pressWith:event button:MouseButton::Right];
}

- (void)otherMouseDown:(NSEvent*)event {
    if (event.buttonNumber == 2) {
        [self pressWith:event button:MouseButton::Middle];
    }
}

- (void)mouseUp:(NSEvent*)event {
    if (sink_ == nullptr) {
        return;
    }
    const MouseButton button = rightFromControlClick_ ? MouseButton::Right : MouseButton::Left;
    rightFromControlClick_ = NO;
    sink_->viewMouseUp([self pointFor:event], button, hh::ui::mac::modifiersFrom(event.modifierFlags));
    [self refreshCursor];
}

- (void)rightMouseUp:(NSEvent*)event {
    if (sink_ != nullptr) {
        sink_->viewMouseUp([self pointFor:event], MouseButton::Right, hh::ui::mac::modifiersFrom(event.modifierFlags));
    }
}

- (void)otherMouseUp:(NSEvent*)event {
    if (sink_ != nullptr && event.buttonNumber == 2) {
        sink_->viewMouseUp([self pointFor:event], MouseButton::Middle, hh::ui::mac::modifiersFrom(event.modifierFlags));
    }
}

- (void)movedWith:(NSEvent*)event {
    if (sink_ == nullptr) {
        return;
    }
    sink_->viewMouseMove([self pointFor:event], hh::ui::mac::modifiersFrom(event.modifierFlags));
    [self refreshCursor];
}

- (void)mouseMoved:(NSEvent*)event { [self movedWith:event]; }
- (void)mouseDragged:(NSEvent*)event { [self movedWith:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self movedWith:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self movedWith:event]; }
- (void)mouseEntered:(NSEvent*)event { [self movedWith:event]; }

- (void)mouseExited:(NSEvent*)event {
    // A drag that leaves the view keeps its capture (AppKit keeps sending
    // mouseDragged here), so only a hover exit counts.
    if (sink_ != nullptr && [NSEvent pressedMouseButtons] == 0) {
        sink_->viewMouseExited();
    }
}

- (void)scrollWheel:(NSEvent*)event {
    if (sink_ == nullptr) {
        return;
    }
    // The toolkit's ScrollView runs its own inertia from the gesture.
    if (event.momentumPhase != NSEventPhaseNone) {
        return;
    }
    const bool precise = event.hasPreciseScrollingDeltas;
    const float k = precise ? hh::ui::mac::kPreciseUnitsPerPoint : static_cast<float>(WHEEL_DELTA);
    // AppKit: positive Y = content moves down (= wheel up, WM_MOUSEWHEEL > 0);
    // positive X = content moves right (= scroll left, WM_MOUSEHWHEEL < 0).
    const float dy = static_cast<float>(event.scrollingDeltaY) * k;
    const float dx = -static_cast<float>(event.scrollingDeltaX) * k;
    if (dy == 0.0f && dx == 0.0f) {
        return;
    }
    sink_->viewWheel([self pointFor:event], dy, dx, precise, hh::ui::mac::modifiersFrom(event.modifierFlags));
}

// ---- keyboard --------------------------------------------------------------------------

- (void)keyDown:(NSEvent*)event {
    if (sink_ == nullptr) {
        [super keyDown:event];
        return;
    }
    // An input method in the middle of a composition owns every key.
    if (self.hasMarkedText) {
        [self interpretKeyEvents:@[ event ]];
        return;
    }

    UINT vk = 0;
    Modifiers mods;
    const bool haveVk = hh::ui::mac::translateKey(event, vk, mods);
    if (haveVk) {
        const bool handled = sink_->viewKeyDown(vk, mods, event.isARepeat);
        // Command+Backspace is the Mac Delete command for the selection.
        const bool command = (event.modifierFlags & NSEventModifierFlagCommand) != 0;
        if (!handled && vk == VK_BACK && command && sink_ != nullptr) {
            sink_->viewKeyDown(VK_DELETE, Modifiers{}, event.isARepeat);
        }
    }
    if (sink_ == nullptr) {
        return;   // the key closed the window
    }

    // Text (WM_CHAR parity): anything that is not a Command/Control chord.
    const NSEventModifierFlags flags = event.modifierFlags;
    if ((flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0) {
        return;
    }
    if (_imeEnabled) {
        // A text field has focus: layout, dead keys and input methods.
        [self interpretKeyEvents:@[ event ]];
    } else {
        // Nothing to compose into: the key's own characters (menu type-ahead).
        [self emitText:event.characters];
    }
}

/// Delivers committed text code point by code point (control characters are keys).
- (void)emitText:(NSString*)text {
    for (const char32_t cp : hh::ui::mac::codePointsOf(text)) {
        if (sink_ == nullptr) {
            return;   // a character closed the window
        }
        if (cp >= 0x20 && cp != 0x7F && !(cp >= 0xF700 && cp <= 0xF8FF)) {
            sink_->viewChar(cp);
        }
    }
}

- (void)keyUp:(NSEvent*)event {
    if (sink_ == nullptr) {
        [super keyUp:event];
        return;
    }
    UINT vk = 0;
    Modifiers mods;
    if (hh::ui::mac::translateKey(event, vk, mods)) {
        sink_->viewKeyUp(vk, mods);
    }
}

/// Replays an Edit-menu action as the Ctrl chord the widgets understand.
- (void)replayShortcut:(UINT)vk shift:(bool)shift {
    if (sink_ == nullptr) {
        return;
    }
    Modifiers mods;
    mods.ctrl = true;
    mods.shift = shift;
    sink_->viewKeyDown(vk, mods, false);
}

- (void)copy:(id)sender { [self replayShortcut:'C' shift:false]; }
- (void)cut:(id)sender { [self replayShortcut:'X' shift:false]; }
- (void)paste:(id)sender { [self replayShortcut:'V' shift:false]; }
- (void)selectAll:(id)sender { [self replayShortcut:'A' shift:false]; }
- (void)undo:(id)sender { [self replayShortcut:'Z' shift:false]; }
- (void)redo:(id)sender { [self replayShortcut:'Z' shift:true]; }

- (NSTextInputContext*)inputContext {
    // No composition unless a text field has focus (ImmAssociateContext parity).
    return _imeEnabled ? [super inputContext] : nil;
}

- (void)setImeEnabled:(BOOL)enabled {
    if (_imeEnabled == enabled) {
        return;
    }
    if (!enabled && marked_.length > 0) {
        [[super inputContext] discardMarkedText];
        [marked_ setAttributedString:[[NSAttributedString alloc] initWithString:@""]];
    }
    _imeEnabled = enabled;
}

// ---- NSTextInputClient -----------------------------------------------------------------

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    NSString* text = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)string string] : (NSString*)string;
    [marked_ setAttributedString:[[NSAttributedString alloc] initWithString:@""]];
    markedSelection_ = NSMakeRange(0, 0);
    if (sink_ == nullptr || ![text isKindOfClass:[NSString class]]) {
        return;
    }
    [self emitText:text];
}

- (void)doCommandBySelector:(SEL)selector {
    // Keys were already delivered as virtual keys; swallowing the command
    // here keeps AppKit from beeping for the ones no widget wanted.
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    if ([string isKindOfClass:[NSAttributedString class]]) {
        [marked_ setAttributedString:(NSAttributedString*)string];
    } else if ([string isKindOfClass:[NSString class]]) {
        [marked_ setAttributedString:[[NSAttributedString alloc] initWithString:(NSString*)string]];
    }
    markedSelection_ = selectedRange;
}

- (void)unmarkText {
    [marked_ setAttributedString:[[NSAttributedString alloc] initWithString:@""]];
    markedSelection_ = NSMakeRange(0, 0);
}

- (NSRange)selectedRange {
    return marked_.length > 0 ? markedSelection_ : NSMakeRange(0, 0);
}

- (NSRange)markedRange {
    return marked_.length > 0 ? NSMakeRange(0, marked_.length) : NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText {
    return marked_.length > 0;
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    return nil;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (sink_ == nullptr || self.window == nil) {
        return NSZeroRect;
    }
    // The candidate window sits under the text field's caret.
    const Rect caret = sink_->viewImeCaret();
    const NSRect local = NSMakeRect(caret.x, caret.y, std::max(1.0f, caret.w), std::max(1.0f, caret.h));
    const NSRect inWindow = [self convertRect:local toView:nil];
    return [self.window convertRectToScreen:inWindow];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    return NSNotFound;
}

// ---- drag and drop ------------------------------------------------------------------------

/// File URLs on a dragging pasteboard (empty when there are none).
- (NSArray<NSURL*>*)fileURLsFrom:(id<NSDraggingInfo>)info {
    NSPasteboard* board = info.draggingPasteboard;
    if (board == nil) {
        return @[];
    }
    NSArray* urls = [board readObjectsForClasses:@[ [NSURL class] ]
                                         options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    return urls ?: @[];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    if (sink_ == nullptr || !sink_->viewAcceptsFileDrops()) {
        return NSDragOperationNone;
    }
    return [self fileURLsFrom:info].count > 0 ? NSDragOperationCopy : NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    return [self draggingEntered:info];
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    if (sink_ == nullptr || !sink_->viewAcceptsFileDrops()) {
        return NO;
    }
    std::vector<std::wstring> paths;
    for (NSURL* url in [self fileURLsFrom:info]) {
        if (url.isFileURL && url.path.length > 0) {
            paths.push_back(hh::ui::mac::wideFrom(url.path));
        }
    }
    if (paths.empty()) {
        return NO;
    }
    sink_->viewFilesDropped(paths);
    return YES;
}

@end
