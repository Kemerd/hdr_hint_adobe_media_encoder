// ---------------------------------------------------------------------------
// QueueScreen.h - the Queue tab: a scrolling list of JobCards (or the empty
// state) above a hairline and the QueueFooter.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"
#include "ui/screens/ViewModels.h"

#include <memory>
#include <optional>
#include <vector>

namespace hh::ui {

class ScrollView;
class EmptyState;
class JobCard;
class QueueFooter;

/**
 * @brief The Queue tab content.
 *
 * refresh() diffs the view model's jobs against the existing cards by JobId:
 * new jobs slide in at their position, removed jobs shrink out, everything
 * else is updated in place. The screen also owns the single-card selection
 * used by the Delete / Ctrl+R shortcuts.
 */
class QueueScreen : public Widget {
public:
    /// Binds the screen to @p vm (must outlive the screen) and subscribes to vm.onChanged.
    explicit QueueScreen(IQueueViewModel& vm);
    ~QueueScreen() override;

    /// Re-reads vm.jobs() and reconciles the card list.
    void refresh();

    // ---- selection ----------------------------------------------------------
    [[nodiscard]] std::optional<JobId> selectedId() const noexcept { return selectedId_; }
    [[nodiscard]] JobCard* selectedCard() const;
    /// Selects the card for @p id (no-op when absent).
    void select(JobId id);
    void clearSelection();
    [[nodiscard]] bool hasSelection() const noexcept { return selectedId_.has_value(); }

    // ---- shortcuts (also reachable from the shell) --------------------------
    /// Removes the selected job when it may be removed.
    void removeSelected();
    /// Runs the selected job, or the first runnable one when nothing is selected.
    void runSelectedOrFirst();

    // ---- lookup -------------------------------------------------------------
    [[nodiscard]] JobCard* cardFor(JobId id) const;
    /// Number of live cards (cards animating out are not counted).
    [[nodiscard]] size_t cardCount() const;
    [[nodiscard]] QueueFooter* footer() const noexcept { return footer_; }
    [[nodiscard]] ScrollView* scrollView() const noexcept { return scroll_; }

    /// Delete removes, Ctrl+R runs, Escape clears the selection.
    bool onKeyDown(const KeyEvent& e) override;

private:
    JobCard* insertCard(size_t index, const JobView& view);
    void retireCard(JobCard* card);
    void updateEmptyState();
    void purgeGraveyard();
    [[nodiscard]] std::optional<size_t> indexOfCard(const JobCard* card) const;

    IQueueViewModel& vm_;
    ScrollView* scroll_ = nullptr;
    Widget* list_ = nullptr;
    EmptyState* empty_ = nullptr;
    QueueFooter* footer_ = nullptr;
    std::optional<JobId> selectedId_;
    /// Cards whose exit animation finished; destroyed on the next refresh so a
    /// card is never deleted from inside its own animation callback.
    std::vector<std::unique_ptr<Widget>> graveyard_;
};

} // namespace hh::ui
