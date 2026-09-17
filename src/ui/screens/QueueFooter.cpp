// ---------------------------------------------------------------------------
// QueueFooter.cpp - Auto-process / dock toggles and the queue summary.
// ---------------------------------------------------------------------------
#include "ui/screens/QueueFooter.h"

#include "core/Logger.h"
#include "ui/controls/Label.h"
#include "ui/controls/ToggleSwitch.h"

#include <memory>
#include <utility>

namespace hh::ui {

namespace {

/// Horizontal inset of the strip (matches the card gutter of the list above).
constexpr float kSidePadding = 12.0f;
/// Gap between neighbouring items.
constexpr float kSpacing = 8.0f;
/// Below this width the dock label is dropped (the switch keeps its tooltip).
constexpr float kNarrowWidth = 400.0f;

} // namespace

/**
 * @brief Builds the strip and performs the first refresh.
 */
QueueFooter::QueueFooter(IQueueViewModel& vm) : vm_(vm) {
    // The footer is a single horizontal row, vertically centred, fixed height.
    stack().axis = Axis::Horizontal;
    stack().spacing = kSpacing;
    stack().padding = Insets::symmetric(kSidePadding, 0.0f);
    stack().crossAlign = CrossAlign::Center;
    layoutParams().height = kHeight;

    // "Auto-process" label + switch. The switch writes straight to the model;
    // the model's onChanged then flows back through refresh().
    autoLabel_ = add(std::make_unique<Label>(L"Auto-process", typography::callout()));
    autoSwitch_ = add(std::make_unique<ToggleSwitch>(vm_.autoProcess(), [this](bool on) {
        if (updating_) { return; }
        vm_.setAutoProcess(on);
    }));
    if (autoSwitch_) {
        autoSwitch_->setTooltipText(L"Process finished exports automatically (Ctrl+Shift+A)");
    }

    // Optional dock toggle; hidden until AppShell configures it.
    dockLabel_ = add(std::make_unique<Label>(L"Dock inside AME", typography::callout()));
    if (dockLabel_) {
        dockLabel_->layoutParams().margin.left = kSpacing;
        dockLabel_->setVisible(false);
    }
    dockSwitch_ = add(std::make_unique<ToggleSwitch>(false, [this](bool on) {
        if (updating_) { return; }
        if (onDockToggle_) { onDockToggle_(on); }
    }));
    if (dockSwitch_) {
        dockSwitch_->setTooltipText(L"Dock inside Media Encoder");
        dockSwitch_->setVisible(false);
    }

    // Push the summary to the right edge.
    add(std::make_unique<Spacer>());
    summary_ = add(std::make_unique<Label>(L"", typography::caption(), LabelTone::Secondary));
    if (summary_) {
        summary_->setAlign(HAlign::Right);
        summary_->setTrimming(Trimming::End);
    }

    refresh();
}

/**
 * @brief Nothing to unhook: the view model callback belongs to the screen.
 */
QueueFooter::~QueueFooter() = default;

/**
 * @brief Re-reads the auto-process flag and the summary line.
 */
void QueueFooter::refresh() {
    // Guard so pushing state into the switch does not echo back into the model.
    updating_ = true;
    if (autoSwitch_) {
        const bool on = vm_.autoProcess();
        if (autoSwitch_->isOn() != on) { autoSwitch_->setOn(on, true); }
    }
    if (summary_) {
        std::wstring text = vm_.footerSummary();
        if (summary_->text() != text) { summary_->setText(std::move(text)); }
    }
    updating_ = false;
}

/**
 * @brief Shows/hides the dock switch and applies its state silently.
 */
void QueueFooter::setDockToggle(std::function<void(bool)> onToggle, bool visible, bool state) {
    onDockToggle_ = std::move(onToggle);
    dockVisible_ = visible;

    updating_ = true;
    if (dockSwitch_) {
        if (dockSwitch_->visible() != visible) { dockSwitch_->setVisible(visible); }
        if (dockSwitch_->isOn() != state) { dockSwitch_->setOn(state, attached()); }
    }
    // The label follows the switch unless the strip is too narrow; measure()
    // re-evaluates the narrow case on the next layout pass.
    if (dockLabel_ && dockLabel_->visible() != visible) { dockLabel_->setVisible(visible); }
    updating_ = false;
    invalidateLayout();
}

/**
 * @brief Drops the dock label on narrow strips, then measures as a stack.
 */
Size QueueFooter::measure(const Constraints& c) {
    // With a bounded width we can decide whether the dock label still fits.
    if (dockLabel_ && c.hasBoundedWidth()) {
        const bool wantLabel = dockVisible_ && c.maxW >= kNarrowWidth;
        if (dockLabel_->visible() != wantLabel) { dockLabel_->setVisible(wantLabel); }
    }
    Size s = Widget::measure(c);
    // The strip always keeps its nominal height so the list above stays put.
    s.h = std::max(0.0f, std::min(kHeight, c.maxH));
    if (s.h < c.minH) { s.h = c.minH; }
    return s;
}

} // namespace hh::ui
