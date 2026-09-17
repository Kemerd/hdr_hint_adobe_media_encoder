// ---------------------------------------------------------------------------
// JobCard.cpp - one queue row bound to a JobView.
// ---------------------------------------------------------------------------
#include "ui/screens/JobCard.h"

#include "core/Logger.h"
#include "ui/controls/Button.h"
#include "ui/controls/Label.h"
#include "ui/controls/PopupButton.h"
#include "ui/controls/PopupMenu.h"
#include "ui/controls/ProgressBar.h"
#include "ui/controls/ToggleSwitch.h"
#include "ui/core/OverlayHost.h"
#include "ui/core/RootView.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <utility>
#include <vector>

namespace hh::ui {

namespace {

/// Inner padding of the card.
constexpr float kCardPadding = 12.0f;
/// Vertical gap between the rows.
constexpr float kRowSpacing = 8.0f;
/// Gap between items inside a row.
constexpr float kItemSpacing = 8.0f;
/// Below this card width the preset/LUT pop-ups stack vertically.
constexpr float kPlanWrapWidth = 380.0f;
/// Width reserved for the "100%" caption right of the progress bar.
constexpr float kPercentWidth = 40.0f;

/// Chip label for a transfer kind.
const wchar_t* transferText(TransferKind t) noexcept {
    switch (t) {
    case TransferKind::PQ: return L"PQ";
    case TransferKind::HLG: return L"HLG";
    case TransferKind::SDR: return L"SDR";
    case TransferKind::Unknown: break;
    }
    return L"";
}

/// Converts view-model choices into pop-up rows.
std::vector<PopupItem> toItems(const std::vector<Choice>& choices) {
    std::vector<PopupItem> items;
    items.reserve(choices.size());
    for (const Choice& ch : choices) {
        PopupItem it;
        it.label = ch.label;
        it.subtitle = ch.subtitle;
        it.value = ch.value;
        items.push_back(std::move(it));
    }
    return items;
}

/// True when the pop-up already shows exactly these choices (label/value/subtitle).
bool sameItems(const std::vector<PopupItem>& items, const std::vector<Choice>& choices) {
    if (items.size() != choices.size()) { return false; }
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].label != choices[i].label || items[i].value != choices[i].value ||
            items[i].subtitle != choices[i].subtitle) {
            return false;
        }
    }
    return true;
}

/// Sets a label text only when it changed (avoids needless re-layout).
void setLabelText(Label* label, const std::wstring& text) {
    if (!label) { return; }
    if (label->text() != text) { label->setText(text); }
}

/// Sets a button text only when it changed.
void setButtonText(Button* button, const std::wstring& text) {
    if (!button) { return; }
    if (button->text() != text) { button->setText(text); }
}

/// Shows/hides a widget only when the state changes.
void setShown(Widget* w, bool shown) {
    if (!w) { return; }
    if (w->visible() != shown) { w->setVisible(shown); }
}

/// Enables/disables a widget only when the state changes.
void setEnabledIf(Widget* w, bool enabled) {
    if (!w) { return; }
    if (w->enabled() != enabled) { w->setEnabled(enabled); }
}

} // namespace

// ---------------------------------------------------------------------------
// Tone helpers
// ---------------------------------------------------------------------------

/**
 * @brief Chip tone per lifecycle state (accent for work in progress).
 */
ChipTone JobCard::toneForState(JobState state) noexcept {
    switch (state) {
    case JobState::Encoding:
    case JobState::Muxing:
    case JobState::Verifying: return ChipTone::Accent;
    case JobState::Ready: return ChipTone::Info;
    case JobState::Held: return ChipTone::Warning;
    case JobState::Done: return ChipTone::Success;
    case JobState::Failed: return ChipTone::Destructive;
    case JobState::Discovered:
    case JobState::SkippedSdr:
    case JobState::Cancelled: break;
    }
    return ChipTone::Neutral;
}

/**
 * @brief States that are actively doing something get a spinner.
 */
bool JobCard::stateSpins(JobState state) noexcept {
    switch (state) {
    case JobState::Discovered:
    case JobState::Encoding:
    case JobState::Muxing:
    case JobState::Verifying: return true;
    default: break;
    }
    return false;
}

