// ---------------------------------------------------------------------------
// Divider.h - hairline separator.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

namespace hh::ui {

class Divider : public Widget {
public:
    explicit Divider(bool vertical = false, float inset = 0.0f) : vertical_(vertical), inset_(inset) {}
    void setStrong(bool strong) { strong_ = strong; invalidate(); }
    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
private:
    bool vertical_ = false;
    float inset_ = 0.0f;
    bool strong_ = false;
};

} // namespace hh::ui
