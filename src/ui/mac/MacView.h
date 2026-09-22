// ---------------------------------------------------------------------------
// MacView.h - the Cocoa view every toolkit window draws into.
//
// HHCanvasView is a flipped, layer-backed NSView: one point is one dip, y
// grows downwards, so root coordinates pass through untouched. It turns
// AppKit events into the toolkit's vocabulary (Win32 virtual keys, WHEEL_DELTA
// units, Command -> Modifiers::ctrl) and hands them to an IViewSink, which is
// the C++ window host (main window or popup panel).
//
// FrameClock is the run-loop side of the frame pacing: a single CFRunLoop
// timer, registered for the common modes so animations keep running during
// live resize and menu tracking, re-armed for "as soon as possible", "next
// vsync-ish tick" or "when the next timeline timer is due".
//
// Only Objective-C++ translation units see the @interface; the C++ parts
// are plain declarations.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"
#include "ui/core/InputEvents.h"
#include "ui/gfx/Geometry.h"

#include <CoreGraphics/CoreGraphics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hh::ui::mac {

/**
 * @brief What a canvas view reports to the window host that owns it.
 *
 * Every point is in view coordinates, which are root dips (the view is
 * flipped and unscaled). All calls arrive on the main thread.
 */
class IViewSink {
public:
    virtual ~IViewSink() = default;

    /// Paints one frame into @p ctx (already flipped; one unit = one dip).
    virtual void viewDraw(CGContextRef ctx, Size sizeDips) = 0;

    // ---- pointer -----------------------------------------------------------
    virtual void viewMouseMove(Point pt, Modifiers mods) = 0;
    /**
     * @brief Button press.
     * @return false to let the view start a window drag (caption hit)
     */
    virtual bool viewMouseDown(Point pt, MouseButton button, Modifiers mods, int clickCount) = 0;
    virtual void viewMouseUp(Point pt, MouseButton button, Modifiers mods) = 0;
    virtual void viewMouseExited() = 0;
    /// Wheel in WHEEL_DELTA units (120 per notch); precise = touchpad / Magic Mouse.
    virtual void viewWheel(Point pt, float delta, float deltaX, bool precise, Modifiers mods) = 0;

    // ---- keyboard ----------------------------------------------------------
    /// Key press already translated to a Win32 virtual key. Returns true when handled.
    virtual bool viewKeyDown(UINT vk, Modifiers mods, bool repeat) = 0;
    virtual bool viewKeyUp(UINT vk, Modifiers mods) = 0;
    /// Committed text, one code point at a time (control characters never arrive).
    virtual void viewChar(char32_t ch) = 0;
    /// Caret rectangle (root dips) the input method places its candidate window at.
    [[nodiscard]] virtual Rect viewImeCaret() const = 0;

    // ---- misc ----------------------------------------------------------------
    /// Cursor to show while the pointer is over the view.
    [[nodiscard]] virtual CursorKind viewCursor() const = 0;
    /// Files dropped from Finder (absolute paths). Default: ignore.
    virtual void viewFilesDropped(const std::vector<std::wstring>& paths) { static_cast<void>(paths); }
    /// True when the view accepts file drops at all.
    [[nodiscard]] virtual bool viewAcceptsFileDrops() const { return false; }
    /// Backing scale factor changed (moved to another display).
    virtual void viewBackingChanged(float backingScale) { static_cast<void>(backingScale); }
    /// The view's size changed (live resize included).
    virtual void viewResized(Size sizeDips) { static_cast<void>(sizeDips); }
    /// About to draw, after AppKit's layout pass (last chance to adjust sibling views).
    virtual void viewWillDraw() {}
};

/// The NSCursor for a toolkit cursor kind, as an opaque NSCursor* (never null).
void* cursorFor(CursorKind kind);

// ---- shared Cocoa helpers for the window hosts ---------------------------------

/// Plain text on the general pasteboard (empty when there is none).
std::wstring clipboardText();
/// Replaces the general pasteboard's contents with plain text.
bool setClipboardText(std::wstring_view text);
/**
 * @brief Writes premultiplied BGRA pixels (top row first, tightly packed)
 *        to a PNG file through ImageIO.
 */
bool writePng(const std::wstring& path, const uint8_t* bgra, UINT width, UINT height);

/**
 * @brief Coalescing one-shot wake-ups on the main run loop (common modes).
 *
 * wakeIn() only ever moves the pending fire date earlier, so any number of
 * requestFrame() calls inside one event collapse into a single frame. Fires
 * are spaced at least kMinIntervalSec apart so a widget that re-requests a
 * frame from its own paint can never spin the CPU.
 */
class FrameClock {
public:
    /// Upper bound on the frame rate (120 Hz: ProMotion's refresh).
    static constexpr double kMinIntervalSec = 1.0 / 120.0;
    /// The animation cadence while something moves.
    static constexpr double kFrameIntervalSec = 1.0 / 60.0;

    explicit FrameClock(std::function<void()> onFire);
    ~FrameClock();
    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;

    /// Fires at most @p seconds from now (earlier pending wake-ups win).
    void wakeIn(double seconds);
    /// Forgets the pending wake-up.
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief A repeating main-thread timer (common modes), for the 1 Hz tick.
 */
class RepeatingTimer {
public:
    RepeatingTimer() = default;
    ~RepeatingTimer();
    RepeatingTimer(const RepeatingTimer&) = delete;
    RepeatingTimer& operator=(const RepeatingTimer&) = delete;

    /// (Re)starts the timer; @p fn runs every @p intervalSec seconds.
    void start(double intervalSec, std::function<void()> fn);
    void stop();
    [[nodiscard]] bool running() const noexcept { return timer_ != nullptr; }

private:
    void* timer_ = nullptr;   ///< CFRunLoopTimerRef (retained)
    std::shared_ptr<std::function<void()>> fn_;
};

} // namespace hh::ui::mac

#ifdef __OBJC__
#import <AppKit/AppKit.h>

/**
 * @brief Flipped, layer-backed view that draws through an IViewSink.
 */
@interface HHCanvasView : NSView <NSTextInputClient, NSDraggingDestination>

/// @param sink  the host; not retained (the host detaches before it dies)
- (instancetype)initWithFrame:(NSRect)frame sink:(hh::ui::mac::IViewSink*)sink;

/// Stops every callback into the sink (called from the host's destructor).
- (void)detachSink;

/// Whether the input method may compose into this view (a text field has focus).
@property (nonatomic) BOOL imeEnabled;

/// Mouse-down in the caption area starts a native window drag (main window only).
@property (nonatomic) BOOL dragsWindowOnCaption;

/// Re-applies the cursor for the pointer's current position.
- (void)refreshCursor;

@end
#endif
