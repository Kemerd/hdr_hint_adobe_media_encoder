// ---------------------------------------------------------------------------
// QueueFooter.h - the 44 dip strip under the queue list: Auto-process toggle,
// optional "Dock inside AME" toggle and the "3 jobs · 1 done" summary.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"
#include "ui/screens/ViewModels.h"

#include <functional>

namespace hh::ui {

class Label;
class ToggleSwitch;

/**
 * @brief Footer strip of the Queue tab.
 *
 * Horizontal stack: "Auto-process" label + switch, an optional dock label +
 * switch (floating window only), a flexible spacer and the summary caption
 * on the right. Everything is bound to IQueueViewModel; refresh() re-reads.
 */
class QueueFooter : public Widget {
public:
    /// Binds the footer to @p vm (must outlive the footer).
    explicit QueueFooter(IQueueViewModel& vm);
    ~QueueFooter() override;

    /// Re-reads autoProcess() and footerSummary() from the view model.
    void refresh();

    /**
     * @brief Configures the optional "Dock inside AME" switch.
     * @param onToggle  called with the new state when the user flips it
     * @param visible   show or hide the label + switch
     * @param state     current switch position (applied without firing onToggle)
     */
    void setDockToggle(std::function<void(bool)> onToggle, bool visible, bool state);

    Size measure(const Constraints& c) override;

    static constexpr float kHeight = 44.0f;

private:
    IQueueViewModel& vm_;
    Label* autoLabel_ = nullptr;
    ToggleSwitch* autoSwitch_ = nullptr;
    Label* dockLabel_ = nullptr;
    ToggleSwitch* dockSwitch_ = nullptr;
    Label* summary_ = nullptr;
    std::function<void(bool)> onDockToggle_;
    bool dockVisible_ = false;   ///< requested visibility (the label may still hide when narrow)
    bool updating_ = false;      ///< suppresses onChanged while we push state into the switches
};

} // namespace hh::ui
