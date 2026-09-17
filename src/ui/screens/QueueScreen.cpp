// ---------------------------------------------------------------------------
// QueueScreen.cpp - job list reconciliation, selection and shortcuts.
// ---------------------------------------------------------------------------
#include "ui/screens/QueueScreen.h"

#include "core/Logger.h"
#include "ui/controls/Divider.h"
#include "ui/controls/EmptyState.h"
#include "ui/controls/ScrollView.h"
#include "ui/screens/JobCard.h"
#include "ui/screens/QueueFooter.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace hh::ui {

namespace {

/// Gutter around the card list inside the scroll view.
constexpr float kListInset = 12.0f;
/// Extra bottom room so the last card clears the footer and toasts.
constexpr float kListBottomExtra = 8.0f;
/// Vertical gap between cards.
constexpr float kCardSpacing = 10.0f;

/// Downcasts a child to a JobCard (nullptr for anything else).
JobCard* asCard(const std::unique_ptr<Widget>& child) noexcept {
    return dynamic_cast<JobCard*>(child.get());
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/**
 * @brief Builds scroll view + list, empty state, hairline and footer.
 */
QueueScreen::QueueScreen(IQueueViewModel& vm) : vm_(vm) {
    stack().axis = Axis::Vertical;
    stack().crossAlign = CrossAlign::Stretch;

    // The scrolling list takes every dip the footer leaves over.
    scroll_ = add(std::make_unique<ScrollView>());
    if (scroll_) {
        scroll_->layoutParams().flexGrow = 1.0f;
        scroll_->setContentInsets(Insets{kListInset, kListInset, kListInset + kListBottomExtra, kListInset});
        list_ = scroll_->setContentAs(std::make_unique<Widget>());
        if (list_) {
            list_->stack().axis = Axis::Vertical;
            list_->stack().spacing = kCardSpacing;
            list_->stack().crossAlign = CrossAlign::Stretch;
        }
    } else {
        HH_LOG_ERROR(L"QueueScreen", L"failed to create the scroll view");
    }

    // The empty state replaces the scroll view (same flex slot) when idle.
    empty_ = add(std::make_unique<EmptyState>(
        L"Waiting for Media Encoder",
        L"Finished exports appear here automatically. Drop a file to process it by hand."));
    if (empty_) {
        empty_->layoutParams().flexGrow = 1.0f;
        empty_->setVisible(false);
    }

    add(std::make_unique<Divider>());
    footer_ = add(std::make_unique<QueueFooter>(vm_));
    if (footer_) { footer_->layoutParams().height = QueueFooter::kHeight; }

    // Every model change flows through refresh().
    vm_.onChanged = [this] { refresh(); };
    refresh();
}

/**
 * @brief Unsubscribes from the model; the cards die with the widget tree.
 */
QueueScreen::~QueueScreen() {
    vm_.onChanged = nullptr;
    graveyard_.clear();
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

/**
 * @brief Diffs vm.jobs() against the cards and animates the difference.
 */
void QueueScreen::refresh() {
    if (!list_) {
        HH_LOG_WARN(L"QueueScreen", L"refresh() without a list container");
        return;
    }
    purgeGraveyard();

    const std::vector<JobView> views = vm_.jobs();
    HH_LOG_TRACE(L"QueueScreen", L"refresh: {} job(s), {} card(s)", views.size(), list_->children().size());

    // Walk the model order, keeping a cursor into the child list. Cards that
    // are animating out still occupy a slot but are never matched.
    size_t pos = 0;
    for (const JobView& view : views) {
        JobCard* card = cardFor(view.id);
        if (!card) {
            pos = std::min(pos, list_->children().size());
            if (insertCard(pos, view)) { ++pos; }
            continue;
        }

        const std::optional<size_t> idx = indexOfCard(card);
        if (!idx) {
            // Should not happen (cardFor found it), but never trust a stale pointer.
            HH_LOG_WARN(L"QueueScreen", L"card for job {} vanished during refresh", view.id);
            continue;
        }
        if (*idx >= pos) {
            // Already in order: update in place and advance past it.
            card->update(view);
            pos = *idx + 1;
        } else {
            // The model reordered this job: move the card behind the cursor.
            std::unique_ptr<Widget> owned = list_->removeChild(card);
            if (!owned) {
                HH_LOG_WARN(L"QueueScreen", L"could not detach card for job {}", view.id);
                continue;
            }
            // Removing an earlier element shifts the cursor back by one.
            const size_t target = std::min(pos > 0 ? pos - 1 : 0, list_->children().size());
            list_->insertChild(target, std::move(owned));
            card->update(view);
            pos = target + 1;
        }
    }

    // Anything left that the model no longer lists gets animated out.
    std::vector<JobCard*> gone;
    for (const std::unique_ptr<Widget>& child : list_->children()) {
        JobCard* card = asCard(child);
        if (!card || card->removing()) { continue; }
        const JobId id = card->jobId();
        const bool listed = std::any_of(views.begin(), views.end(), [id](const JobView& v) { return v.id == id; });
        if (!listed) { gone.push_back(card); }
    }
    for (JobCard* card : gone) { retireCard(card); }

    updateEmptyState();
    if (footer_) { footer_->refresh(); }
}

/**
 * @brief Creates a card for @p view at @p index and slides it in.
 */
JobCard* QueueScreen::insertCard(size_t index, const JobView& view) {
    if (!list_) { return nullptr; }
    auto fresh = std::make_unique<JobCard>(vm_, view);
    JobCard* card = fresh.get();
    if (!card) { return nullptr; }

    // Clicking the card body selects it (controls inside consume their own clicks).
    const JobId id = view.id;
    card->onClick = [this, id](const MouseEvent&) { select(id); };
    card->setSelected(selectedId_.has_value() && *selectedId_ == id);

    const size_t clamped = std::min(index, list_->children().size());
    list_->insertChild(clamped, std::move(fresh));
    card->animateIn();
    return card;
}

/**
 * @brief Shrinks a card out and parks it in the graveyard once done.
 */
void QueueScreen::retireCard(JobCard* card) {
    if (!card) { return; }
    if (selectedId_.has_value() && *selectedId_ == card->jobId()) { selectedId_.reset(); }
    card->setSelected(false);

    card->animateOut([this, card] {
        if (!list_) { return; }
        std::unique_ptr<Widget> owned = list_->removeChild(card);
        if (owned) { graveyard_.push_back(std::move(owned)); }
        updateEmptyState();
    });
}

/**
 * @brief Swaps between the list and the empty state.
 */
void QueueScreen::updateEmptyState() {
    const bool hasCards = list_ && !list_->children().empty();
    if (scroll_ && scroll_->visible() != hasCards) { scroll_->setVisible(hasCards); }
    if (empty_) {
        if (empty_->visible() == hasCards) {
            empty_->setVisible(!hasCards);
            empty_->setPulsing(!hasCards);
        }
    }
}

/**
 * @brief Destroys cards whose exit animation finished earlier.
 */
void QueueScreen::purgeGraveyard() {
    if (graveyard_.empty()) { return; }
    graveyard_.clear();
}

/**
 * @brief Index of @p card inside the list container.
 */
std::optional<size_t> QueueScreen::indexOfCard(const JobCard* card) const {
    if (!list_ || !card) { return std::nullopt; }
    const auto& kids = list_->children();
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].get() == card) { return i; }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

/**
 * @brief Live (not exiting) card for a job id.
 */
JobCard* QueueScreen::cardFor(JobId id) const {
    if (!list_) { return nullptr; }
    for (const std::unique_ptr<Widget>& child : list_->children()) {
        JobCard* card = asCard(child);
        if (card && !card->removing() && card->jobId() == id) { return card; }
    }
    return nullptr;
}

/**
 * @brief Number of live cards.
 */
size_t QueueScreen::cardCount() const {
    if (!list_) { return 0; }
    size_t n = 0;
    for (const std::unique_ptr<Widget>& child : list_->children()) {
        JobCard* card = asCard(child);
        if (card && !card->removing()) { ++n; }
    }
    return n;
}

/**
 * @brief The selected card, if the selection still exists.
 */
JobCard* QueueScreen::selectedCard() const {
    if (!selectedId_) { return nullptr; }
    return cardFor(*selectedId_);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

/**
 * @brief Single selection: highlights @p id and clears every other card.
 */
void QueueScreen::select(JobId id) {
    if (!list_) { return; }
    if (!cardFor(id)) {
        HH_LOG_DEBUG(L"QueueScreen", L"select({}) ignored: no such card", id);
        return;
    }
    selectedId_ = id;
    for (const std::unique_ptr<Widget>& child : list_->children()) {
        if (JobCard* card = asCard(child)) { card->setSelected(card->jobId() == id); }
    }
}

/**
 * @brief Drops the selection highlight.
 */
void QueueScreen::clearSelection() {
    selectedId_.reset();
    if (!list_) { return; }
    for (const std::unique_ptr<Widget>& child : list_->children()) {
        if (JobCard* card = asCard(child)) {
            if (card->selected()) { card->setSelected(false); }
        }
    }
}

// ---------------------------------------------------------------------------
// Shortcuts
// ---------------------------------------------------------------------------

/**
 * @brief Delete key: removes the selected job when the model allows it.
 */
void QueueScreen::removeSelected() {
    JobCard* card = selectedCard();
    if (!card) { return; }
    if (!card->view().canRemove) {
        HH_LOG_DEBUG(L"QueueScreen", L"job {} cannot be removed right now", card->jobId());
        return;
    }
    vm_.remove(card->jobId());
}

/**
 * @brief Ctrl+R: runs the selection, else the first job that can run.
 */
void QueueScreen::runSelectedOrFirst() {
    if (JobCard* selected = selectedCard()) {
        if (selected->view().canRun) {
            vm_.run(selected->jobId());
            return;
        }
    }
    if (!list_) { return; }
    for (const std::unique_ptr<Widget>& child : list_->children()) {
        JobCard* card = asCard(child);
        if (card && !card->removing() && card->view().canRun) {
            vm_.run(card->jobId());
            return;
        }
    }
    HH_LOG_DEBUG(L"QueueScreen", L"Ctrl+R: nothing runnable");
}

/**
 * @brief Keyboard handling for the list (invoked by the shell as well).
 */
bool QueueScreen::onKeyDown(const KeyEvent& e) {
    const bool plain = !e.mods.ctrl && !e.mods.shift && !e.mods.alt;
    // Delete removes the selected job.
    if (e.vk == VK_DELETE && plain) {
        if (!hasSelection()) { return false; }
        removeSelected();
        return true;
    }
    // Ctrl+R runs the selected (or first runnable) job.
    if (e.vk == 'R' && e.mods.ctrl && !e.mods.shift && !e.mods.alt) {
        runSelectedOrFirst();
        return true;
    }
    // Escape clears the selection when there is one.
    if (e.vk == VK_ESCAPE && plain) {
        if (!hasSelection()) { return false; }
        clearSelection();
        return true;
    }
    return false;
}

} // namespace hh::ui
