// ---------------------------------------------------------------------------
// ProgressBar.h - determinate (spring fill) and indeterminate (shimmer).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

namespace hh::ui {

enum class ProgressTone { Accent, Success, Warning, Destructive };

class ProgressBar : public Widget {
public:
    ProgressBar();

    void setProgress(float value, bool animated = true);   ///< 0..1
    [[nodiscard]] float progress() const noexcept { return fill_.target(); }
    void setIndeterminate(bool on);
    [[nodiscard]] bool indeterminate() const noexcept { return indeterminate_; }
    void setTone(ProgressTone tone) { tone_ = tone; invalidate(); }
    void setThick(bool thick) { thick_ = thick; invalidateLayout(); }

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    void onFrame(double now) override;

private:
    Animatable<float> fill_{0.0f};
    bool indeterminate_ = false;
    bool thick_ = false;
    ProgressTone tone_ = ProgressTone::Accent;
    double phase_ = 0.0;
};

} // namespace hh::ui
