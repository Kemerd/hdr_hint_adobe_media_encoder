// ---------------------------------------------------------------------------
// StatusDot.h - 8 dip status dot with a pulsing halo when linked.
// ---------------------------------------------------------------------------
#pragma once

#include "ui/core/Widget.h"

#include <string>

namespace hh::ui {

enum class LinkStatus { Offline, Watching, Linked, Warning };

class StatusDot : public Widget {
public:
    StatusDot();
    void setStatus(LinkStatus status);
    [[nodiscard]] LinkStatus status() const noexcept { return status_; }
    void setTooltipText(std::wstring tip) { tooltip_ = std::move(tip); }
    /// Optional label to the right of the dot ("AME linked").
    void setLabel(std::wstring label);

    Size measure(const Constraints& c) override;
    void paintSelf(Canvas& c) override;
    void onFrame(double now) override;
    [[nodiscard]] std::wstring tooltip() const override { return tooltip_; }
    [[nodiscard]] bool interactive() const override { return true; }

private:
    LinkStatus status_ = LinkStatus::Offline;
    std::wstring tooltip_;
    std::wstring label_;
    double haloStart_ = 0.0;
    float halo_ = 0.0f;
};

} // namespace hh::ui
