// ---------------------------------------------------------------------------
// Spinner.h - rotating arc, ticked at ~24 fps (not at monitor refresh).
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

namespace hh::ui {

class Spinner : public Widget {
public:
    explicit Spinner(float size = 16.0f);
    void setSize(float size) { size_ = size; invalidateLayout(); }
    void setUseAccent(bool accent) { accent_ = accent; invalidate(); }

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    void onFrame(double now) override;
    void onAttached() override;
    void onDetached() override;

private:
    float size_ = 16.0f;
    bool accent_ = true;
    float angle_ = 0.0f;
    double lastTick_ = 0.0;
};

} // namespace hh::ui