/**
 * @brief Domain tone for the transfer badge.
 */
ChipTone JobCard::toneForTransfer(TransferKind transfer) noexcept {
    switch (transfer) {
    case TransferKind::PQ: return ChipTone::PQ;
    case TransferKind::HLG: return ChipTone::HLG;
    case TransferKind::SDR: return ChipTone::SDR;
    case TransferKind::Unknown: break;
    }
    return ChipTone::Neutral;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/**
 * @brief Builds the five rows and applies the initial view.
 */
JobCard::JobCard(IQueueViewModel& vm, const JobView& view) : vm_(vm), id_(view.id) {
    // The card body is a vertical stack of rows with the standard 12 dip inset.
    stack().axis = Axis::Vertical;
    stack().spacing = kRowSpacing;
    stack().padding = Insets::all(kCardPadding);
    stack().crossAlign = CrossAlign::Stretch;

    build();

    // Right-click anywhere on the card (not on a control) opens the menu.
    onContextMenu = [this](Point rootPt) { showContextMenu(rootPt); };

    update(view);
}

/**
 * @brief Children are torn down by the Widget base.
 */
JobCard::~JobCard() = default;

/**
 * @brief Creates every control once; update() only pushes state afterwards.
 */
void JobCard::build() {
    // ---- row 1: name + transfer chip + state chip ---------------------------
    Widget* row1 = add(std::make_unique<Widget>());
    if (row1) {
        row1->stack().axis = Axis::Horizontal;
        row1->stack().spacing = kItemSpacing;
        row1->stack().crossAlign = CrossAlign::Center;

        name_ = row1->add(std::make_unique<Label>(L"", typography::bodyStrong()));
        if (name_) {
            name_->setTrimming(Trimming::Middle);
            name_->layoutParams().flexGrow = 1.0f;
        }
        transferChip_ = row1->add(std::make_unique<Chip>(L"", ChipTone::Neutral));
        if (transferChip_) { transferChip_->setVisible(false); }
        stateChip_ = row1->add(std::make_unique<Chip>(L"", ChipTone::Neutral));
    }

    // ---- row 2: subtitle + optional reason ----------------------------------
    subtitle_ = add(std::make_unique<Label>(L"", typography::caption(), LabelTone::Secondary));
    if (subtitle_) { subtitle_->setTrimming(Trimming::End); }
    reason_ = add(std::make_unique<Label>(L"", typography::caption(), LabelTone::Destructive));
    if (reason_) {
        reason_->setTrimming(Trimming::End);
        reason_->setVisible(false);
    }

    // ---- row 3: progress bar + percent --------------------------------------
    progressRow_ = add(std::make_unique<Widget>());
    if (progressRow_) {
        progressRow_->stack().axis = Axis::Horizontal;
        progressRow_->stack().spacing = kItemSpacing;
        progressRow_->stack().crossAlign = CrossAlign::Center;
        progressRow_->setVisible(false);

        progress_ = progressRow_->add(std::make_unique<ProgressBar>());
        if (progress_) { progress_->layoutParams().flexGrow = 1.0f; }
        percent_ = progressRow_->add(std::make_unique<Label>(L"", typography::caption(), LabelTone::Secondary));
        if (percent_) {
            percent_->setAlign(HAlign::Right);
            percent_->layoutParams().width = kPercentWidth;
        }
    }

    // ---- row 4: preset + LUT pop-ups (wrap on narrow cards) -----------------
    planRow_ = add(std::make_unique<Widget>());
    if (planRow_) {
        planRow_->stack().axis = Axis::Horizontal;
        planRow_->stack().spacing = kItemSpacing;
        planRow_->stack().wrapIfNarrowerThan = kPlanWrapWidth;
        // Stretch is harmless horizontally (28 dip rows) and fills the width
        // once the row wraps to a vertical stack.
        planRow_->stack().crossAlign = CrossAlign::Stretch;

        preset_ = planRow_->add(std::make_unique<PopupButton>());
        if (preset_) {
            preset_->setCaption(L"Preset");
            preset_->setPlaceholder(L"Choose preset");
            preset_->layoutParams().flexGrow = 1.0f;
            preset_->onChanged = [this](int) {
                if (updating_ || !preset_) { return; }
                vm_.setPreset(id_, preset_->selectedValue());
            };
        }
        lut_ = planRow_->add(std::make_unique<PopupButton>());
        if (lut_) {
            lut_->setCaption(L"LUT");
            lut_->setPlaceholder(L"None");
            lut_->layoutParams().flexGrow = 1.0f;
            lut_->onChanged = [this](int) {
                if (updating_ || !lut_) { return; }
                vm_.setLut(id_, lut_->selectedValue());
            };
        }
    }

    // ---- row 5: Attach LUT toggle, spacer, action buttons -------------------
    actionRow_ = add(std::make_unique<Widget>());
    if (actionRow_) {
        actionRow_->stack().axis = Axis::Horizontal;
        actionRow_->stack().spacing = kItemSpacing;
        actionRow_->stack().crossAlign = CrossAlign::Center;

        attachLabel_ = actionRow_->add(std::make_unique<Label>(L"Attach LUT", typography::callout()));
        attach_ = actionRow_->add(std::make_unique<ToggleSwitch>(false, [this](bool on) {
            if (updating_) { return; }
            vm_.setAttachLut(id_, on);
        }));
        if (attach_) { attach_->setTooltipText(L"Embed the LUT in the hint file"); }

        actionRow_->add(std::make_unique<Spacer>());

        // Reveal shows the hint file when it exists, otherwise the AME output.
        reveal_ = actionRow_->add(std::make_unique<Button>(L"Reveal", ButtonKind::Secondary, [this] {
            vm_.reveal(id_, view_.canRevealHint);
        }));
        remove_ = actionRow_->add(std::make_unique<Button>(L"Remove", ButtonKind::Destructive, [this] {
            vm_.remove(id_);
        }));
        // One button toggles between Hold and Resume depending on the job.
        hold_ = actionRow_->add(std::make_unique<Button>(L"Hold", ButtonKind::Secondary, [this] {
            if (view_.held) { vm_.resume(id_); } else { vm_.hold(id_); }
        }));
        run_ = actionRow_->add(std::make_unique<Button>(L"Run", ButtonKind::Primary, [this] {
            vm_.run(id_);
        }));
    }
}

// ---------------------------------------------------------------------------
// Updates
// ---------------------------------------------------------------------------

/**
 * @brief Pushes a new snapshot into the controls without echoing callbacks.
 */
void JobCard::update(const JobView& view) {
    if (view.id != id_ && initialized_) {
        // A card never changes identity; log and keep the original id.
        HH_LOG_WARN(L"JobCard", L"update() with foreign id {} on card {}", view.id, id_);
    }
    const bool force = !initialized_;
    updating_ = true;

    applyHeader(view, force);
    applyProgress(view, force);
    applyPlan(view, force);
    applyActions(view, force);

    // Failed jobs get the red outline; everything else the normal hairline.
    const bool failed = view.state == JobState::Failed;
    if (force || failed != (view_.state == JobState::Failed)) { setDestructiveOutline(failed); }

    view_ = view;
    initialized_ = true;
    updating_ = false;
}

/**
 * @brief Name, chips, subtitle and the Held/Failed reason line.
 */
void JobCard::applyHeader(const JobView& view, bool force) {
    // Name with the full output path as tooltip (the label trims in the middle).
    setLabelText(name_, view.name);
    if (name_ && (force || view.outputPath != view_.outputPath)) { name_->setTooltipText(view.outputPath); }

    // Transfer badge: hidden while the transfer is unknown.
    if (transferChip_) {
        const bool known = view.transfer != TransferKind::Unknown;
        if (force || view.transfer != view_.transfer) {
            transferChip_->setText(transferText(view.transfer));
            transferChip_->setTone(toneForTransfer(view.transfer));
        }
        setShown(transferChip_, known);
    }

    // State badge: label from the view model, tone/spinner from the state.
    if (stateChip_) {
        const std::wstring label = view.stateLabel.empty() ? std::wstring(toString(view.state)) : view.stateLabel;
        const std::wstring oldLabel = view_.stateLabel.empty() ? std::wstring(toString(view_.state)) : view_.stateLabel;
        if (force || label != oldLabel) { stateChip_->setText(label); }
        if (force || view.state != view_.state) {
            stateChip_->setTone(toneForState(view.state));
            stateChip_->setSpinner(stateSpins(view.state));
        }
        if (force || view.stateReason != view_.stateReason) { stateChip_->setTooltipText(view.stateReason); }
    }

    setLabelText(subtitle_, view.subtitle);

    // Reason line: red for Failed, amber for Held; the last mkvmerge line as tooltip.
    if (reason_) {
        const bool failed = view.state == JobState::Failed;
        const bool held = view.state == JobState::Held || view.held;
        const std::wstring text = !view.stateReason.empty() ? view.stateReason : view.lastError;
        const bool show = (failed || held) && !text.empty();
        setLabelText(reason_, text);
        if (force || failed != (view_.state == JobState::Failed)) {
            reason_->setTone(failed ? LabelTone::Destructive : LabelTone::Warning);
        }
        if (force || view.lastError != view_.lastError) { reason_->setTooltipText(view.lastError); }
        setShown(reason_, show);
    }
}

/**
 * @brief Progress row: determinate spring fill or indeterminate shimmer.
 */
void JobCard::applyProgress(const JobView& view, bool force) {
    if (!progressRow_ || !progress_) { return; }
    setShown(progressRow_, view.showProgress);
    if (!view.showProgress) { return; }

    const bool indeterminate = view.progress < 0.0f;
    if (force || indeterminate != progress_->indeterminate()) { progress_->setIndeterminate(indeterminate); }

    // Tone follows the outcome: green once done, red when failed.
    ProgressTone tone = ProgressTone::Accent;
    if (view.state == JobState::Done) { tone = ProgressTone::Success; }
    else if (view.state == JobState::Failed) { tone = ProgressTone::Destructive; }
    else if (view.state == JobState::Held) { tone = ProgressTone::Warning; }
    progress_->setTone(tone);

    if (indeterminate) {
        setShown(percent_, false);
        return;
    }
    const float clamped = std::clamp(view.progress, 0.0f, 1.0f);
    // Animate only when we are on screen; the first snapshot snaps into place.
    progress_->setProgress(clamped, !force && attached());
    const int pct = static_cast<int>(std::lround(clamped * 100.0f));
    setLabelText(percent_, std::format(L"{}%", pct));
    setShown(percent_, true);
}

/**
 * @brief Preset / LUT pop-ups: items from the view model, value from the job.
 */
void JobCard::applyPlan(const JobView& view, bool force) {
    if (preset_) {
        // Presets depend on the transfer; only rebuild when the list changed.
        const std::vector<Choice> choices = vm_.presetChoices(view.transfer);
        if (force || !sameItems(preset_->items(), choices)) { preset_->setItems(toItems(choices)); }
        if (force || view.presetId != view_.presetId || preset_->selectedValue() != view.presetId) {
            if (!preset_->setSelectedValue(view.presetId)) { preset_->setSelectedIndex(-1); }
        }
        setEnabledIf(preset_, view.canEditPlan);
    }
    if (lut_) {
        const std::vector<Choice> choices = vm_.lutChoices();
        if (force || !sameItems(lut_->items(), choices)) { lut_->setItems(toItems(choices)); }
        if (force || view.lutPath != view_.lutPath || lut_->selectedValue() != view.lutPath) {
            // An empty path maps to the "None" entry (empty value) when present.
            if (!lut_->setSelectedValue(view.lutPath)) { lut_->setSelectedIndex(-1); }
        }
        setEnabledIf(lut_, view.canEditPlan);
    }
    if (attach_) {
        if (force || attach_->isOn() != view.attachLut) { attach_->setOn(view.attachLut, !force && attached()); }
        setEnabledIf(attach_, view.canEditPlan);
    }
    setEnabledIf(attachLabel_, view.canEditPlan);
}

/**
 * @brief Action buttons: visibility, labels and enabled state.
 */
void JobCard::applyActions(const JobView& view, [[maybe_unused]] bool force) {
    // Reveal is always present; it only enables once something exists on disk.
    if (reveal_) {
        setEnabledIf(reveal_, view.canReveal);
        reveal_->setTooltipText(view.canRevealHint ? L"Show the hint file in Explorer" : L"Show the export in Explorer");
    }
    setEnabledIf(remove_, view.canRemove);

    // Hold/Resume: shown while the job can be held or is currently held. When
    // the primary button already reads "Resume" we do not show it twice.
    if (hold_) {
        const std::wstring runLabel = view.runLabel.empty() ? std::wstring(L"Run") : view.runLabel;
        const bool duplicateResume = view.held && view.canRun && runLabel == L"Resume";
        const bool showHold = (view.canHold || view.held) && !duplicateResume;
        setButtonText(hold_, view.held ? L"Resume" : L"Hold");
        setShown(hold_, showHold);
    }
    if (run_) {
        setButtonText(run_, view.runLabel.empty() ? std::wstring(L"Run") : view.runLabel);
        setShown(run_, view.canRun);
    }
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

/**
 * @brief Opens the right-click menu in the popup window at @p rootPt.
 *
 * The menu callbacks capture the view model and the JobId, never the card,
 * so a row that disappears while the menu is open cannot dangle.
 */
void JobCard::showContextMenu(Point rootPt) {
    RootView* r = root();
    if (!r) {
        HH_LOG_WARN(L"JobCard", L"context menu requested while detached (job {})", id_);
        return;
    }

    // Actions are identified by short tokens so the selection handler stays flat.
    std::vector<PopupItem> items;
    std::vector<std::wstring> actions;
    auto push = [&](std::wstring label, const wchar_t* action, bool enabled, bool destructive, IconId icon) {
        PopupItem it;
        it.label = std::move(label);
        it.value = action;
        it.enabled = enabled;
        it.destructive = destructive;
        it.icon = icon;
        items.push_back(std::move(it));
        actions.emplace_back(action);
    };

    if (view_.canRun) {
        push(view_.runLabel.empty() ? std::wstring(L"Run") : view_.runLabel, L"run", true, false, IconId::Play);
    }
    if (view_.canHold || view_.held) {
        push(view_.held ? L"Resume" : L"Hold", view_.held ? L"resume" : L"hold", true, false,
             view_.held ? IconId::Play : IconId::Pause);
    }
    push(L"Reveal output", L"reveal", view_.canReveal, false, IconId::Folder);
    if (view_.canRevealHint) { push(L"Reveal hint file", L"revealHint", true, false, IconId::Eye); }
    if (!view_.mkvmergeCommand.empty()) { push(L"Copy mkvmerge command", L"copy", true, false, IconId::Copy); }

    // Separator before the destructive entry.
    if (!items.empty()) { items.back().separatorAfter = true; }
    push(L"Remove", L"remove", view_.canRemove, true, IconId::Trash);

    auto menu = std::make_unique<PopupMenu>(std::move(items), -1);
    IQueueViewModel* vm = &vm_;
    const JobId id = id_;
    menu->onSelect = [vm, id, actions](int index) {
        if (!vm || index < 0 || static_cast<size_t>(index) >= actions.size()) { return; }
        const std::wstring& action = actions[static_cast<size_t>(index)];
        if (action == L"run") { vm->run(id); }
        else if (action == L"hold") { vm->hold(id); }
        else if (action == L"resume") { vm->resume(id); }
        else if (action == L"reveal") { vm->reveal(id, false); }
        else if (action == L"revealHint") { vm->reveal(id, true); }
        else if (action == L"copy") { vm->copyCommand(id); }
        else if (action == L"remove") { vm->remove(id); }
    };

    // A zero-size anchor at the pointer; the popup window places itself there.
    r->overlay().showPopup(std::move(menu), Rect{rootPt.x, rootPt.y, 0.0f, 0.0f}, PopupPlacement::AtPoint, {});
}

} // namespace hh::ui
