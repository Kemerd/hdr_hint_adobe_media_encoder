// ---------------------------------------------------------------------------
// Toast.h - transient notifications stacked at the bottom of the window.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <functional>
#include <string>

namespace hh::ui {

enum class ToastTone { Info, Success, Warning, Error };

struct ToastSpec {
    std::wstring text;
    ToastTone tone = ToastTone::Info;
    std::wstring actionLabel;              ///< optional button
    std::function<void()> onAction;
    double durationSec = 4.0;              ///< 0 = sticky until dismissed
};

class ToastView;

class ToastHost : public Widget {
public:
    ToastHost();

    void show(ToastSpec spec);
    /// Dismisses the newest toast (Escape).
    bool dismissTop();
    [[nodiscard]] bool hasToasts() const noexcept { return !children().empty(); }
    /// Distance from the bottom edge (keeps toasts above the footer).
    void setBottomInset(float inset) { bottomInset_ = inset; invalidateLayout(); }

    Size measure(const Constraints& c) override;
    void layout(const Rect& frame) override;
    Widget* hitTest(Point local) override;

private:
    friend class ToastView;
    void remove(ToastView* view);
    float bottomInset_ = 56.0f;
    static constexpr int kMaxVisible = 3;
};

} // namespace hh::ui
