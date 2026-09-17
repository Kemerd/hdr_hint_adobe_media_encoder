// ---------------------------------------------------------------------------
// JobCard.h - one queue row: name + transfer/state chips, subtitle, progress,
// preset/LUT pop-ups and the action row (Attach LUT, Reveal, Remove, Hold, Run).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/controls/Card.h"
#include "ui/controls/Chip.h"
#include "ui/screens/ViewModels.h"

#include <string>

namespace hh::ui {

class Label;
class ProgressBar;
class PopupButton;
class Button;
class ToggleSwitch;

/**
 * @brief A Card bound to one JobView.
 *
 * The card keeps its JobId for the lifetime of the row; update() pushes a new
 * JobView into the controls without firing their change callbacks (guarded by
 * updating_), so the queue can refresh on every progress tick cheaply.
 */
class JobCard : public Card {
public:
    /// Builds the row for @p view; @p vm must outlive the card.
    JobCard(IQueueViewModel& vm, const JobView& view);
    ~JobCard() override;

    /// Applies a fresh snapshot of the job (same JobId).
    void update(const JobView& view);
    [[nodiscard]] JobId jobId() const noexcept { return id_; }
    [[nodiscard]] const JobView& view() const noexcept { return view_; }

    /// Opens the context menu (Run/Hold/Reveal/Copy/Remove) at a root point.
    void showContextMenu(Point rootPt);

    /// Chip tone for a job state.
    [[nodiscard]] static ChipTone toneForState(JobState state) noexcept;
    /// True for states that show a spinner in the state chip.
    [[nodiscard]] static bool stateSpins(JobState state) noexcept;
    /// Chip tone for a transfer kind (Neutral for Unknown).
    [[nodiscard]] static ChipTone toneForTransfer(TransferKind transfer) noexcept;

private:
    void build();
    void applyHeader(const JobView& view, bool force);
    void applyProgress(const JobView& view, bool force);
    void applyPlan(const JobView& view, bool force);
    void applyActions(const JobView& view, bool force);

    IQueueViewModel& vm_;
    JobId id_ = 0;
    JobView view_;
    bool updating_ = false;     ///< suppresses control callbacks while pushing state
    bool initialized_ = false;  ///< false until the first update() ran

    // row 1
    Label* name_ = nullptr;
    Chip* transferChip_ = nullptr;
    Chip* stateChip_ = nullptr;
    // row 2
    Label* subtitle_ = nullptr;
    Label* reason_ = nullptr;
    // row 3
    Widget* progressRow_ = nullptr;
    ProgressBar* progress_ = nullptr;
    Label* percent_ = nullptr;
    // row 4
    Widget* planRow_ = nullptr;
    PopupButton* preset_ = nullptr;
    PopupButton* lut_ = nullptr;
    // row 5
    Widget* actionRow_ = nullptr;
    Label* attachLabel_ = nullptr;
    ToggleSwitch* attach_ = nullptr;
    Button* reveal_ = nullptr;
    Button* remove_ = nullptr;
    Button* hold_ = nullptr;
    Button* run_ = nullptr;
};

} // namespace hh::ui
