// ---------------------------------------------------------------------------
// EmptyState.h - centred illustration + title + subtitle for empty lists.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <string>

namespace hh::ui {

class EmptyState : public Widget {
public:
    EmptyState(std::wstring title, std::wstring subtitle, IconId icon = IconId::FilmFrame);
    void setTitle(std::wstring title);
    void setSubtitle(std::wstring subtitle);
    /// Slow pulse of the status dot (stops after 30 s to save power).
    void setPulsing(bool on);

    Size measure(const Constraints& c) override;
    void onLayout() override;
    void paintSelf(Canvas& c) override;
    void onFrame(double now) override;

private:
    Widget* title_ = nullptr;
    Widget* subtitle_ = nullptr;
    IconId icon_ = IconId::FilmFrame;
    bool pulsing_ = false;
    double pulseStart_ = 0.0;
    float pulse_ = 1.0f;
};

} // namespace hh::ui
